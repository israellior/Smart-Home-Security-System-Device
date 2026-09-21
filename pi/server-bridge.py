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

By hand, against a live server and with no daemon involved:

    ./server-bridge.py --url http://host:4000 --device-id porch-1 \\
        --credential-file /etc/porchlight/credential --check
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

CLOSE_REPLACED = 4001
CLOSE_BAD_CREDENTIAL = 4002
CLOSE_NO_HELLO = 4003


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


class Bridge:
    def __init__(self, url, device_id, credential):
        self.url = url
        self.device_id = device_id
        self.credential = credential
        self.socket = None
        self.ready = False
        self.stop = False
        self.delay = RECONNECT_INITIAL
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
            socket.send(json.dumps({
                "type": "hello",
                "role": HELLO_ROLE,
                "deviceId": self.device_id,
                "token": self.credential,
            }))

    def on_message(self, _socket, raw):
        try:
            message = json.loads(raw)
        except ValueError:
            note(f"ignoring unparseable frame: {raw[:120]}")
            return

        kind = message.get("type")
        if kind == "hello-ok":
            self.ready = True
            self.delay = RECONNECT_INITIAL  # a good connection resets the backoff
            note(f"connected as role={HELLO_ROLE} deviceId={self.device_id}")
            emit({"type": "online"})
        elif kind == "hello-error":
            note(f"hello refused: {message.get('error')}")
        elif kind in ("event-ack", "viewer-requested"):
            emit(message)
        elif kind == "ping":
            self.send({"type": "pong"})
        elif kind == "replaced":
            note("another connection with our role took over")
        else:
            note(f"ignoring {kind}")

    def on_close(self, _socket, code, reason):
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
            note(f"disconnected: {code} {reason}")

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
    options = parser.parse_args()

    credential = read_credential(options.credential_file)

    if options.check:
        check(options.url, options.device_id, credential)
        return

    if websocket is None:
        raise SystemExit(
            "python3-websocket is not installed: sudo apt install -y python3-websocket")

    Bridge(signal_url(options.url), options.device_id, credential).run()


if __name__ == "__main__":
    main()
