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

    bridge --> server   {"type":"link","rssi":-61,"ssid":"HomeNet"}
    server --> bridge   {"type":"update","version":...,"url":...,"bytes":...,"sha256":...}
    bridge --> server   {"type":"update-status","version":...,"state":...,"progress":...}
    server --> bridge   {"type":"wifi-scan"}  {"type":"wifi-join","id":...,"ssid":...,"psk":...}
    bridge --> server   {"type":"wifi-networks",...}  {"type":"wifi-status","id":...,"state":...}
    server --> bridge   {"type":"faces-sync","revision":8,"people":[{"id":...,"photos":[...]}]}
    bridge --> server   {"type":"faces-status","revision":8,"photos":[{"id":...,"ok":...}]}

Those are the bridge's own and never pass through the daemon. The first is the
Wi-Fi signal and network, for the app to show (see read_rssi). Updates and
networks are things only root can do, so the bridge, which runs as
`porchlight`, hands each to a root helper through a directory on /data and
relays what it reports. See start_update and start_wifi. The people this
doorbell knows need no root: porchlight-faces.py runs as this same user, and
start_faces runs it directly.

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
import importlib.util
import json
import os
import random
import re
import subprocess
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

# Changing wi-fi from the app - porchlight-wifi.py is the other side.
WIFI_DIR = "/data/wifi"
WIFI_REQUEST = os.path.join(WIFI_DIR, "request.json")
WIFI_STATUS = os.path.join(WIFI_DIR, "status.json")
WIFI_NETWORKS = os.path.join(WIFI_DIR, "networks.json")
WIFI_HELPER = "/usr/local/lib/porchlight/porchlight-wifi.py"
SETUP_SSID = os.path.join(WIFI_DIR, "setup-ssid")

# A scan takes a few seconds; a change can take minutes - three join attempts
# fifteen seconds apart, the staged check, and a fall back if it fails - and
# its result has to wait for this socket to come back on whichever network won.
WIFI_SCAN_LIMIT = 60
WIFI_JOIN_LIMIT = 8 * 60

# How often the updater's status file is read while an update runs, and how
# long before the bridge stops watching one that never finishes. The server
# gives up on its own after 45 minutes of silence.
UPDATE_POLL = 2.0
UPDATE_WATCH_LIMIT = 60 * 60

# The people this doorbell knows - porchlight-faces.py is the other side, and
# runs as this same user: nothing in it needs root, unlike an update or a
# change of network. The list arrives here and goes to it as a file; its
# answer comes back on its stdout.
FACES_DIR = "/data/faces"
FACES_STORE = os.path.join(FACES_DIR, "faces.json")
FACES_REQUEST = os.path.join(FACES_DIR, "sync.json")
FACES_HELPER = "/usr/local/lib/porchlight/porchlight-faces.py"
FACES_MODELS = ("/usr/local/share/porchlight/face_detection_full_range.tflite",
                "/usr/local/share/porchlight/face_code.tflite")
FACES_MODEL_ID = "/usr/local/share/porchlight/face_code.id"
# Downloading and coding up to 200 photos at a few hundred milliseconds each,
# on a doorbell that may be recording at the same time.
FACES_LIMIT = 15 * 60


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


def can_change_wifi():
    """Whether this device has the wi-fi helper, and we can ask it things."""
    return os.path.exists(WIFI_HELPER) and os.access(WIFI_DIR, os.W_OK)


def setup_ssid():
    """The name of the network this doorbell raises when it has none, so the
    app can say which one to join - the one time it is needed, the doorbell
    is offline and cannot be asked. Copied out of the identity at boot."""
    try:
        with open(SETUP_SSID) as handle:
            return handle.read().strip() or None
    except OSError:
        return None


def current_ssid():
    """The network the doorbell is on, from NetworkManager's own list - which
    an unprivileged user may read. None off wi-fi, or with no nmcli."""
    try:
        done = subprocess.run(["nmcli", "-t", "-f", "ACTIVE,SSID", "device", "wifi"],
                              capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for row in done.stdout.splitlines():
        active, _, ssid = row.partition(":")
        if active == "yes" and ssid:
            return ssid.replace("\\:", ":")
    return None


def faces_hello():
    """{"model", "revision"} when this doorbell can recognise faces, else None.

    Can means: the helper and both models installed, Pillow to read the
    photos, and the store's directory ours to write. A doorbell missing any of
    them says nothing, and is never sent the list it could not use.

    `revision` is the list last applied - but only if the store was made by
    the model installed now. After an update that changed the model its codes
    compare with nothing, so it reports None and is sent every photo again."""
    try:
        with open(FACES_MODEL_ID) as handle:
            model = handle.read().strip()
    except OSError:
        return None
    if not (model and os.path.exists(FACES_HELPER) and all(map(os.path.exists, FACES_MODELS))
            and importlib.util.find_spec("PIL") is not None and os.access(FACES_DIR, os.W_OK)):
        return None
    revision = None
    try:
        with open(FACES_STORE) as handle:
            store = json.load(handle)
        if store.get("model") == model and isinstance(store.get("revision"), int):
            revision = store["revision"]
    except (OSError, ValueError, AttributeError):
        pass
    return {"model": model, "revision": revision}


def hello_frame(device_id, credential):
    """Said first on every connection, by the bridge and by --probe alike.

    It carries what this device is running, so the server learns how an
    update went from the hello after the restart - including a rollback,
    which nothing on the device ever reports in so many words."""
    frame = {
        "type": "hello",
        "role": HELLO_ROLE,
        "deviceId": device_id,
        "token": credential,
        "version": installed_version(),
        "updater": can_update(),
        "wifi": can_change_wifi(),
        "setupSsid": setup_ssid(),
    }
    faces = faces_hello()
    if faces is not None:
        frame["faces"] = faces
    return frame


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
        self.wifi_watch = None
        # The people this doorbell knows. One sync at a time, on its own
        # thread; a list that arrives while one runs waits here, and only the
        # newest waits - it is the whole list, so an older one is out of date.
        self.faces_lock = threading.Lock()
        self.faces_running = False
        self.faces_waiting = None
        # The answer to the last sync, until it has reached the server: the
        # socket may well be down when a long sync ends, and the hello after
        # it reports the new revision, so nothing would make it be asked again.
        self.faces_unsent = None

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
            self.send_faces_status()
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
        elif kind == "wifi-scan":
            self.start_wifi({"action": "scan"})
        elif kind == "wifi-join":
            self.start_wifi({"action": "join", "id": message.get("id"),
                             "ssid": message.get("ssid"), "psk": message.get("psk")})
        elif kind == "faces-sync":
            self.start_faces(message)
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
                ssid = current_ssid()
                last = self.link_sent
                last_rssi, last_ssid = (None, None) if last is _UNSENT else last
                moved = (
                    last is _UNSENT
                    or ssid != last_ssid
                    or (rssi is None) != (last_rssi is None)
                    or (rssi is not None and abs(rssi - last_rssi) >= LINK_REPORT_STEP)
                )
                if moved and self.send({"type": "link", "rssi": rssi, "ssid": ssid}):
                    self.link_sent = (rssi, ssid)
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

    def start_wifi(self, request):
        """Hands a scan or a change to the root wi-fi helper, and watches it.

        The request can carry a wi-fi password, so it is written for its
        owner's group alone and the helper deletes it as it reads it."""
        action = request["action"]
        if not can_change_wifi():
            if action == "join":
                self.send({"type": "wifi-status", "id": request.get("id"), "state": "failed",
                           "error": "This doorbell needs an update before it can change networks."})
            return
        if self.wifi_watch is not None and self.wifi_watch.is_alive():
            note(f"wi-fi {action} asked for while one is running; ignored")
            return

        since = time.time()
        tmp = WIFI_REQUEST + ".tmp"
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o640)
            with os.fdopen(fd, "w") as handle:
                json.dump(request, handle)
            os.replace(tmp, WIFI_REQUEST)
        except OSError as error:
            note(f"could not hand over the wi-fi {action}: {error}")
            if action == "join":
                self.send({"type": "wifi-status", "id": request.get("id"), "state": "failed",
                           "error": "The doorbell could not start the change."})
            return

        note(f"wi-fi {action} handed to the helper")
        target = self.watch_wifi_scan if action == "scan" else self.watch_wifi_join
        args = (since,) if action == "scan" else (request.get("id"), since)
        self.wifi_watch = threading.Thread(target=target, args=args, daemon=True)
        self.wifi_watch.start()

    def watch_wifi_scan(self, since):
        deadline = time.time() + WIFI_SCAN_LIMIT
        while time.time() < deadline and not self.stop:
            time.sleep(UPDATE_POLL)
            try:
                with open(WIFI_NETWORKS) as handle:
                    found = json.load(handle)
            except (OSError, ValueError):
                continue
            if found.get("at", 0) < since:
                continue
            self.send({"type": "wifi-networks", "networks": found.get("networks", []),
                       "current": found.get("current")})
            return

    def watch_wifi_join(self, request_id, since):
        """Relays joining, then the outcome. The outcome is sent until it lands:
        by then this socket has dropped with the old network and come back on
        whichever one won, and an outcome nobody hears leaves the app waiting."""
        last = None
        deadline = time.time() + WIFI_JOIN_LIMIT
        while time.time() < deadline and not self.stop:
            time.sleep(UPDATE_POLL)
            try:
                with open(WIFI_STATUS) as handle:
                    status = json.load(handle)
            except (OSError, ValueError):
                continue
            if status.get("id") != request_id or status.get("at", 0) < since:
                continue
            state = status.get("state")
            frame = {"type": "wifi-status", "id": request_id, "state": state,
                     "ssid": status.get("ssid"), "previous": status.get("previous")}
            if status.get("error"):
                frame["error"] = status["error"]
            if state != last and self.send(frame):
                last = state
            if state in ("connected", "failed") and last == state:
                return

    def start_faces(self, message):
        """Hands the server's list of people to porchlight-faces.py.

        Not done here: coding a photo needs numpy and two models, and this
        process's main thread answers the server's pings and must never wait
        on anything. The helper runs as this same user, on its own thread,
        and the list reaches it as a file - it can carry a few hundred signed
        URLs, which is more than belongs on a command line."""
        if not isinstance(message.get("people"), list) or faces_hello() is None:
            note("faces-sync ignored: not a list, or this doorbell cannot recognise faces")
            return
        with self.faces_lock:
            if self.faces_running:
                self.faces_waiting = message
                note(f"faces-sync {message.get('revision')} waiting for the one running")
                return
            self.faces_running = True
        threading.Thread(target=self.run_faces, args=(message,), daemon=True).start()

    def run_faces(self, message):
        while message is not None and not self.stop:
            self.faces_once(message)
            with self.faces_lock:
                message, self.faces_waiting = self.faces_waiting, None
                if message is None:
                    self.faces_running = False

    def faces_once(self, message):
        revision = message.get("revision")
        tmp = FACES_REQUEST + ".tmp"
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as handle:
                json.dump(message, handle)
            os.replace(tmp, FACES_REQUEST)
        except OSError as error:
            note(f"faces-sync {revision}: could not hand it over: {error}")
            return
        began = time.monotonic()
        try:
            done = subprocess.run(
                [sys.executable, FACES_HELPER, "sync", "--request", FACES_REQUEST,
                 "--store", FACES_STORE],
                stdout=subprocess.PIPE, text=True, timeout=FACES_LIMIT)
            answer = json.loads(done.stdout.strip().splitlines()[-1])
        except (OSError, subprocess.TimeoutExpired, ValueError, IndexError) as error:
            # Nothing is sent: the store is as it was, the next hello reports
            # its old revision, and the server sends the list again.
            note(f"faces-sync {revision} failed: {error!r}")
            return
        finally:
            try:
                os.unlink(FACES_REQUEST)  # signed URLs, no use to anyone afterwards
            except OSError:
                pass
        ok = sum(1 for p in answer.get("photos", []) if p.get("ok"))
        note(f"faces-sync {revision}: {ok} of {len(answer.get('photos', []))} photos usable, "
             f"{time.monotonic() - began:.0f}s")
        self.faces_unsent = {"type": "faces-status", "revision": answer.get("revision"),
                             "photos": answer.get("photos", [])}
        self.send_faces_status()

    def send_faces_status(self):
        frame = self.faces_unsent
        if frame is not None and self.send(frame):
            self.faces_unsent = None

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
