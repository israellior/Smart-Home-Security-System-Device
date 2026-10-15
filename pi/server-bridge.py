#!/usr/bin/env python3
"""The daemon's WebSocket to the app server, as a child process.

porchlightd is C++ and C++ has no WebSocket. This Pi already runs
python3-websocket for webrtc-video.py, so the socket lives here instead of
becoming a TLS dependency in the daemon.

It is deliberately stupid. Every rule about when to alert, what to retry and
what to drop is in the daemon's core, which is tested. This connects, says
hello, and relays JSON lines both ways.

    daemon --> stdin    {"type":"event","eventId":...,"kind":...,"at":...}
    stdout --> daemon   {"type":"online"}
                        {"type":"offline","reason":...}
                        {"type":"fatal","reason":...}
                        {"type":"event-ack","eventId":...,"kind":...,"ok":...}
                        {"type":"viewer-requested","peer":7}

    bridge --> server   {"type":"link","rssi":-61}
    server --> bridge   {"type":"update","version":...,"url":...,"bytes":...,"sha256":...}
    bridge --> server   {"type":"update-status","version":...,"state":...,"progress":...}

Those three are the bridge's own and never pass through the daemon. The first
is the Wi-Fi signal, for the app to show (see read_rssi). The other two are a
software update: the bridge cannot install one - it runs as `porchlight`, not
root - so it hands the request to porchlight-update.py through /data/update and
relays what that reports. See start_update.

There is no enrolment. The credential is written onto the SD card when the
device is built; this only ever reads it.

By hand, against a live server and with no daemon involved. --check and
--probe are the two halves of a first connection and they fail differently:
--check is one HTTPS request and proves the credential, the URL and the
route; --probe opens the real socket and proves the handshake the daemon
depends on. A device that passes the first and fails the second has a proxy
or a firewall in the way that allows HTTP and not WebSocket - otherwise
diagnosed as "the daemon just sits there".

    ./server-bridge.py --url http://host:4000 --device-id porch-1 \\
        --credential-file /etc/porchlight/credential --check
    ./server-bridge.py --url http://host:4000 --device-id porch-1 \\
        --credential-file /etc/porchlight/credential --probe
    ./server-bridge.py --url http://host:4000 --device-id porch-1 \\
        --credential-file /etc/porchlight/credential
    {"type":"event","eventId":"test-1","kind":"ring","at":"2026-09-21T10:00:00.000Z"}
"""

import argparse
import configparser
import json
import os
import random
import re
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import websocket  # python3-websocket
except ImportError:  # --check is pure stdlib and must work without it
    websocket = None

# Three roles share this socket and replacement is scoped per role, so the
# daemon claiming "pi" would close the media script's socket on every
# reconnect - which looks like video randomly failing, not like an auth bug.
HELLO_ROLE = "device"

# Reconnect delays. A doorbell that has been offline for an hour should come
# back promptly, but a street of them must not return in the same second.
RECONNECT_INITIAL = 1.0
RECONNECT_MAX = 60.0

# How long the server has to answer our hello before we assume the socket is
# no longer a socket. The server drops anything that has not said hello within
# ten seconds; this is the mirror of that rule, and it exists because a TCP
# connection that is open but dead looks exactly like one that is working. A
# transparent proxy that accepts the upgrade and forwards nothing is the usual
# cause, and without this the daemon waits for it forever.
HELLO_TIMEOUT = 10.0

CLOSE_REPLACED = 4001
CLOSE_BAD_CREDENTIAL = 4002
CLOSE_NO_HELLO = 4003

# The Wi-Fi signal, for the app. Read every half minute and sent only when it
# has moved by a few dB - a signal wanders by one or two from second to second,
# and every frame fans out to every phone watching this doorbell. The server
# ignores anything faster than every five seconds.
LINK_SAMPLE_INTERVAL = 30.0
LINK_REPORT_STEP = 3
WIRELESS_STATS = "/proc/net/wireless"

# "Not yet reported on this connection", which is not the same as None: None
# is a report, and means "not on Wi-Fi".
_UNSENT = object()


# Software updates. The request and status files are the whole interface to
# the root updater - see porchlight-update.py for the other side.
UPDATE_DIR = "/data/update"
UPDATE_REQUEST = os.path.join(UPDATE_DIR, "request.json")
UPDATE_STATUS = os.path.join(UPDATE_DIR, "status.json")
UPDATER = "/usr/local/lib/porchlight/porchlight-update.py"
RAUC_STATUS = "/data/rauc/central.raucs"
RAUC_SECTIONS = {"A": "slot.rootfs.0", "B": "slot.rootfs.1"}

# How often the updater's status file is read while an update runs, and how
# long before the bridge stops watching one that never finishes. The server
# gives up on its own after 45 minutes of silence.
UPDATE_POLL = 2.0
UPDATE_WATCH_LIMIT = 60 * 60


def booted_slot():
    """A or B, from the kernel command line U-Boot wrote, or None off our image."""
    try:
        with open("/proc/cmdline") as handle:
            found = re.search(r"\brauc\.slot=([AB])\b", handle.read())
    except OSError:
        return None
    return found.group(1) if found else None


def installed_version():
    """The version of the release this system came from, as RAUC recorded it
    when it installed the bundle - the `version=` in the bundle's manifest.

    None for the system flashed at the factory, which RAUC never installed and
    so has no record of, and for any device not on our image. The server
    treats None as "behind", so a factory doorbell is brought to the latest
    release like any other."""
    slot = booted_slot()
    if slot is None:
        return None
    status = configparser.ConfigParser(interpolation=None)
    try:
        status.read(RAUC_STATUS)
    except (OSError, configparser.Error):
        return None
    return status.get(RAUC_SECTIONS[slot], "bundle.version", fallback=None) or None


def can_update():
    """Whether this device can take an update: our image (RAUC slots), the
    updater installed, and the request directory writable by us. A Raspberry
    Pi OS unit set up by provision.sh has none of these, and says so."""
    return (booted_slot() is not None and os.path.exists(UPDATER)
            and os.access(UPDATE_DIR, os.W_OK))


def hello_frame(device_id, credential):
    """Said first on every connection, by the bridge and by --probe alike.

    It carries what this device is running, so the server learns how an
    update went from the hello after the restart - including a rollback,
    which nothing on the device ever reports in so many words."""
    return {
        "type": "hello",
        "role": HELLO_ROLE,
        "deviceId": device_id,
        "token": credential,
        "version": installed_version(),
        "updater": can_update(),
    }


def close_meaning(code):
    """What a close code means to this device, in the words the docs use."""
    if code == CLOSE_BAD_CREDENTIAL:
        return ("the credential was refused. This is permanent: re-mint the device "
                "on the server and write the new credential onto the card.")
    if code == CLOSE_REPLACED:
        return ("another connection signed in as this device. Something else is "
                "already running - a second porchlightd, or a stale one.")
    if code == CLOSE_NO_HELLO:
        return "the server says we never said hello, which would be a bug in here."
    if code == 1013:
        return "the server is temporarily unable to take us. It is worth retrying."
    return "no reason given; the daemon would retry with backoff."


def read_rssi(path=WIRELESS_STATS):
    """The Wi-Fi signal in dBm, or None when there is no Wi-Fi to report.

    /proc/net/wireless rather than nmcli or iw: it is one read of a file with
    no process to start, it is there on Raspberry Pi OS and on our own image
    (CONFIG_CFG80211_WEXT), and it does not need NetworkManager to be the one
    managing the link. Two header lines, then one line per interface:

        wlan0: 0000   58.  -52.  -256   0  0  0  0  0   0

    The third number is the level. brcmfmac reports it in dBm; some drivers
    report it unsigned, which is the same number plus 256.

    None for a doorbell on a cable, or one whose driver reports nothing - the
    app then shows no signal rather than a made-up one.
    """
    try:
        with open(path) as handle:
            lines = handle.read().splitlines()[2:]
    except OSError:
        return None

    readings = {}
    for line in lines:
        name, _, rest = line.partition(":")
        fields = rest.split()
        if len(fields) < 3:
            continue
        try:
            level = float(fields[2].rstrip("."))
        except ValueError:
            continue
        if level > 0:
            level -= 256
        # Zero is what an interface with no association reports.
        if level == 0 or level < -120:
            continue
        readings[name.strip()] = round(level)

    if not readings:
        return None
    # wlan0 is the one that faces the house. The setup access point, when it
    # is up, is a second interface on the same radio and says nothing about
    # the home network.
    return readings.get("wlan0", next(iter(readings.values())))


def emit(message):
    """One JSON object per line on stdout, flushed: it is an event stream."""
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def note(text):
    """Human-readable output goes to stderr so it cannot corrupt the stream."""
    sys.stderr.write(f"bridge: {text}\n")
    sys.stderr.flush()


def signal_url(base_url):
    if base_url.startswith("https://"):
        return "wss://" + base_url[len("https://"):].rstrip("/") + "/signal"
    if base_url.startswith("http://"):
        return "ws://" + base_url[len("http://"):].rstrip("/") + "/signal"
    raise SystemExit(f"--url must start with http:// or https://, got {base_url!r}")


def read_credential(path):
    try:
        with open(path) as handle:
            credential = handle.read().strip()
    except OSError as error:
        raise SystemExit(
            f"cannot read credential {path}: {error}\n"
            "It is written onto the card when the device is built. There is no\n"
            "enrolment call - if it is missing, the device must be re-minted.")
    if not credential:
        raise SystemExit(f"{path} is empty; the device must be re-minted.")
    return credential


def check(base_url, device_id, credential):
    """Does this credential work? Answers without touching the socket."""
    request = urllib.request.Request(
        base_url.rstrip("/") + f"/api/devices/{device_id}/self",
        headers={"Authorization": f"Bearer {credential}"},
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            answer = json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise SystemExit(
            f"credential rejected: HTTP {error.code} "
            f"{error.read().decode(errors='replace')}")
    except urllib.error.URLError as error:
        raise SystemExit(f"cannot reach {base_url}: {error.reason}")

    device = answer.get("device", answer)
    note(f"credential works. deviceId={device.get('deviceId')} "
         f"name={device.get('name')} location={device.get('location')}")


def probe(base_url, device_id, credential, timeout=HELLO_TIMEOUT * 2):
    """Open the real socket, say hello, and report what came back.

    The last step of provisioning, and the first thing to run when a device
    that used to work has stopped. It answers a question --check cannot: the
    daemon speaks WebSocket, and a network can carry the HTTPS request that
    proves the credential while dropping the upgrade that carries everything
    else. It connects once and never retries - this is a verdict, not a link.

    Exits non-zero on anything but hello-ok, so a provisioning script can stop.
    """
    outcome = {"ok": False, "why": "no answer at all", "hello": {}}
    done = threading.Event()

    def on_open(socket):
        note(f"socket open, saying hello as role={HELLO_ROLE} deviceId={device_id}")
        socket.send(json.dumps(hello_frame(device_id, credential)))

    def on_message(socket, raw):
        try:
            message = json.loads(raw)
        except ValueError:
            return
        kind = message.get("type")
        if kind == "hello-ok":
            outcome["ok"] = True
            outcome["hello"] = message
            socket.close()
        elif kind == "hello-error":
            outcome["why"] = f"the server refused the hello: {message.get('error')}"
            socket.close()

    def on_close(_socket, code, reason):
        if not outcome["ok"] and code is not None:
            outcome["why"] = f"closed {code} {reason or ''} - {close_meaning(code)}"
        done.set()

    socket = websocket.WebSocketApp(
        signal_url(base_url),
        on_open=on_open,
        on_message=on_message,
        on_close=on_close,
        on_error=lambda _socket, error: outcome.update(why=f"socket error: {error}"),
    )
    threading.Thread(target=socket.run_forever, daemon=True).start()

    if not done.wait(timeout):
        # Nothing came back and nothing closed. The connection is open and
        # dead, which is the one case this probe exists to be able to name.
        note(f"nothing in {timeout:.0f}s. The socket is open and silent, which is a "
             "proxy or a middlebox rather than the server.")
        socket.close()
        return 1

    if not outcome["ok"]:
        note(f"the socket did not come up: {outcome['why']}")
        return 1

    answer = outcome["hello"]
    note(f"connected. The server knows this device as {answer.get('deviceId')} "
         f"(record {answer.get('device')}). This is what porchlightd does at boot.")
    return 0


class Bridge:
    def __init__(self, url, device_id, credential):
        self.url = url
        self.device_id = device_id
        self.credential = credential
        self.socket = None
        self.ready = False
        self.stop = False
        self.delay = RECONNECT_INITIAL
        # Armed when the socket opens, cancelled by hello-ok. See HELLO_TIMEOUT.
        self.hello_timer = None
        # stdin is read on its own thread, and the signal on another, so three
        # threads can reach the socket. websocket-client does not serialise
        # writes, and interleaved frames would corrupt the stream rather than
        # merely reorder it.
        self.sending = threading.Lock()
        # What the server was last told the signal is, on this connection.
        self.link_sent = _UNSENT
        # Set on hello-ok, so a new connection reports at once instead of up
        # to half a minute later.
        self.link_wake = threading.Event()
        # The update being watched, if any. One at a time: the server never
        # offers a second while one is in flight, and this holds to it too.
        self.update_watch = None

    def run(self):
        threading.Thread(target=self.read_stdin, daemon=True).start()
        threading.Thread(target=self.report_link, daemon=True).start()
        while not self.stop:
            self.connect_once()
            if self.stop:
                break
            # Jittered, so a power cut on a street does not return as a
            # thundering herd.
            wait = min(self.delay, RECONNECT_MAX) * (0.5 + random.random())
            note(f"reconnecting in {wait:.1f}s")
            time.sleep(wait)
            self.delay = min(self.delay * 2, RECONNECT_MAX)

    def connect_once(self):
        self.socket = websocket.WebSocketApp(
            self.url,
            on_open=self.on_open,
            on_message=self.on_message,
            on_close=self.on_close,
            on_error=lambda _socket, error: note(f"socket error: {error}"),
        )
        # run_forever owns this thread and does nothing else, which is what
        # keeps the automatic pong answering the server's 30s pings. Anything
        # that blocks in here shows up as a flaky network.
        self.socket.run_forever(ping_interval=20, ping_timeout=10)

    def on_open(self, socket):
        # The server closes anything that has not said hello within ten
        # seconds, so this is the first thing that happens.
        with self.sending:
            socket.send(json.dumps(hello_frame(self.device_id, self.credential)))

        def unanswered():
            if self.ready:
                return
            note(f"no answer to hello in {HELLO_TIMEOUT:.0f}s; starting again")
            # Not a graceful close: a peer that never answered the handshake
            # will not complete a closing one either, and waiting for it is
            # the stall this timer exists to end.
            socket.close()

        self.hello_timer = threading.Timer(HELLO_TIMEOUT, unanswered)
        self.hello_timer.daemon = True
        self.hello_timer.start()

    def cancel_hello_timer(self):
        if self.hello_timer is not None:
            self.hello_timer.cancel()
            self.hello_timer = None

    def on_message(self, _socket, raw):
        try:
            message = json.loads(raw)
        except ValueError:
            note(f"ignoring unparseable frame: {raw[:120]}")
            return

        kind = message.get("type")
        if kind == "hello-ok":
            self.cancel_hello_timer()
            self.ready = True
            self.delay = RECONNECT_INITIAL  # a good connection resets the backoff
            note(f"connected as role={HELLO_ROLE} deviceId={self.device_id}")
            emit({"type": "online"})
            # The server forgets the signal with the connection.
            self.link_sent = _UNSENT
            self.link_wake.set()
        elif kind == "hello-error":
            # The server should close after this, and this one does. Closing it
            # ourselves covers the server that does not: a socket left open
            # after a refused hello is one nothing will ever arrive on, and
            # waiting out HELLO_TIMEOUT for it only delays the retry.
            note(f"hello refused: {message.get('error')}")
            self.cancel_hello_timer()
            _socket.close()
        elif kind in ("event-ack", "viewer-requested"):
            emit(message)
        elif kind == "update":
            self.start_update(message)
        elif kind == "ping":
            self.send({"type": "pong"})
        elif kind == "replaced":
            note("another connection with our role took over")
        else:
            note(f"ignoring {kind}")

    def on_close(self, _socket, code, reason):
        self.cancel_hello_timer()
        if self.ready:
            emit({"type": "offline", "reason": f"closed {code}"})
        self.ready = False

        if code == CLOSE_BAD_CREDENTIAL:
            note("credential rejected. This is permanent - the device must be "
                 "re-minted and re-flashed. Not reconnecting.")
            emit({"type": "fatal", "reason": "credential-rejected"})
            self.stop = True
        elif code == CLOSE_REPLACED:
            # Reconnecting would fight whatever displaced us, most likely a
            # second copy of the daemon.
            note("replaced by another connection of our role; not reconnecting")
            emit({"type": "fatal", "reason": "replaced"})
            self.stop = True
        elif code == CLOSE_NO_HELLO:
            note("server says we never sent hello - that is a bug here")
        else:
            note(f"disconnected: {code} {reason} - {close_meaning(code)}")

    def send(self, message):
        if not self.ready:
            return False
        try:
            with self.sending:
                self.socket.send(json.dumps(message))
            return True
        except Exception as error:  # the socket can die between check and send
            note(f"send failed: {error}")
            return False

    def report_link(self):
        """Tells the server how good the Wi-Fi is, when that changes.

        On its own thread rather than in run_forever's, because that thread
        answers the server's pings and must never wait on anything. Reading a
        /proc file does not block in practice, but the rule is simpler kept
        without exceptions.
        """
        while not self.stop:
            if self.ready:
                rssi = read_rssi()
                last = self.link_sent
                moved = (
                    last is _UNSENT
                    or (rssi is None) != (last is None)
                    or (rssi is not None and abs(rssi - last) >= LINK_REPORT_STEP)
                )
                if moved and self.send({"type": "link", "rssi": rssi}):
                    self.link_sent = rssi
            self.link_wake.wait(LINK_SAMPLE_INTERVAL)
            self.link_wake.clear()

    def start_update(self, message):
        """Hands an update request to the root updater, and watches it.

        Written atomically - a temporary name, then a rename - because the
        path unit fires the moment request.json exists, and must never see
        half of one."""
        version = message.get("version")
        if not isinstance(version, str):
            return
        if not can_update():
            self.send({"type": "update-status", "version": version, "state": "failed",
                       "error": "This doorbell cannot install updates"})
            return
        if self.update_watch is not None and self.update_watch.is_alive():
            note(f"update to {version} asked for while one is running; ignored")
            return

        request = {key: message.get(key) for key in ("version", "url", "bytes", "sha256")}
        tmp = UPDATE_REQUEST + ".tmp"
        try:
            with open(tmp, "w") as handle:
                json.dump(request, handle)
            os.replace(tmp, UPDATE_REQUEST)
        except OSError as error:
            note(f"could not hand over the update: {error}")
            self.send({"type": "update-status", "version": version, "state": "failed",
                       "error": "The doorbell could not start the update"})
            return

        note(f"update to {version} handed to the updater")
        self.update_watch = threading.Thread(target=self.watch_update, args=(version, time.time()),
                                             daemon=True)
        self.update_watch.start()

    def watch_update(self, version, since):
        """Relays the updater's status file until it ends: failed, or about to
        restart. Only statuses for this version written after the request
        count - the file still holds the last update's ending until the new
        one starts."""
        last = None
        deadline = time.time() + UPDATE_WATCH_LIMIT
        while time.time() < deadline and not self.stop:
            time.sleep(UPDATE_POLL)
            try:
                with open(UPDATE_STATUS) as handle:
                    status = json.load(handle)
            except (OSError, ValueError):
                continue
            if status.get("version") != version or status.get("at", 0) < since:
                continue
            state = status.get("state")
            frame = {"type": "update-status", "version": version, "state": state}
            if isinstance(status.get("progress"), (int, float)):
                frame["progress"] = status["progress"]
            if status.get("error"):
                frame["error"] = status["error"]
            key = (state, frame.get("progress"))
            if key != last and self.send(frame):
                last = key
            if state in ("failed", "rebooting") and last == key:
                return

    def read_stdin(self):
        """Lines from the daemon. EOF means the daemon is gone; so are we."""
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                note(f"ignoring unparseable line: {line[:120]}")
                continue

            if message.get("type") == "event":
                # The socket is authoritative about who we are, but sending it
                # keeps the logs readable on both ends.
                message["deviceId"] = self.device_id
                if not self.send(message):
                    # No ack will ever arrive for this one, and the core treats
                    # silence as retryable. That is exactly right.
                    note(f"not sent, link is down: {message.get('eventId')}")
            else:
                note(f"ignoring command {message.get('type')}")

        note("stdin closed; exiting")
        self.stop = True
        if self.socket:
            self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="e.g. http://192.168.0.219:4000")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--credential-file", required=True)
    parser.add_argument("--check", action="store_true",
                        help="verify the credential over HTTP and exit")
    parser.add_argument("--probe", action="store_true",
                        help="open the signaling socket once, report, and exit")
    options = parser.parse_args()

    credential = read_credential(options.credential_file)

    if options.check:
        check(options.url, options.device_id, credential)
        return

    if websocket is None:
        raise SystemExit(
            "python3-websocket is not installed: sudo apt install -y python3-websocket")

    if options.probe:
        raise SystemExit(probe(options.url, options.device_id, credential))

    Bridge(signal_url(options.url), options.device_id, credential).run()


if __name__ == "__main__":
    main()
