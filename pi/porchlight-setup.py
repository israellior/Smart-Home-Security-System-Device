#!/usr/bin/env python3
"""Gets a doorbell onto a network it has never heard of, from a phone.

Runs at every boot, before porchlightd. If the device is already on a network
it exits at once and is never noticed. If it is not - a new device out of its
box, or one whose router has been replaced - it becomes an access point of its
own, serves a page, takes an SSID and a password, and joins.

    boot
     |
     +- something is connected? --------------------- yes -> exit 0
     |
     +- scan for networks            (before the AP: one radio, see below)
     +- raise the access point       (WPA2, 192.168.4.1)
     +- serve the page               (and answer captive-portal probes)
     |
     +- POST /wifi -> drop the AP, join, verify in stages
            success -> exit 0, systemd starts porchlightd
            failure -> raise the AP again, and remember why

Three things about this are not obvious and each one is a bug if missed.

**Scan before the access point.** One radio cannot both be an access point and
look for networks. The list of SSIDs has to be captured while the device is
still a client and cached for the page, or the page has nothing to offer.

**The phone loses us the moment we try.** Joining means dropping the access
point the phone is connected to, so the POST cannot report its own result -
nobody is listening by then. It answers immediately, and the outcome is written
down so the page can show it if the customer comes back. The LED is what speaks
in between, and on success there is nothing to come back to: the device is on
the network and the app says so.

**Three failures look identical from the sofa.** A wrong password, a network
with no internet, and a network that cannot reach Porchlight are three
different things for a customer to do something about, so the join is verified
in stages - link, then DNS, then the app server - and the page says which
stage failed rather than "could not connect".

    ./porchlight-setup.py --check                  # what would it do, now
    ./porchlight-setup.py --fake-nm --port 8080    # the whole flow, no radio
    ./porchlight-setup.py                          # for real, as root
"""

import argparse
import http.server
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

EXIT_OK = 0
EXIT_FAILED = 1

AP_CONNECTION = "porchlight-setup"
AP_ADDRESS = "192.168.4.1"
AP_PREFIX = 24

# Where NetworkManager's shared-mode dnsmasq reads extra configuration. Sending
# every name to ourselves is what makes the setup page open by itself: a phone
# joining a network immediately fetches a known URL to find out whether it has
# internet, and answering that probe with a redirect is what a captive portal
# is. Without it the customer has to be told to type an IP address.
DNSMASQ_DIR = "/etc/NetworkManager/dnsmasq-shared.d"
DNSMASQ_FILE = "porchlight-setup.conf"

# A clock this far behind means the Pi has never had NTP - it has no battery -
# and TLS will refuse a certificate that is not yet valid. That reads as a
# broken network, so it is worth naming.
PLAUSIBLE_CLOCK = 1735689600  # 2025-01-01

# If a card was never minted there is nothing to put on a sticker, so the
# access point still has to be findable. Nothing can be stolen through it: the
# device holds no credential, and the page says exactly that.
FALLBACK_SSID = "Porchlight-setup"
FALLBACK_PSK = "porchlight"


def note(text):
    sys.stderr.write(f"setup: {text}\n")
    sys.stderr.flush()


# ---------------------------------------------------------------- the light

# The two patterns that exist only here, in the same terms as the daemon's
# table in porchlightd/src/io/led_patterns.h. They never overlap with the
# daemon's: this process holds the GPIO line and porchlightd does not start
# until it has let go.
LED_WAITING = [(True, 0.5), (False, 0.5)]            # even, 1 Hz: your move
LED_JOINING = [(True, 0.1), (False, 0.1)]            # 5 Hz: working on it
# Deliberately the daemon's Fault shape - lit, with two short gaps. It means
# the same thing there and here: waiting will not fix this, a person must act.
LED_FAILED = [(True, 1.64), (False, 0.12), (True, 0.12), (False, 0.12)]


class Led:
    """The only output during setup, so it must never be the thing that fails.

    libgpiod's Python binding is the good path. Anything missing - no module,
    no chip, a line somebody else holds - is a warning and a dark LED, because
    a doorbell that will not set up because its light is broken is worse than
    one that sets up quietly.
    """

    def __init__(self, chip="/dev/gpiochip0", line=25, active_low=False, enabled=True):
        self.request = None
        self.phases = None
        self.stop = threading.Event()
        self.thread = None
        if not enabled:
            return
        try:
            import gpiod
            from gpiod.line import Direction, Value
        except ImportError:
            note("no python gpiod; the LED will stay dark through setup")
            return
        try:
            self.Value = Value
            settings = gpiod.LineSettings(direction=Direction.OUTPUT,
                                          active_low=active_low,
                                          output_value=Value.INACTIVE)
            self.request = gpiod.request_lines(chip, consumer="porchlight-setup",
                                               config={line: settings})
            self.line = line
        except Exception as error:
            note(f"cannot drive the LED on {chip} line {line}: {error}")
            self.request = None

    def show(self, phases):
        self.phases = phases
        if self.request is None or self.thread is not None:
            return
        self.thread = threading.Thread(target=self._blink, daemon=True)
        self.thread.start()

    def _blink(self):
        index = 0
        while not self.stop.is_set():
            phases = self.phases
            on, hold = phases[index % len(phases)]
            self._set(on)
            index += 1
            if self.phases is not phases:
                index = 0  # a new pattern always begins at its first phase
            self.stop.wait(hold)

    def _set(self, on):
        try:
            self.request.set_value(self.line,
                                   self.Value.ACTIVE if on else self.Value.INACTIVE)
        except Exception:
            pass  # the line went away; nothing here is worth failing for

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=1)
        if self.request is not None:
            self._set(False)  # a released line keeps its last level
            try:
                self.request.release()
            except Exception:
                pass


# ---------------------------------------------------------------- the radio

class Network:
    """nmcli, because NetworkManager is what Raspberry Pi OS uses now.

    Every method returns plain data and raises nothing the caller has to catch:
    this runs unattended on a device nobody can log into, so a surprise here is
    a doorbell that never comes up.
    """

    def __init__(self, interface=None):
        self.interface = interface or self._wifi_interface()

    def _run(self, args, timeout=60):
        try:
            done = subprocess.run(["nmcli"] + args, capture_output=True, text=True,
                                  timeout=timeout)
            return done.returncode, done.stdout.strip(), done.stderr.strip()
        except FileNotFoundError:
            return 127, "", "nmcli is not installed"
        except subprocess.TimeoutExpired:
            return 124, "", f"nmcli {' '.join(args)} timed out"

    def _wifi_interface(self):
        code, out, _ = self._run(["-t", "-f", "DEVICE,TYPE", "device", "status"])
        if code == 0:
            for row in out.splitlines():
                name, _, kind = row.partition(":")
                if kind == "wifi":
                    return name
        return "wlan0"

    def scan(self):
        """SSIDs in range, strongest first, one entry per name.

        Must be called before the access point goes up: one radio cannot be an
        access point and scan at the same time, and on this hardware the second
        request simply returns what it last saw, or nothing.
        """
        self._run(["device", "wifi", "rescan"], timeout=30)
        time.sleep(2)  # nmcli returns before the results land
        code, out, err = self._run(["-t", "-f", "SSID,SIGNAL,SECURITY",
                                    "device", "wifi", "list"])
        if code != 0:
            note(f"could not scan: {err}")
            return []
        seen = {}
        for row in out.splitlines():
            # SSIDs can contain colons, which nmcli escapes as "\:" in -t mode.
            fields = re.split(r"(?<!\\):", row)
            if len(fields) < 3 or not fields[0]:
                continue
            ssid = fields[0].replace("\\:", ":")
            try:
                signal = int(fields[1])
            except ValueError:
                signal = 0
            if ssid not in seen or signal > seen[ssid]["signal"]:
                seen[ssid] = {"ssid": ssid, "signal": signal,
                              "secure": bool(fields[2])}
        return sorted(seen.values(), key=lambda n: -n["signal"])

    def saved_wifi_profiles(self):
        """Networks this device has been told about, our own AP aside."""
        code, out, _ = self._run(["-t", "-f", "NAME,TYPE", "connection", "show"])
        if code != 0:
            return []
        names = []
        for row in out.splitlines():
            name, _, kind = row.rpartition(":")
            if kind == "802-11-wireless" and name != AP_CONNECTION:
                names.append(name)
        return names

    def active_ipv4(self):
        """An address on something that is not our own access point."""
        code, out, _ = self._run(["-t", "-f", "DEVICE,TYPE,STATE,CONNECTION",
                                  "device", "status"])
        if code != 0:
            return None
        for row in out.splitlines():
            fields = row.split(":")
            if len(fields) < 4 or fields[2] != "connected" or fields[3] == AP_CONNECTION:
                continue
            address = self._address_of(fields[0])
            if address:
                return address
        return None

    def _address_of(self, device):
        code, out, _ = self._run(["-t", "-f", "IP4.ADDRESS", "device", "show", device])
        if code != 0:
            return None
        for row in out.splitlines():
            _, _, value = row.partition(":")
            if value:
                return value.split("/")[0]
        return None

    def start_ap(self, ssid, psk):
        """A profile of our own rather than `nmcli device wifi hotspot`.

        The address matters: the dnsmasq rule, the page's own links and
        anything printed on a card all have to agree on it, and NetworkManager's
        default shared address is not something to depend on across versions.
        """
        self._write_dnsmasq()
        self._run(["connection", "delete", AP_CONNECTION])
        code, _, err = self._run([
            "connection", "add", "type", "wifi", "ifname", self.interface,
            "con-name", AP_CONNECTION, "autoconnect", "no", "ssid", ssid,
            "802-11-wireless.mode", "ap", "802-11-wireless.band", "bg",
            "ipv4.method", "shared",
            "ipv4.addresses", f"{AP_ADDRESS}/{AP_PREFIX}",
            "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", psk,
        ])
        if code != 0:
            note(f"could not define the access point: {err}")
            return False
        code, _, err = self._run(["connection", "up", AP_CONNECTION])
        if code != 0:
            note(f"could not raise the access point: {err}")
            return False
        note(f"access point {ssid} is up on {AP_ADDRESS}")
        return True

    def stop_ap(self):
        self._run(["connection", "down", AP_CONNECTION])

    def _write_dnsmasq(self):
        try:
            os.makedirs(DNSMASQ_DIR, exist_ok=True)
            with open(os.path.join(DNSMASQ_DIR, DNSMASQ_FILE), "w") as handle:
                handle.write(
                    "# Written by porchlight-setup.py. Sends every name to the\n"
                    "# device itself, which is what makes the setup page open by\n"
                    "# itself instead of the customer being told an IP address.\n"
                    f"address=/#/{AP_ADDRESS}\n")
        except OSError as error:
            # The page still works, typed by hand. Not worth stopping for.
            note(f"no captive portal redirect ({error}); the page is at {AP_ADDRESS}")

    def join(self, ssid, psk):
        """Returns None on success, or a cause the page can turn into a sentence."""
        args = ["device", "wifi", "connect", ssid, "ifname", self.interface]
        if psk:
            args += ["password", psk]
        code, _, err = self._run(args, timeout=90)
        if code == 0:
            return None
        lowered = err.lower()
        # These strings are nmcli's and they move between versions, so the
        # fallback is the one that matters: an unrecognised failure is still
        # reported, just less specifically.
        if "secrets were required" in lowered or "no secrets" in lowered:
            return "wifi-auth"
        if "no network with ssid" in lowered or "not found" in lowered:
            return "wifi-missing"
        if code == 124:
            return "wifi-timeout"
        note(f"join failed: {err}")
        return "wifi-failed"

    def forget(self, ssid):
        self._run(["connection", "delete", ssid])


class FakeNetwork:
    """A radio for a machine that has none, so the whole flow can be driven.

    Everything above talks to hardware and cannot run on the development host;
    everything that decides what to do can, and this is the seam between them.
    A password of "wrong" fails authentication and "nointernet" joins a network
    that goes nowhere, so both halves of the page's error handling are
    reachable without a Pi.
    """

    def __init__(self, interface="wlan0"):
        self.interface = interface
        self.ap_up = False
        self.joined = None
        self.saved = []

    def scan(self):
        return [
            {"ssid": "Wombat", "signal": 88, "secure": True},
            {"ssid": "BT-HOMEHUB-9F2A", "signal": 61, "secure": True},
            {"ssid": "virgin-guest", "signal": 40, "secure": False},
        ]

    def saved_wifi_profiles(self):
        return list(self.saved)

    def active_ipv4(self):
        return "192.168.1.50" if self.joined else None

    def start_ap(self, ssid, psk):
        self.ap_up = True
        note(f"[fake] access point {ssid} up, psk {psk}")
        return True

    def stop_ap(self):
        self.ap_up = False
        note("[fake] access point down")

    def join(self, ssid, psk):
        if psk == "wrong":
            return "wifi-auth"
        self.joined = ssid
        self.saved.append(ssid)
        note(f"[fake] joined {ssid}")
        return None

    def forget(self, ssid):
        pass


# ------------------------------------------------------------ does it work

def check_link(network):
    return "no-ip" if not network.active_ipv4() else None


def check_dns(host):
    try:
        socket.getaddrinfo(host, None)
        return None
    except socket.gaierror:
        return "no-dns"


def check_server(base_url, device_id, credential, timeout=20):
    """The only check that proves the network is useful rather than present."""
    url = f"{base_url.rstrip('/')}/api/devices/{device_id}/self"
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {credential}"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            json.loads(response.read())
        return None
    except urllib.error.HTTPError as error:
        # The network is fine; we are not. A customer cannot fix this and
        # should not be told to try another password.
        return "credential" if error.code in (401, 403) else "server-error"
    except urllib.error.URLError as error:
        if isinstance(error.reason, ssl.SSLCertVerificationError):
            # Almost always the clock: a Pi has no battery-backed one, and a
            # certificate that is not yet valid reads as a network fault.
            if time.time() < PLAUSIBLE_CLOCK:
                return "clock"
            return "tls"
        return "no-server"
    except (ValueError, socket.timeout):
        return "no-server"


def verify(network, base_url, device_id, credential, fake=False):
    """Link, then names, then the app server. In that order, and stop at the
    first that fails - anything after it would fail for the same reason and
    report the wrong cause."""
    if fake:
        return None
    for stage in (lambda: check_link(network),
                  lambda: check_dns(urllib.parse.urlparse(base_url).hostname or ""),
                  lambda: check_server(base_url, device_id, credential)):
        cause = stage()
        if cause:
            return cause
    return None


# The customer reads these, so they say what to do rather than what happened.
CAUSES = {
    "wifi-auth": "That password was not accepted. Check it and try again.",
    "wifi-missing": "That network was not there when we looked. Move the "
                    "doorbell closer to the router, or try again.",
    "wifi-timeout": "Joining took too long. The signal here may be too weak.",
    "wifi-failed": "The doorbell could not join that network.",
    "no-ip": "The doorbell joined the network but was not given an address. "
             "That is usually the router; restarting it often fixes it.",
    "no-dns": "The doorbell is on the network but cannot look up names. "
              "Check the router's DNS settings.",
    "no-server": "The doorbell is on the network, but that network has no way "
                 "out to the internet.",
    "server-error": "Porchlight answered but something is wrong at our end. "
                    "The doorbell will keep trying.",
    "credential": "This doorbell was not set up correctly at the factory and "
                  "needs to be replaced. The network is fine.",
    "clock": "The doorbell's clock is wrong, so it cannot verify a secure "
             "connection. Leave it powered on for a few minutes and try again.",
    "tls": "The secure connection to Porchlight could not be verified.",
    "unprovisioned": "This doorbell has no identity and cannot be set up. "
                     "It needs to be replaced.",
}


# ---------------------------------------------------------------- the page

PAGE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Set up your doorbell</title>
<style>
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 24px 16px 48px; font: 16px/1.5 system-ui, sans-serif;
         max-width: 30rem; margin-inline: auto; }
  h1 { font-size: 1.4rem; margin: 0 0 4px; }
  p.sub { margin: 0 0 24px; opacity: .7; }
  label { display: block; font-weight: 600; margin: 16px 0 6px; }
  select, input, button { width: 100%; padding: 12px; font-size: 16px;
                          border-radius: 10px; border: 1px solid #8884; }
  button { margin-top: 20px; font-weight: 600; border: 0; padding: 14px;
           background: #2f6fed; color: #fff; }
  button:disabled { opacity: .5; }
  .msg { padding: 12px 14px; border-radius: 10px; margin-bottom: 20px; }
  .bad { background: #d32f2f18; border: 1px solid #d32f2f66; }
  .ok  { background: #2e7d3218; border: 1px solid #2e7d3266; }
  .code { font: 600 1.3rem/1 ui-monospace, monospace; letter-spacing: .12em; }
  footer { margin-top: 32px; font-size: .85rem; opacity: .6; }
</style>
</head><body>
<h1>__NAME__</h1>
<p class="sub">Tell this doorbell how to reach your wi-fi.</p>
__MESSAGE__
<form method="POST" action="/wifi" id="f">
  <label for="ssid">Your network</label>
  <select name="ssid" id="ssid">__OPTIONS__</select>
  <label for="psk">Password</label>
  <input type="password" name="psk" id="psk" autocomplete="off"
         autocapitalize="none" autocorrect="off">
  <button type="submit" id="go">Connect</button>
</form>
<p class="sub" style="margin-top:20px">
  When you press Connect this network disappears, because the doorbell needs
  its radio to join yours. Your phone will go back to its usual wi-fi on its
  own. Watch the light on the doorbell: fast flashing means it is trying.
</p>
<footer>
  Doorbell code <span class="code">__CLAIM__</span><br>
  Enter this in the Porchlight app to add it to your account.
</footer>
<script>
  document.getElementById('f').addEventListener('submit', function () {
    var go = document.getElementById('go');
    go.disabled = true; go.textContent = 'Connecting\\u2026';
  });
</script>
</body></html>
"""

SENT = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Connecting</title>
<style>body{margin:0;padding:48px 16px;font:16px/1.6 system-ui,sans-serif;
max-width:30rem;margin-inline:auto;text-align:center}</style>
</head><body>
<h1>Connecting…</h1>
<p>The doorbell is joining <b>__SSID__</b>. This network will disappear while
it tries — that is meant to happen.</p>
<p>If it works, the doorbell appears in the Porchlight app within a minute and
its light stops flashing. If it does not, this setup network comes back and
you can try again.</p>
</body></html>
"""


def render(state):
    options = "".join(
        f'<option value="{escape(n["ssid"])}">{escape(n["ssid"])}'
        f'{"" if n["secure"] else " (open)"} &nbsp; {bars(n["signal"])}</option>'
        for n in state.networks) or "<option value=''>No networks found</option>"

    message = ""
    if state.problem:
        message = f'<div class="msg bad">{escape(CAUSES.get(state.problem, state.problem))}</div>'
    elif state.tried:
        message = '<div class="msg ok">Connected.</div>'

    return (PAGE.replace("__NAME__", escape(state.name))
                .replace("__OPTIONS__", options)
                .replace("__MESSAGE__", message)
                .replace("__CLAIM__", escape(state.claim_code or "--")))


def bars(signal):
    return "▂▄▆█"[:max(1, min(4, signal // 25))]


def escape(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


# ------------------------------------------------------------ the server

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "porchlight-setup"

    # Every probe a phone makes on joining a network, and what it expects. An
    # answer that is not what it expects is what makes the portal open.
    PROBES = ("/hotspot-detect.html", "/library/test/success.html",  # Apple
              "/generate_204", "/gen_204",                            # Android
              "/ncsi.txt", "/connecttest.txt")                        # Windows

    def log_message(self, fmt, *args):
        note(f"http {self.address_string()} {fmt % args}")

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            return self.html(render(self.server.state))
        if path == "/status":
            state = self.server.state
            return self.json({"tried": state.tried, "problem": state.problem,
                              "done": state.finished.is_set()})
        if path in self.PROBES:
            return self.redirect(f"http://{AP_ADDRESS}/")
        return self.redirect(f"http://{AP_ADDRESS}/")  # anything else, too

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != "/wifi":
            return self.redirect(f"http://{AP_ADDRESS}/")
        length = int(self.headers.get("Content-Length") or 0)
        form = urllib.parse.parse_qs(self.rfile.read(length).decode("utf-8", "replace"))
        ssid = (form.get("ssid") or [""])[0]
        psk = (form.get("psk") or [""])[0]
        if not ssid:
            return self.html(render(self.server.state))

        # Answered before anything is attempted, deliberately. The attempt
        # takes the access point down, and a reply written after that reaches
        # nobody - the phone is no longer on this network to receive it.
        self.html(SENT.replace("__SSID__", escape(ssid)))
        self.server.state.attempt(ssid, psk)

    def html(self, body, code=200):
        encoded = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def json(self, payload):
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def redirect(self, where):
        self.send_response(302)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        self.end_headers()


class State:
    """What the page shows, and the one action it can take."""

    def __init__(self, options, network, identity, credential, led):
        self.options = options
        self.network = network
        self.identity = identity
        self.credential = credential
        self.led = led
        self.networks = []
        self.problem = None if credential else "unprovisioned"
        self.tried = False
        self.finished = threading.Event()
        self.busy = threading.Lock()

    @property
    def name(self):
        return self.identity.get("name") or "Porchlight doorbell"

    @property
    def claim_code(self):
        return self.identity.get("claimCode")

    def attempt(self, ssid, psk):
        thread = threading.Thread(target=self._attempt, args=(ssid, psk), daemon=True)
        thread.start()

    def _attempt(self, ssid, psk):
        if not self.busy.acquire(blocking=False):
            return  # a second Connect while the first is still going
        try:
            self.tried = True
            self.problem = None
            self.led.show(LED_JOINING)
            note(f"trying {ssid}")

            self.network.stop_ap()  # one radio: the AP has to go first
            cause = self.network.join(ssid, psk)
            if cause is None:
                cause = verify(self.network,
                               self.identity.get("url", ""),
                               self.identity.get("deviceId", ""),
                               self.credential,
                               fake=self.options.fake_nm)

            if cause is None:
                note(f"joined {ssid} and reached the app server")
                self.led.close()
                self.finished.set()
                return

            # Not kept: a profile that does not work would be retried on every
            # future boot and would stop this ever running again.
            self.network.forget(ssid)
            self.problem = cause
            note(f"failed: {cause}")
            self.led.show(LED_FAILED)
            self.raise_ap()
        finally:
            self.busy.release()

    def raise_ap(self):
        ssid = self.identity.get("setupSsid") or FALLBACK_SSID
        psk = self.identity.get("setupPassword") or FALLBACK_PSK
        return self.network.start_ap(ssid, psk)


# ---------------------------------------------------------------- the flow

def load_identity(path):
    try:
        with open(path) as handle:
            return json.load(handle)
    except (OSError, ValueError):
        note(f"no identity at {path}; this card was never minted")
        return {}


def read_credential(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return ""


def already_connected(network, options):
    """Whether to stand aside, and the one place patience matters.

    At boot NetworkManager takes a few seconds to associate, and a device that
    gave up after two would raise an access point in a house whose wi-fi is
    working - and never join it again, because the customer has no reason to
    come and tell it anything. So: if it has ever been told about a network,
    wait. If it has not, there is nothing to wait for.
    """
    if network.active_ipv4():
        return True
    if not network.saved_wifi_profiles():
        note("no saved networks; this device has never been set up")
        return False

    note(f"waiting up to {options.wait}s for a saved network")
    deadline = time.monotonic() + options.wait
    while time.monotonic() < deadline:
        time.sleep(2)
        if network.active_ipv4():
            return True
    note("saved networks, but none of them came up")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--identity", default="/etc/porchlight/identity.json")
    parser.add_argument("--credential-file", default="/etc/porchlight/credential")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--wait", type=int, default=45,
                        help="seconds to let a saved network come up (default 45)")
    parser.add_argument("--interface", default=None)
    parser.add_argument("--fake-nm", action="store_true",
                        help="a pretend radio, so the flow runs on any machine")
    parser.add_argument("--no-led", action="store_true")
    parser.add_argument("--check", action="store_true",
                        help="say what would happen now, and change nothing")
    options = parser.parse_args()

    identity = load_identity(options.identity)
    credential = read_credential(options.credential_file)
    network = FakeNetwork() if options.fake_nm else Network(options.interface)

    if options.check:
        note(f"interface {network.interface}")
        note(f"device {identity.get('deviceId', '(unminted)')}")
        note(f"saved networks: {network.saved_wifi_profiles() or 'none'}")
        note(f"address now: {network.active_ipv4() or 'none'}")
        note("would hand over to porchlightd" if network.active_ipv4()
             else "would raise the access point")
        return EXIT_OK

    if already_connected(network, options):
        note("already on a network; handing over")
        return EXIT_OK

    led = Led(line=identity.get("ledLine", 25), enabled=not options.no_led)
    state = State(options, network, identity, credential, led)

    # Before the access point, and this ordering is the whole reason the scan
    # is a separate step: the radio cannot do both.
    state.networks = network.scan()
    note(f"{len(state.networks)} network(s) in range")

    if not state.raise_ap():
        note("could not raise the access point; nothing more can be done here")
        led.close()
        return EXIT_FAILED
    led.show(LED_WAITING)

    server = http.server.ThreadingHTTPServer(("0.0.0.0", options.port), Handler)
    server.state = state
    threading.Thread(target=server.serve_forever, daemon=True).start()
    note(f"listening on {AP_ADDRESS}:{options.port}")

    try:
        state.finished.wait()
    except KeyboardInterrupt:
        note("interrupted")
        led.close()
        return EXIT_FAILED

    server.shutdown()
    led.close()
    note("setup done; porchlightd takes it from here")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
