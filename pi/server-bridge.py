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

Run it by hand to test against a server with no daemon involved:

    ./server-bridge.py --pair PAIR-7K2M9P4Q --url http://host:4000 \
        --device-id porch-1 --credential-file ./cred
    ./server-bridge.py --url http://host:4000 --device-id porch-1 \
        --credential-file ./cred
    {"type":"event","eventId":"test-1","kind":"ring","at":"2026-09-21T10:00:00.000Z"}
"""

import argparse
import json
import os
import random
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import websocket  # python3-websocket
except ImportError:  # --pair is pure stdlib and must work without it
    websocket = None

HELLO_ROLE = "device"

# Reconnect delays. A doorbell that has been offline for an hour should come
# back promptly, but a thousand of them must not return in the same second.
RECONNECT_INITIAL = 1.0
RECONNECT_MAX = 60.0

# Close codes the server defines. 4001 and 4002 both mean stop trying, for
# opposite reasons: one says another copy of us took over, the other says the
# credential will never be accepted.
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


def pair(base_url, device_id, code, credential_file):
    """Redeem a pairing code. The credential comes back exactly once."""
    body = json.dumps({"pairingCode": code, "deviceId": device_id}).encode()
    request = urllib.request.Request(
        base_url.rstrip("/") + "/api/provision",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            answer = json.loads(response.read())
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        raise SystemExit(f"pairing failed: HTTP {error.code} {detail}")
    except urllib.error.URLError as error:
        raise SystemExit(f"pairing failed: {error.reason}")

    credential = answer.get("credential")
    if not credential:
        raise SystemExit(f"pairing returned no credential: {answer}")

    # Written before anything else happens. There is no endpoint to read it
    # back, so losing it here means pairing again.
    directory = os.path.dirname(os.path.abspath(credential_file))
    if directory:
        os.makedirs(directory, exist_ok=True)
    handle = os.open(credential_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(handle, "w") as out:
        out.write(credential + "\n")

    note(f"paired as {answer.get('deviceId')}, credential saved to {credential_file}")
    note(f"name: {answer.get('name')}  paired at: {answer.get('pairedAt')}")


def read_credential(path):
    try:
        with open(path) as handle:
            credential = handle.read().strip()
    except OSError as error:
        raise SystemExit(f"cannot read credential {path}: {error}. Run --pair first.")
    if not credential:
        raise SystemExit(f"{path} is empty. Run --pair first.")
    return credential


class Bridge:
    def __init__(self, url, device_id, credential):
        self.url = url
        self.device_id = device_id
        self.credential = credential
        self.socket = None
        self.ready = False
        self.stop = False
        self.delay = RECONNECT_INITIAL

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
        # websocket-client answers the server's protocol pings by itself; this
        # interval is our own liveness check on top of that.
        self.socket.run_forever(ping_interval=20, ping_timeout=10)

    def on_open(self, socket):
        # The server closes anything that has not said hello within ten
        # seconds, so this is the first thing that happens.
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
            note(f"connected as {HELLO_ROLE}/{self.device_id}")
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
            note("credential rejected; this cannot be retried. Pair again.")
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
                # deviceId is ours to add; the daemon should not have to
                # repeat it on every alert.
                message["deviceId"] = self.device_id
                if not self.send(message):
                    # No ack will ever arrive for this one. The daemon's core
                    # treats silence as retryable, which is what we want.
                    note(f"dropped event {message.get('eventId')}: link is down")
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
    parser.add_argument("--pair", metavar="CODE", help="redeem a pairing code and exit")
    options = parser.parse_args()

    if options.pair:
        pair(options.url, options.device_id, options.pair, options.credential_file)
        return

    if websocket is None:
        raise SystemExit(
            "python3-websocket is not installed: sudo apt install -y python3-websocket")

    credential = read_credential(options.credential_file)
    Bridge(signal_url(options.url), options.device_id, credential).run()


if __name__ == "__main__":
    main()
