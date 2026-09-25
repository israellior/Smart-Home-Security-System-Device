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
import json
import random
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


def hello_frame(device_id, credential):
    """Said first on every connection, by the bridge and by --probe alike."""
    return {
        "type": "hello",
        "role": HELLO_ROLE,
        "deviceId": device_id,
        "token": credential,
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
        # stdin is read on its own thread, so two threads can reach the socket.
        # websocket-client does not serialise writes, and interleaved frames
        # would corrupt the stream rather than merely reorder it.
        self.sending = threading.Lock()

    def run(self):
        threading.Thread(target=self.read_stdin, daemon=True).start()
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
