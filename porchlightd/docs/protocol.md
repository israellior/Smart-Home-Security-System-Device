# Device protocol (proposal)

What `porchlightd` needs from the app server. **Nothing here is implemented.**
`server.js` has no event message and no upload endpoint, and this task does not
touch it. This file exists so the daemon's offline queue and its retry rules are
written against a decided shape rather than an imagined one.

Two channels, because they have different failure modes:

| | carries | transport |
|---|---|---|
| `ServerLink` | alerts out, viewer requests in | the signaling WebSocket, already open |
| `ClipUploader` | the MP4 | a separate HTTPS request per clip |

An alert must arrive in under a second and is small. A clip is megabytes and can
wait. Putting them on one channel would let an upload delay a doorbell.

## The role split, and why it is a trap

Three roles share `/signal`. They present the **same credential** and differ
only by that one string.

| role | who | what it does |
|---|---|---|
| `device` | `porchlightd` | sends alerts, receives viewer requests |
| `pi` | `webrtc-video.py` | carries the call |
| `browser` | a viewer | watches |

Replacement is scoped **per role**, so a reconnecting daemon displaces only the
previous daemon. Give the daemon `'pi'` and every reconnect silently closes the
media script's socket — which presents as video that randomly stops working
rather than as an authentication error, and so gets debugged in entirely the
wrong place.

Worse: **only `device` may send events**, and from any other role they are
*silently ignored*. Silence is also how the server signals a transient
failure, so a misconfigured role means retrying forever and never succeeding.

`webrtc-video.py` already sends `'pi'`
([line 544](../../pi/webrtc-video.py#L544)); the daemon sends `'device'`.

## The credential

Written onto the card when the device is built. There is no enrolment call and
no way to read it back, so a lost credential means the device is re-minted and
re-flashed. The daemon only ever reads it — as `Authorization: Bearer` on HTTP,
and as `token` in `hello`.

The device is never told who owns it and does not need to know. It reports; the
server decides who hears about it.

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

The daemon does not answer this itself. It stops any recording, then tells
`webrtc-video.py` to take the call over the local socket that is out of scope
here. The reply travels on the media script's own signaling socket, not this one.

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
