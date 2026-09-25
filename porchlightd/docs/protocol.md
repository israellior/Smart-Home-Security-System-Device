# Device protocol

What `porchlightd` needs from the app server, and what it now speaks to a real
one. `server.js` is **not** that server: it has no event message and no upload
endpoint, and never will — it is the LAN development stub. The real app server
implements everything below.

Two channels, because they have different failure modes:

| | carries | transport |
|---|---|---|
| `ServerLink` | alerts out, viewer requests in | the signaling WebSocket, already open |
| `ClipUploader` | the MP4 | a separate HTTPS request per clip |

An alert must arrive in under a second and is small. A clip is megabytes and can
wait. Putting them on one channel would let an upload delay a doorbell.

## The role split, and what became of it

| role | who | what it does |
|---|---|---|
| `device` | `porchlightd` | sends alerts, receives viewer requests |
| ~~`pi`~~ | ~~`webrtc-video.py`~~ | **nothing — the media script has no socket now** |
| `browser` | a viewer | watches |

**The device holds exactly one socket, and the daemon holds it.** When the call
moved to LiveKit, `webrtc-video.py` stopped exchanging SDP with anybody: it
POSTs for a token with the device credential and negotiates with LiveKit
directly. So the `pi` role has no client on this device any more, and the trap
the rest of this section described — a reconnecting daemon claiming `'pi'` and
silently closing the media script's socket — can no longer happen because there
is no second socket to close.

It is still worth the server keeping the roles apart. **Only `device` may send
events**, and from any other role they are *silently ignored*; silence is also
how the server signals a transient failure, so a misconfigured role means
retrying forever and never succeeding. That is the same failure it always was.

`pi/server-bridge.py` sends `'device'`, which is the only `hello` this device
sends at all.

## The credential

Written onto the card when the device is built. There is no enrolment call and
no way to read it back, so a lost credential means the device is re-minted and
re-flashed. The daemon only ever reads it — as `Authorization: Bearer` on HTTP,
and as `token` in `hello`.

The device is never told who owns it and does not need to know. It reports; the
server decides who hears about it.

`pi/provision.sh` is what writes it, and it checks two things nothing else
does: that the credential names **this** device (`pl_<deviceId>_...`, so
porch-2's credential cannot be installed on porch-1 and discovered as a
rejected credential an hour later), and that the file ends up owned by the user
the unit runs as. A credential at 0600 owned by whoever typed it is unreadable
to the daemon, and that fails as an authentication error rather than as a
permission one.

The daemon checks it can read the file before it starts the bridge at all, for
the same reason: a bridge that exits on a missing credential exits instantly,
every time, and a restart loop over that is a busy CPU and a log that says
nothing.

## Connecting for the first time

Three questions, in the order they fail, each isolating a different half. In
any other order a socket that will not open is indistinguishable from a
credential that is refused:

```bash
server-bridge.py --url URL --device-id ID --credential-file F --check
server-bridge.py --url URL --device-id ID --credential-file F --probe
webrtc-video.py  --url URL --device-id ID --credential-file F --check
```

`--check` is one HTTPS request against `/api/devices/<id>/self`: it proves the
credential, the URL and the route, and touches no socket. `--probe` opens the
real signaling socket, says hello and waits for `hello-ok`, which proves the
thing the daemon actually depends on. **A network can carry the first and drop
the second** — a proxy that allows HTTPS and not a WebSocket upgrade is
otherwise diagnosed as "the daemon just sits there". The third proves the two
LiveKit token endpoints with no camera involved.

`pi/provision.sh` runs all three at the end of an install and refuses to enable
the unit until the first two pass.

## Close codes

| code | meaning | what the device does |
|---|---|---|
| 4001 | replaced by another connection of your role | stop; looping would fight a duplicate |
| 4002 | credential rejected | **permanent** — stop, log loudly, re-mint |
| 4003 | no hello within 10 s | our bug; reconnect |
| 1013 | transient server failure | reconnect with backoff |
| other | — | reconnect with backoff and jitter |

The server pings at the protocol level every 30 s and drops anything that
missed the previous one. The library answers those automatically **from inside
its read loop**, so nothing else may block that loop — if it does, the symptom
is a flaky network rather than a stuck reader.

The bridge pings the other way every 20 s with a 10 s timeout, so it notices a
dead link before the server does.

**4001 and 4002 stop the device until a person arrives.** Both set the LED to
the fault pattern — lit with two short gaps, the exact negative of the ordinary
offline blink — because the difference between them and a flat router is whether
waiting helps. That makes the codes load-bearing on the server side too:
anything transient must be 1013, never 4002, or a doorbell needs a site visit
to come back. See [app-server-changes.md](../../docs/app-server-changes.md).

**A hello that is never answered is its own failure.** The server closes a
socket that has not said hello within ten seconds; the bridge closes one whose
hello has not been answered within ten. An open TCP connection that nothing is
reading looks exactly like a working one, and without the second rule the
daemon waits on it forever.

**The bridge is restarted with a backoff**, `server.restart_backoff_*`, two
seconds doubling to sixty and reset by a connection that reached `hello-ok`.
The bridge reconnects by itself, so it exiting never means the network went
away — it means the bridge could not run, and that class of fault recurs
instantly.

## Alerts

Device to server, on the WebSocket:

```json
{
  "type": "event",
  "deviceId": "porch-1",
  "eventId": "0f4b9c2e-8a21-4f6d-9c3a-2b7e5d1a6f80",
  "kind": "motion",
  "at": "2026-09-20T17:05:31.412Z"
}
```

`kind` is `motion` or `ring`. `at` is when the sensor fired, **not** when the
message was sent — a queued alert keeps its original timestamp, so a delivery
after ten minutes offline still says when the motion happened.

The server replies, and the daemon retries until it gets one:

```json
{ "type": "event-ack", "eventId": "0f4b...", "kind": "motion", "ok": true }
```

`ok: false` means the server understood the message and rejected it (unknown
device, bad credential). The daemon drops that alert rather than retrying — a
retry would fail the same way forever. A missing ack is the retryable case.

### Dedupe, and the ring upgrade

The server keys on **`(eventId, kind)`**, not on `eventId` alone. A repeated
`(eventId, kind)` is ignored, which is what makes the daemon's retries safe.

A button pressed during a motion recording upgrades that event. The daemon sends
a second `event` with the **same `eventId`** and `kind: "ring"`, and the server
raises the stored kind:

- `motion` then `ring` → the event becomes a ring
- `ring` then `motion` → ignored, never a downgrade

One event, one clip, one row — notified as a ring, because someone was at the
door. The two alerts may arrive in either order after an outage, so the rule is
written as a maximum rather than a sequence.

## Viewer requests

Server to device, unchanged in spirit from today's `request-offer`:

```json
{ "type": "viewer-requested", "peer": 7 }
```

The daemon does not answer this at all, and there is nothing it could answer
with: there is no offer here and no SDP anywhere on this socket.

It stops any recording — the camera and the sound card go to one process at a
time, and the recorder has both — and once the recorder has flushed its EOS it
runs `webrtc-video.py`, which fetches its own LiveKit tokens and joins the
room. `peer` is never passed on; the script joins a room, not a peer. It is
kept only so the daemon can tell the core which viewer a `CallEnded` belongs
to.

**Nothing comes back on this socket when the call ends.** The script is the
half that can see who is in the room, so it decides when the call is over and
exits; the daemon learns that from a pidfd. A second `viewer-requested` while
a call is running is nothing to act on — the Pi publishes one stream and
LiveKit copies it out to as many viewers as there are.

## Clip upload

Three steps. Any failure restarts from the first — the device never resumes a
sequence from the middle.

### 1 — ask for somewhere to put it

```
POST /api/clips/<eventId>/upload-url
Authorization: Bearer <device credential>
```

```json
{
  "url": "https://<bucket>/<opaque path>?<signature>",
  "method": "PUT",
  "headers": { "Content-Type": "video/mp4" },
  "expiresAt": "2026-09-20T17:20:31.412Z"
}
```

**A fresh URL is requested on every attempt.** The device never stores one and
never reuses one — signed URLs expire, so a retry an hour later needs its own.

### 2 — send the bytes, and only the bytes

```
PUT <url>
Content-Type: video/mp4
```

Exactly the headers step 1 handed back, and nothing else. The signature covers
the headers, so an extra `X-Porchlight-*` here does not add metadata — it makes
the upload fail. That is why the metadata lives in step 3.

No device credential travels here either: the URL carries its own
authorisation, and may point somewhere that has never heard of this device.

### 3 — confirm

```
POST /api/clips/<eventId>/confirm
Authorization: Bearer <device credential>
Content-Type: application/json
```

```json
{
  "deviceId": "porch-1",
  "kind": "ring",
  "at": "2026-09-20T17:05:31.412Z",
  "durationMs": 15000,
  "partial": true,
  "bytes": 2310584
}
```

`partial` is set when a viewer arrived and cut the recording short. The clip is
playable either way — the recorder always finishes with EOS — but it is not the
length that was asked for, and a UI should say so.

**The confirm is what makes a clip real.** A bucket PUT that succeeds and is
never confirmed is a clip the server does not know exists — treat it as absent
and sweep it on a timer, because the device is probably about to send the same
bytes again under a new URL.

Steps 1 and 3 are both idempotent per `eventId`. Re-confirming is exactly how a
device that died between the PUT and the confirm recovers.

Because the body carries everything about the event, an upload still does not
depend on its alert having arrived first.

## What the device keeps, and until when

That confirm body is exactly the `<eventId>.json` sidecar written beside the
MP4 in the spool. The sidecar is the source of truth; the request is a copy of
it. It is also what lets a restart re-queue a clip it has never seen before.

**The MP4 and its sidecar are deleted on a `2xx` from step 3 — never from step
2.** Anything else, at any step, keeps both, and the next attempt starts again
at step 1. `UploadFinished{ok}` inside the daemon means *the confirm
succeeded*, not that the bytes were sent.

The cost of that rule is re-uploading a file whose PUT already worked. The
alternative is deleting a clip the server never recorded, which is
unrecoverable, so the trade is not close.

## Uploads wait for a live call to end

The device does not **begin** an upload while someone is watching. A home
connection has one upstream link, and the live stream is what a person is
waiting on — spending it on a clip from ten minutes ago would degrade the call
they opened to look at.

An upload already in flight when a call starts is left to finish; there is no
way to unsend it, and one clip's remainder is a bounded cost.

## When the server is unreachable

Alerts are held in memory, in a bounded queue:

- **Bounded** — at the limit the **oldest** is dropped. A doorbell that has been
  offline for an hour should deliver the last few events, not the first few.
- **Aged out** — an alert older than the configured limit is dropped unsent. An
  alert about motion from yesterday is noise, and the clip still uploads.
- **Backoff** — exponential from an initial delay to a cap, reset by a success.

Clips are not queued in memory. They are already on disk in the spool, and the
spool is scanned at startup, so an upload retry survives a restart while a
pending alert does not. That asymmetry is deliberate: an undelivered alert has a
short useful life, and an undelivered clip does not.

The chime never waits for any of this. It is a local effect and plays on a
button press whether or not the server has ever been reachable.
