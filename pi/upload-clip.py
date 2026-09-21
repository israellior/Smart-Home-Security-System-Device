#!/usr/bin/env python3
"""Upload one clip, in the three steps the server expects.

Spawned once per attempt by porchlightd and judged entirely by its exit code:
0 means the confirm succeeded and the daemon may delete its local copy.
Anything else means keep the file and try the whole sequence again later.

    1. POST /api/clips/<eventId>/upload-url   -> a short-lived signed PUT URL
    2. PUT  <that url>                        -> the bytes, and only the bytes
    3. POST /api/clips/<eventId>/confirm      -> the metadata

Step 2 succeeding is not enough. A bucket PUT that is never confirmed is a
clip the server does not know exists, so only step 3 earns exit 0.

Nothing is cached between attempts: signed URLs expire, so a retry an hour
later asks for a new one.

By hand, against a sidecar written beside the MP4:

    ./upload-clip.py --url http://host:4000 --credential-file ./cred \\
        --clip /var/lib/porchlight/spool/<eventId>.mp4
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

TIMEOUT = 60


def note(text):
    sys.stderr.write(f"upload: {text}\n")
    sys.stderr.flush()


def fail(text):
    note(text)
    raise SystemExit(1)


def request_json(url, credential, payload=None, method="GET"):
    body = json.dumps(payload).encode() if payload is not None else None
    headers = {"Authorization": f"Bearer {credential}"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        raw = response.read()
        return json.loads(raw) if raw else {}


def put_bytes(url, headers, path):
    with open(path, "rb") as handle:
        data = handle.read()
    # Exactly the headers step 1 handed back. The signature covers them, so an
    # extra one here does not add metadata - it makes the upload fail.
    request = urllib.request.Request(url, data=data, headers=dict(headers), method="PUT")
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        return response.status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--credential-file", required=True)
    parser.add_argument("--clip", required=True, help="path to the .mp4")
    options = parser.parse_args()

    clip = options.clip
    sidecar = os.path.splitext(clip)[0] + ".json"
    base = options.url.rstrip("/")

    if not os.path.exists(clip):
        fail(f"{clip} is gone")
    try:
        with open(options.credential_file) as handle:
            credential = handle.read().strip()
    except OSError as error:
        fail(f"cannot read credential: {error}")
    try:
        with open(sidecar) as handle:
            metadata = json.load(handle)
    except (OSError, ValueError) as error:
        # Without it there is nothing to confirm with, and a clip the server
        # cannot describe is worse than one it never received.
        fail(f"cannot read sidecar {sidecar}: {error}")

    event_id = metadata.get("eventId") or os.path.splitext(os.path.basename(clip))[0]
    metadata = {key: value for key, value in metadata.items() if key != "eventId"}
    metadata["bytes"] = os.path.getsize(clip)

    try:
        grant = request_json(f"{base}/api/clips/{event_id}/upload-url", credential,
                             payload={}, method="POST")
    except urllib.error.HTTPError as error:
        fail(f"step 1 failed: HTTP {error.code} {error.read().decode(errors='replace')}")
    except urllib.error.URLError as error:
        fail(f"step 1 failed: {error.reason}")

    put_url = grant.get("url")
    if not put_url:
        fail(f"step 1 returned no url: {grant}")

    try:
        status = put_bytes(put_url, grant.get("headers", {}), clip)
        note(f"step 2 uploaded {metadata['bytes']} bytes, HTTP {status}")
    except urllib.error.HTTPError as error:
        fail(f"step 2 failed: HTTP {error.code} {error.read().decode(errors='replace')}")
    except (urllib.error.URLError, OSError) as error:
        fail(f"step 2 failed: {error}")

    try:
        request_json(f"{base}/api/clips/{event_id}/confirm", credential,
                     payload=metadata, method="POST")
    except urllib.error.HTTPError as error:
        fail(f"step 3 failed: HTTP {error.code} {error.read().decode(errors='replace')}")
    except urllib.error.URLError as error:
        fail(f"step 3 failed: {error.reason}")

    note(f"confirmed {event_id}")


if __name__ == "__main__":
    main()
