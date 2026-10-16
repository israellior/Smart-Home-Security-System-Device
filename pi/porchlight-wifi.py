#!/usr/bin/env python3
"""Moves a working doorbell to another wi-fi network, asked from the app.

The case this exists for: the doorbell is online, and its owner wants it on a
different network - a new router while the old one still works, the 5 GHz
one, a mesh. A doorbell whose network has gone already has a way back: after
a restart, porchlight-setup.py raises its own access point when no saved
network comes up, and the app says so.

Started by porchlight-wifi.path, as root, because changing networks is
NetworkManager's privileged side. The bridge runs as `porchlight` and asks
through /data/wifi, exactly as it asks for updates:

    /data/wifi/request.json   bridge -> here  {"action": "scan"}
                                              {"action": "join", "id", "ssid", "psk"}
    /data/wifi/networks.json  here -> bridge  what a scan found
    /data/wifi/status.json    here -> bridge  {"id", "state", "ssid", "error"}

    joining -> connected                       or failed, back on the old one

THE FALL BACK IS THE POINT. Joining means leaving the network the doorbell is
on, and a wrong password typed in the app would otherwise be a doorbell lost
until somebody walks up to it. So the new network has to prove it reaches
Porchlight - the same staged check the setup page uses - before the old one is
let go of, and anything short of that puts the doorbell back where it was.

The joining and the checking are porchlight-setup.py's, imported rather than
copied, so a doorbell explains a failed change in the same sentences its setup
page uses.

    ./porchlight-wifi.py             # serve the pending request
"""

import importlib.util
import json
import os
import sys
import time
import grp

WIFI_DIR = "/data/wifi"
REQUEST = os.path.join(WIFI_DIR, "request.json")
STATUS = os.path.join(WIFI_DIR, "status.json")
NETWORKS = os.path.join(WIFI_DIR, "networks.json")
SETUP = "/usr/local/lib/porchlight/porchlight-setup.py"
IDENTITY = "/data/porchlight/identity.json"
CREDENTIAL = "/etc/porchlight/credential"
BRIDGE_GROUP = "porchlight"

# How long to wait for the old network to come back after a failed change.
RESTORE_WAIT = 60


def note(text):
    sys.stderr.write(f"porchlight-wifi: {text}\n")
    sys.stderr.flush()


def load_setup():
    spec = importlib.util.spec_from_file_location("porchlight_setup", SETUP)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write(path, body):
    """Atomically, group porchlight, 0640 - the bridge has to read it, and a
    file root writes belongs to group root (the release-15 lesson)."""
    tmp = path + ".tmp"
    with open(tmp, "w") as handle:
        json.dump({**body, "at": time.time()}, handle)
    try:
        os.chown(tmp, -1, grp.getgrnam(BRIDGE_GROUP).gr_gid)
    except KeyError:
        pass
    os.chmod(tmp, 0o640)
    os.replace(tmp, path)


def take_request():
    """The request is removed as it is read: it may hold a wi-fi password, and
    one request is one attempt."""
    try:
        with open(REQUEST) as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None
    finally:
        try:
            os.remove(REQUEST)
        except FileNotFoundError:
            pass


def current_connection(network):
    """The connection profile wlan0 is using now, and its network's name."""
    code, out, _ = network._run(["-t", "-f", "NAME,TYPE,DEVICE", "connection", "show", "--active"])
    if code == 0:
        for row in out.splitlines():
            name, kind, device = (row.rsplit(":", 2) + ["", ""])[:3]
            if kind == "802-11-wireless" and device == network.interface:
                return name.replace("\\:", ":")
    return None


def scan(network):
    found = network.scan()
    current = current_connection(network)
    write(NETWORKS, {"networks": found[:30], "current": current})
    note(f"scan: {len(found)} network(s); on {current}")


def restore(network, previous):
    """Back onto the network it was on. NetworkManager would usually find it on
    its own - the profile is still there - but usually is not good enough for a
    doorbell nobody can reach if it does not."""
    if not previous:
        return
    network._run(["connection", "up", previous], timeout=90)
    deadline = time.monotonic() + RESTORE_WAIT
    while time.monotonic() < deadline:
        if current_connection(network) == previous and network.active_ipv4():
            note(f"back on {previous}")
            return
        time.sleep(2)
    note(f"could not confirm {previous} came back; NetworkManager keeps trying")


def join(setup, network, request):
    request_id = request.get("id")
    ssid = request.get("ssid")
    psk = request.get("psk") or ""
    if not isinstance(ssid, str) or not ssid or len(ssid.encode()) > 32:
        write(STATUS, {"id": request_id, "state": "failed", "ssid": ssid,
                       "error": "That network name is not valid."})
        return

    previous = current_connection(network)
    if previous == ssid and not psk:
        write(STATUS, {"id": request_id, "state": "connected", "ssid": ssid})
        return

    write(STATUS, {"id": request_id, "state": "joining", "ssid": ssid, "previous": previous})
    note(f"moving from {previous} to {ssid}")

    try:
        with open(IDENTITY) as handle:
            identity = json.load(handle)
    except (OSError, ValueError):
        identity = {}
    try:
        with open(CREDENTIAL) as handle:
            credential = handle.read().strip()
    except OSError:
        credential = ""

    cause = network.join(ssid, psk)
    if cause is None:
        cause = setup.verify(network, identity.get("url", ""), identity.get("deviceId", ""), credential)

    if cause is None:
        # Preferred from now on: the profile nmcli just made is the one to
        # come back to after a restart. The old one stays as a second choice.
        network._run(["connection", "modify", ssid, "connection.autoconnect-priority", "10"])
        note(f"now on {ssid}")
        write(STATUS, {"id": request_id, "state": "connected", "ssid": ssid})
        return

    note(f"{ssid} did not work ({cause}); going back to {previous}")
    # Not kept: a profile that does not work would be tried at every boot.
    if ssid != previous:
        network.forget(ssid)
    restore(network, previous)
    write(STATUS, {"id": request_id, "state": "failed", "ssid": ssid, "previous": previous,
                   "error": setup.CAUSES.get(cause, "The doorbell could not join that network.")})


def main():
    request = take_request()
    if not request:
        return 0
    setup = load_setup()
    network = setup.Network()
    action = request.get("action")
    if action == "scan":
        scan(network)
    elif action == "join":
        join(setup, network, request)
    else:
        note(f"unknown action {action!r}; ignored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
