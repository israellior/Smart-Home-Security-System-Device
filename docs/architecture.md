# The three flows, as built

What actually happens today for **alerts**, **clips** and **live stream** —
across `porchlightd` on the Pi, the app server, and the browser. Written from
the code, not the plan.

Companion to [server-brief.md](server-brief.md), which is the contract. This
one is the implementation.

---

## The two rules everything else follows from

**1. The app server moves JSON and nothing else.** It never carries a video
frame — LiveKit copies the stream out. It never carries a clip byte — the Pi
PUTs straight to R2 under a pre-signed URL. The server mints credentials and
records facts.

**2. Two channels, because they fail differently.**

| | carries | transport | why |
|---|---|---|---|
| `ServerLink` | alerts out, viewer requests in | one long-lived WebSocket | must arrive in under a second, tiny |
| `ClipUploader` | the MP4 | a separate HTTPS request per clip | megabytes, can wait |

On one channel, an upload would delay a doorbell press.

The device holds **exactly one socket**, owned by the daemon, signed in as
`role: "device"`. `webrtc-video.py` has no socket at all — it fetches tokens
over HTTP and talks to LiveKit directly.

---

## Flow 1 — Alerts

### On the Pi

The core decides; the backends only report. Two kinds, `motion` and `ring`.

```
GPIO 23 (PIR)    ─→ MotionDetected ─┐
GPIO 24 (button) ─→ ButtonPressed  ─┴→ core ─→ chime, LED, recording, alert
```

Dedup happens before an alert is ever queued:

| rule | value | what it stops |
|---|---|---|
| `motion_cooldown_seconds` | 25 | a PIR retriggering on the same person |
| `press_dedup_window_seconds` | 5 | one visitor pressing four times |

**A press during a motion recording upgrades that same event.** One `eventId`,
one clip, one row — the device sends a second `event` frame with the same id
and `kind: "ring"`.

### The queue that survives an outage

`AlertQueue` is bounded and ages out, so a Pi that was offline overnight does
not flood the server at breakfast:

| | value |
|---|---|
| capacity | 200 alerts |
| max age | 24 h |
| backoff | 2 s → 60 s |

Two details that matter more than they look:

- **The in-flight entry is never dropped or expired.** Both `push` (at
  capacity) and `expire` skip the first entry when something is in flight,
  because dropping it would strand its result and the outcome would never be
  applied.
- **`ok: false` is permanent and no-ack is retryable.** A rejection makes the
  device drop that alert forever; silence makes it retry. So the server must
  answer nothing at all during a deploy or a database blip — answering "no"
  loses a real doorbell press.

### On the server

`ingestEvent` is the **single** entry point for both transports — the device
socket and the HTTP endpoint the app and tests use. That is deliberate: two
implementations of "raise the kind, never lower it" is how a ring silently
becomes a motion on whichever path got it wrong.

```
event frame ─→ ingestEvent ─┬→ CREATED   → notify
                            ├→ UPGRADED  → notify
                            ├→ DUPLICATE → silent
                            └→ REJECTED  → ok:false, device drops it
```

The write is one round trip:

- `findOneAndUpdate` upsert on `{ device, eventId }`
- `$min` on `at`, so whichever of the motion and the ring lands first, the
  event keeps the **earliest sensor time**. Arrival order is irrelevant.
- The upsert deliberately does **not** raise the kind. Reading the stored rank
  back unchanged is what lets the upgrade be a compare-and-set:
  `{ _id, kindRank: { $lt: rank } }`. A stale ring changes nothing, and two
  concurrent upgrades produce one winner rather than two notifications.
- A duplicate-key race (`11000`) is not a failure — both writers describe the
  same press, so the loser reads the winner's row and carries on.

**The ack names the kind that was *sent*, not the kind now stored.** The device
dedupes its outbox by `(eventId, kind)`, so a motion acknowledged as "ring"
because a later press upgraded the record would leave the motion looking unsent
forever.

### Fan-out

`notify()` fires on **create and upgrade only**, never on a duplicate — that is
part of the dedupe rule, not something each transport is trusted to remember.
A ring is new information even when the motion was already announced.

Two consumers, in order of cost:

1. **`eventBus`** — in-process, synchronous. The signaling layer pushes to any
   browser already watching. Cheap, and the person with the screen open is who
   the notification was trying to reach anyway.
2. **`dispatchEventNotifications`** — web push and email, **not awaited**.
   Recording that motion happened is the job; telling people is best-effort on
   top. A doorbell gets its ack even while a push provider times out.

Delivery is opt-in per person per doorbell (`notifMotion` / `notifRing` on the
membership), and the query cost is **fixed at three queries** regardless of how
many members a doorbell has — memberships, then all their subscriptions in one
`$in`, then one `insertMany` for the delivery log. The obvious per-recipient
loop would be `2N+1`.

---

## Flow 2 — Clips

### Recording

A `gst-launch-1.0` child process, watched through a pidfd. `libcamerasrc` →
`x264enc` → `mp4mux`, plus `alsasrc` → AAC, both through `queue` elements —
without them the capture thread does the encoding and the card overruns.

- `clip_seconds` 15, `min_clip_seconds` 2
- **A viewer arriving cuts the recording short** and the clip uploads anyway,
  flagged `partial: true`. Live view outranks recording because the camera and
  the sound card go to one process at a time.
- Shutdown sends SIGINT so the muxer writes its `moov` atom; a killed muxer
  leaves an unplayable file.
- `ok = clean_exit && bytes >= 1024` — a size floor and nothing more. This does
  **not** verify the video is good, which is a known gap.

### Upload — three steps, and only the third one counts

```
1. POST /api/clips/<eventId>/upload-url     Bearer <device credential>
      → { url, method: PUT, headers, expiresAt }     signed, 900 s

2. PUT <url>                                → straight to R2; the server sees nothing

3. POST /api/clips/<eventId>/confirm        Bearer <device credential>
      → { deviceId, kind, at, durationMs, partial, bytes }
```

- **Step 2 sends exactly the headers step 1 returned and nothing else.** The
  signature covers them, so an extra `X-Porchlight-*` header does not add
  metadata — it makes the upload fail. That is why the metadata moved to step 3.
- **The server re-reads the object's real size from the bucket on confirm**
  rather than believing the reported `bytes`.
- `Clip.status` goes `pending` → `stored`.
- **Only a 2xx on step 3 lets the device delete its local copy.** A PUT that
  succeeded and was never confirmed is a clip the server does not know about,
  so the Pi keeps it and retries.
- `upload-clip.py` is judged by its **exit code alone**; 0 means the confirm
  answered 2xx.

A confirm with no matching alert **creates** the event record rather than
failing — a clip may arrive before its alert, or with no alert ever.

The spool is capped (512 MB) so a long outage cannot fill the card.

### Playback

The server returns a **signed R2 GET URL** (900 s), which goes straight into
`<video src={clip.url}>`. The bytes never touch the app server in either
direction — the same rule as live media.

---

## Flow 3 — Live stream

### Starting a call

Two things can nudge the device, and the token request is the more reliable:

```
POST /api/devices/<id>/live   → mints the viewer token
                              → AND calls requestViewer() if the device is online

browser WebSocket "watch"     → also calls requestViewer()
```

Asking for a token *is* the intent to watch, so the nudge lives there rather
than depending on the browser's socket being up. The response carries
`deviceOnline`, so the UI can say "your doorbell is offline" instead of showing
a black rectangle and letting the viewer conclude it is broken.

```json
{ "type": "viewer-requested", "peer": "<userId>" }
```

**This is a cue, not an offer.** There is no SDP on this socket in either
direction, no ack, and no "call ended". `peer` is kept by the daemon only to
know which viewer a later internal `CallEnded` belongs to, and is never passed
to the media process.

The daemon **stops any recording first** and waits for its EOS, then spawns
`webrtc-video.py`. One media process serves every viewer — the Pi publishes one
stream and LiveKit copies it out, so a second `viewer-requested` is a no-op.

### Two tokens, two connections, one process

```
libcamerasrc → I420 → appsink ─┐
                                ├→ publisher token → LiveKit → viewers
alsasrc → webrtcdsp → appsink ─┘

alsasink ← webrtcechoprobe ← appsrc ←── listener token ← LiveKit ← a viewer
```

| token | identity | grants |
|---|---|---|
| publisher | `device:<id>:pub` | `canPublish` limited to `[CAMERA, MICROPHONE]`, **`canSubscribe: false`** |
| listener | `device:<id>:sub` | `canSubscribe`, `canPublish: false` |
| viewer | `user:<userId>` | `canSubscribe`, `canPublish: false` |
| viewer talking | `user:<userId>` | `canPublish` limited to `[MICROPHONE]`, `canSubscribe` |

- `canSubscribe: false` is a claim **inside the token that LiveKit rejects on**,
  not a policy the app server enforces. A leaked publisher token cannot be
  turned into a way to watch the house.
- `canPublishSources` supersedes `canPublish` in LiveKit, so these grants are
  whitelists rather than open permissions.
- **TTL 120 s, `nbf` = mint time.** Fetched fresh on every connection attempt
  and never cached — a token is needed to join and for nothing after, so expiry
  needs no handling beyond not keeping one.
- **The Pi appears as two participants.** Anything counting who is in the room
  must skip identities starting `device:`, or the Pi looks like its own
  audience and the call never ends.

### Push-to-talk

Last press wins, revoke before grant, so two microphones are never live at once.

**Permissions change on the session that is already up**, through LiveKit's
room admin API — not by issuing a new token. A token swap would cost a second
of dead air every time somebody spoke.

A 200 from `POST /talk` means the viewer really may publish; if the grant did
not land it answers 502 and leaves the floor free, because the browser treats
success as permission to open the microphone. A 60 s TTL covers the viewer who
closes their laptop mid-turn and never releases.

### Ending

**The call ends itself.** The listener connection is the only half that can see
who is in the room, so it decides: `--linger` (3 s) after the last viewer
leaves, or `--idle-timeout` (30 s) if nobody ever arrived. The process exits,
and `porchlightd` learns that from a pidfd. There is no "call ended" message on
any socket.

---

## What is held in memory

All of it per-device, all of it single-process, and all of it named in the code
as such:

| | where | what breaks with two instances |
|---|---|---|
| socket registry | `signaling/registry.js` | a viewer on A cannot reach a device on B |
| talk floor | `services/media/talkFloor.js` | two floors, two live microphones |
| presence reset | `signaling/index.js` | one instance booting wipes the other's connections |

This is the constraint to resolve before running more than one app server.

---

## Known gaps

- **A doorbell press during a live view will not chime** — the call holds the
  sound card and `AlsaChime` cannot open it. The daemon's own rule is that the
  chime never waits for anything, and this quietly breaks it.
- **Live view has not been exercised end to end on hardware.** Alerts and clips
  are confirmed; the watch path is not.
- **`ok=true` on a recording proves almost nothing** — clean exit and >1 KB. A
  clip with a broken video track passes.
- **Some clips report a nonsense duration in the player** while
  `gst-discoverer-1.0` reads them as a correct ~15 s on the Pi. Unresolved
  whether the file or the playback path is at fault.
