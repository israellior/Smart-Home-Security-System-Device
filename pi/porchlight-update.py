#!/usr/bin/env python3
"""Installs one software update the app server asked for, then restarts.

Started by porchlight-update.path when the bridge writes a request, and run as
root, because installing a system and rebooting are root's to do. The bridge
runs as `porchlight` and can do neither; what it can do is write a request
into /data/update and read back how it is going. That is the whole interface:

    /data/update/request.json   bridge -> here   {version, url, bytes, sha256}
    /data/update/status.json    here -> bridge   {version, state, progress, error}

    downloading -> installing -> rebooting       or failed, with a reason

Nothing in a request can make this install anything it should not. RAUC only
accepts a bundle signed by the key whose certificate is in the image, so the
worst a request can do is install a genuine release - which is why the bridge,
and the server behind it, are trusted to ask.

Rolling back is not here either. A slot that fails to come up steadily is
marked bad by porchlight-health and the device boots the other one; the server
learns which happened from the version the doorbell reports when it is back.

    ./porchlight-update.py                     # serve the pending request
    ./porchlight-update.py --dry-run           # download and check, no install
"""

import argparse
import grp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

UPDATE_DIR = "/data/update"
REQUEST = os.path.join(UPDATE_DIR, "request.json")
STATUS = os.path.join(UPDATE_DIR, "status.json")
BUNDLE = os.path.join(UPDATE_DIR, "bundle.raucb")

# A bundle is ~90 MB. Anything far past this is not one of ours.
MAX_BYTES = 512 * 1024 * 1024

# Room left on /data after the download, for what lives there and keeps
# arriving while this runs: the journal (held to 32 MB) and queued clips.
HEADROOM = 48 * 1024 * 1024

CHUNK = 256 * 1024
ATTEMPTS = 3
READ_TIMEOUT = 60

# How often progress is written - every 5%, which is about every 4.5 MB.
PROGRESS_STEP = 0.05

INSTALL_TIMEOUT = 20 * 60


def note(text):
    sys.stderr.write(f"porchlight-update: {text}\n")
    sys.stderr.flush()


# The bridge's group. The file is written by root, and a file root creates
# belongs to group root - which the bridge, as porchlight, cannot read. That
# was release-15: the update worked and not one step of its progress reached
# the app.
BRIDGE_GROUP = "porchlight"


def bridge_gid():
    try:
        return grp.getgrnam(BRIDGE_GROUP).gr_gid
    except KeyError:
        return None


def write_status(version, state, **extra):
    """Atomically, and readable by the bridge: group porchlight, mode 0640."""
    body = {"version": version, "state": state, "at": time.time(), **extra}
    tmp = STATUS + ".tmp"
    with open(tmp, "w") as handle:
        json.dump(body, handle)
    gid = bridge_gid()
    if gid is not None:
        os.chown(tmp, -1, gid)
    os.chmod(tmp, 0o640)
    os.replace(tmp, STATUS)


class Refused(Exception):
    """A failure with a reason worth showing in the app."""


def read_request():
    """Takes the request - it is removed whatever happens next, so one request
    is one attempt. Returns None when there is nothing to do."""
    try:
        with open(REQUEST) as handle:
            request = json.load(handle)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as error:
        note(f"unreadable request, dropping it: {error}")
        return None
    finally:
        try:
            os.remove(REQUEST)
        except FileNotFoundError:
            pass

    version = request.get("version")
    url = request.get("url")
    size = request.get("bytes")
    digest = request.get("sha256")
    if not isinstance(version, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", version):
        note("request has no usable version; dropping it")
        return None
    problems = []
    if not isinstance(url, str) or not url.startswith("https://"):
        problems.append("no https URL")
    if not isinstance(size, int) or not 0 < size <= MAX_BYTES:
        problems.append("no usable size")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        problems.append("no usable sha256")
    if problems:
        write_status(version, "failed", error="The update request was incomplete: " + ", ".join(problems))
        return None
    return {"version": version, "url": url, "bytes": size, "sha256": digest}


def download(request):
    """To /data/update/bundle.raucb, checked against the size and hash the
    server published. A whole-file retry rather than a resume: three tries at
    90 MB is cheaper than the code that resumes, and a signed URL for one
    object does not care how many times it is read."""
    version, size = request["version"], request["bytes"]

    try:
        os.remove(BUNDLE)
    except FileNotFoundError:
        pass
    free = shutil.disk_usage(UPDATE_DIR).free
    if free < size + HEADROOM:
        raise Refused(f"Not enough space on the doorbell ({free // 2**20} MB free, "
                      f"{(size + HEADROOM) // 2**20} MB needed)")

    last_error = None
    for attempt in range(1, ATTEMPTS + 1):
        write_status(version, "downloading", progress=0)
        digest = hashlib.sha256()
        received = 0
        reported = 0.0
        try:
            with urllib.request.urlopen(request["url"], timeout=READ_TIMEOUT) as response, \
                    open(BUNDLE, "wb") as out:
                while True:
                    chunk = response.read(CHUNK)
                    if not chunk:
                        break
                    received += len(chunk)
                    if received > size:
                        raise Refused("The download was larger than the release")
                    digest.update(chunk)
                    out.write(chunk)
                    progress = received / size
                    if progress - reported >= PROGRESS_STEP:
                        reported = progress
                        write_status(version, "downloading", progress=round(progress, 2))
                out.flush()
                os.fsync(out.fileno())
        except Refused:
            raise
        except (urllib.error.URLError, OSError, TimeoutError) as error:
            last_error = error
            note(f"download attempt {attempt} failed: {error}")
            time.sleep(5 * attempt)
            continue

        if received != size:
            last_error = f"got {received} of {size} bytes"
            note(f"download attempt {attempt} was short: {last_error}")
            continue
        if digest.hexdigest() != request["sha256"]:
            # Not retried: the same URL would give the same wrong bytes.
            raise Refused("The download did not match the release")
        return

    raise Refused(f"The download kept failing ({last_error})")


def install(version):
    write_status(version, "installing")
    try:
        done = subprocess.run(["rauc", "install", BUNDLE], capture_output=True, text=True,
                              timeout=INSTALL_TIMEOUT)
    except FileNotFoundError:
        raise Refused("This doorbell's system cannot install updates")
    except subprocess.TimeoutExpired:
        raise Refused("Installing took too long")
    if done.returncode != 0:
        # RAUC's last line is its reason - a bad signature says so in words.
        lines = (done.stderr or done.stdout).strip().splitlines()
        raise Refused("Install failed: " + (lines[-1] if lines else f"rauc exited {done.returncode}"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true",
                        help="download and verify, but do not install or restart")
    options = parser.parse_args()

    request = read_request()
    if request is None:
        return 0
    version = request["version"]
    note(f"updating to {version} ({request['bytes']} bytes)")

    try:
        download(request)
        if options.dry_run:
            note("dry run: downloaded and verified, not installing")
            write_status(version, "failed", error="Dry run: verified, not installed")
            return 0
        install(version)
    except Refused as error:
        note(str(error))
        write_status(version, "failed", error=str(error))
        return 1
    finally:
        # The installed slot holds the system now; the bundle is 90 MB of a
        # 256 MB partition that clips and the journal also live on.
        try:
            os.remove(BUNDLE)
        except FileNotFoundError:
            pass

    note(f"installed {version}; restarting into it")
    write_status(version, "rebooting")
    # Long enough for the bridge, which polls every two seconds, to tell the
    # server before the network goes.
    time.sleep(6)
    # --no-block, and then out: waiting on the reboot from inside a service
    # meant shutdown stopped this service mid-wait, and every successful
    # update was logged as porchlight-update.service failing on a signal.
    subprocess.run(["systemctl", "--no-block", "reboot"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
