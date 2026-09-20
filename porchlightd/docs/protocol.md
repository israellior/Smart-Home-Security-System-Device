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

## A trap in the current server

`server.js` treats a second `hello` with `role: 'pi'` as a replacement and closes
the first socket ([server.js:81-90](../../server.js#L81-L90)). `webrtc-video.py`
already signs in that way. If `porchlightd` also said `role: 'pi'` it would kick
the media script off its socket every time it reconnected.

Anything that is not `'pi'` is currently filed as `'browser'`, so the daemon
cannot sign in correctly at all until the server learns a third role. The
proposal is `role: 'device'`: one per `deviceId`, replaces its own predecessor,
and is never a signaling target for an offer.

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

```
PUT /clips/<eventId>
Authorization: Bearer <device credential>
Content-Type: video/mp4
```

Metadata rides in the request rather than in a second call, so an upload is
self-describing and does not depend on its alert having arrived first:

```
X-Porchlight-Device: porch-1
X-Porchlight-Kind: ring
X-Porchlight-At: 2026-09-20T17:05:31.412Z
X-Porchlight-Duration-Ms: 15000
X-Porchlight-Partial: true
```

`partial` is set when a viewer arrived and cut the recording short. The clip is
playable either way — the recorder always finishes with EOS — but it is not the
length that was asked for, and a UI should say so.

These same five fields are written to `<eventId>.json` beside the MP4 in the
spool, which is what lets a restart re-queue a clip it has never seen before.
The sidecar is the source of truth; the headers are derived from it.

A `2xx` deletes both files. Anything else leaves them for the next attempt.

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
