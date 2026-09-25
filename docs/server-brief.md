# Brief for the app server

Copy this into the server repository. It is what the device side already
expects, written for whoever builds the other end.

**There is a follow-up: [app-server-changes.md](app-server-changes.md).** This
document is the contract; that one is the list of changes, written later and
against the real server rather than from this specification, covering a
device's first connection and what has to happen when the network comes back.
Where the two overlap they agree; where one is more specific, it is that one.

Nothing here describes code you have. It describes code that **already exists,
runs on real hardware, and is written against the contract below** — so where
this document and a future design disagree, this one is the constraint.

## The product

A private video doorbell. A Raspberry Pi with a camera, microphone and speaker,
watched and talked to from a browser anywhere in the world.

```
   ┌────────────┐        ┌──────────────────────┐        ┌───────────┐
   │     Pi     │        │   app server (VPS)   │        │  browser  │
   │  at a home │◄──────►│  accounts, groups,   │◄──────►│  anywhere │
   │            │  wss   │  tokens, signaling,  │  wss   │           │
   └─────┬──────┘        │  alerts, clips       │        └─────┬─────┘
         │               └──────────────────────┘              │
         │               ┌──────────────────┐                  │
         └──────────────►│       SFU        │◄─────────────────┘
            one copy up  │  copies it out   │   one copy each
                         └──────────────────┘
```

**You are building the app server.** It never carries media — the SFU does.
Both the Pi and the browsers connect *outward* to the SFU, so no home router
needs to accept an incoming connection and no TURN relay is required.

## What already exists

**`porchlightd`** — a C++20 daemon on a Pi 4, verified on hardware. It owns
every decision about when to alert, record, chime and call. It currently fakes
the server link and the uploader, which is the gap you are filling.

What it already does, and you do not need to reimplement:

- Motion and button handling, with a cooldown and a press de-dup window
- A button press during a motion recording **upgrades that same event** to a
  ring — one event, one clip, one row
- Records H.264 + AAC into an MP4, 10–20 s, and holds it in a spool
- A live viewer takes priority: it stops a recording early and uploads the
  partial clip
- An LED priority order, a chime that plays even when you are unreachable
- A bounded offline alert queue with exponential backoff

**`webrtc-video.py`** — the live call, on the same Pi, a separate process that
`porchlightd` starts. Camera and microphone up, a viewer's microphone back down
to the speaker, with echo cancellation between them because they are one sound
card. **It has no WebSocket to you.** It POSTs for its two LiveKit tokens with
the device credential and negotiates with LiveKit directly.

**`server.js`** — a LAN-only signaling stub with no auth. It stays in the
device repo as a development tool. **You are replacing it**, not extending it.

## The contract the device already implements

Two channels, because they fail differently. An alert must arrive in under a
second and is tiny; a clip is megabytes and can wait.

| | carries | transport |
|---|---|---|
| ServerLink | alerts out, viewer requests in | the signaling WebSocket |
| ClipUploader | the MP4 | a separate HTTPS request per clip |

### Alerts — device to server, on the WebSocket

```json
{
  "type": "event",
  "deviceId": "porch-1",
  "eventId": "0f4b9c2e-8a21-4f6d-9c3a-2b7e5d1a6f80",
  "kind": "motion",
  "at": "2026-09-20T17:05:31.412Z"
}
```

`kind` is `motion` or `ring`. **`at` is when the sensor fired, not when the
message was sent** — an alert held through a ten-minute outage still reports
when the person was at the door. Do not substitute receipt time.

You must answer, or the device retries forever:

```json
{ "type": "event-ack", "eventId": "0f4b…", "kind": "motion", "ok": true }
```

### Four rules you cannot get wrong

**1. Dedupe on `(eventId, kind)`, never on `eventId` alone.** Retries are
normal and the same pair will arrive repeatedly.

**2. `motion` → `ring` is an upgrade; `ring` → `motion` is ignored.** The
device sends a second `event` with the *same* `eventId` and `kind: "ring"` when
someone presses the button during a motion recording. Raise the stored kind and
never lower it. The two may arrive in either order after an outage, so write it
as a maximum, not a sequence.

**3. `ok: false` is permanent.** The device **drops that alert and never
retries it**. Return it only for an unrecoverable rejection — unknown device,
bad credential. For anything transient (your database is down, a deploy is in
progress) **send no ack at all**; a missing ack is the retryable case.

**4. Order independence.** A clip may arrive before its alert, or with no alert
ever. Do not make the upload depend on the event record existing.

### Viewer requests — server to device

```json
{ "type": "viewer-requested", "peer": 7 }
```

The device stops any recording first — the camera and the sound card go to one
process at a time and the recorder holds both — and then starts
`webrtc-video.py`, which fetches its two tokens and joins the room.

**Nothing comes back on this socket, for this message, ever.** There is no SDP
on it in either direction, no ack, and no "call ended". The device decides when
the call is over, from the room, and simply stops publishing.

**`peer` is never sent anywhere.** The device keeps it only to know which
viewer a later internal `CallEnded` belongs to. Send whatever identifies the
viewer to you; the device treats it as an opaque string and a JSON number is
fine.

**Send it once per viewer.** A second one while a call is already up is a
no-op on the device, which is correct: the Pi publishes one stream and LiveKit
copies it out. You do not need to suppress them, and you do not need to send
one when a second viewer joins a call that is already running — though it costs
nothing if you do.

### Clip upload

**Changed from the first issue of this brief.** The device's uploader was not
written yet, which is the only reason this shape was still free to move. Clips
now go to object storage directly. The app server never carries the bytes —
the same reason it never carries media.

Three steps.

**1. Ask for somewhere to put it.**

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

**2. Upload the bytes, and only the bytes.**

```
PUT <url>
Content-Type: video/mp4
```

Send exactly the headers step 1 handed back, and nothing else. The URL is
signed and the signature covers the headers — an extra `X-Porchlight-*` header
here does not add metadata, it makes the upload fail. That is why the metadata
moved to step 3.

**3. Confirm, with the metadata in the body.**

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

The same fields the headers used to carry, with the same meanings. `at` is
still sensor-fire time, not upload time. `partial` still means a viewer arrived
and cut the recording short — the clip is playable either way, but it is not
the length that was asked for and a UI should say so.

The confirm is what makes the upload self-describing, so **rule 4 still
holds**: a confirm with no matching alert creates the event record rather than
failing.

**A `2xx` on step 3 — not on step 2 — lets the device delete its local copy.**
A bucket PUT that succeeds and is never confirmed is a clip the server does not
know exists. Anything else, at any step, leaves the file for the next attempt,
and that attempt restarts at step 1: signed URLs expire, so a retry an hour
later needs a fresh one.

Steps 1 and 3 are both idempotent per `eventId`. Re-confirming is exactly how a
device that crashed between the PUT and the confirm recovers.

### What the uploader actually sends

The uploader exists: `pi/upload-clip.py` in the device repo, written against the
three steps above and judged purely by its exit code. What it does is therefore
a constraint, and some of it is not visible from the shapes alone.

- **Step 1 carries no body and no `deviceId`.** The body is literally `{}` — it
  must not require any fields. The device comes from the credential and the
  event from the path, so an `eventId` you have never seen has to be accepted
  and bound to the authenticated device on first use, and refused if it is
  already bound to a different one.
- **`url` is the only response field the device needs.** `headers` is sent
  verbatim on the PUT; `method` and `expiresAt` are ignored — it always PUTs,
  and it never caches a grant between attempts.
- **The PUT is Python's `urllib`**, so it also sends `Host`, `User-Agent:
  Python-urllib/3.x`, `Accept-Encoding` and `Content-Length`, and no checksum
  header at all. None of those may appear in the signature's signed headers.
  Anything you want stored as object metadata must come back in `headers` at
  step 1, because nothing else is added there.
- **Only a 2xx on the confirm counts**, and an empty `200` or a `204` is fine.
  Anything else, at any step, leaves the file on the SD card for the next
  attempt — and that attempt starts again at step 1.
- **`kind` arrives on the confirm as well as on the alert**, so it needs the
  same maximum rule: a late `motion` confirm must never downgrade an event
  already raised to `ring`.
- A clip at the current settings — 640×480, 2 Mbps, 10–20 s — is roughly
  **2–4 MB**.

### Storage, and playback

The device never holds bucket credentials. It holds its own device credential,
and what step 1 mints is scoped to **one object, PUT only, short-lived**. A
stolen doorbell yields the ability to write one clip for a few minutes; it
cannot read anything, and it cannot delete anything.

The orphan case is the price of that: a PUT that succeeds and is never
confirmed leaves an object you do not know about. That is what step 3 is for,
and a lifecycle rule that deletes unconfirmed objects after a day closes it.

**Playback follows the same rule as the upload.** When the app shows a clip,
hand the viewer a short-lived signed GET and let the browser fetch from the
bucket. Proxying recordings through the app server would keep it out of the
live media path only to make it a video CDN for the recorded one.

**These endpoints return 404 today, and the device side is being written
against them now.** The shapes are agreed but not yet load-bearing on either
side, so if any of it is wrong for your storage — a field you need in the step 1
body, a different confirm shape — say so now rather than after both ends are
built.

## The roles, and a trap that has now closed

**The device holds exactly one socket, and `porchlightd` holds it**, as
`role: 'device'`: one per `deviceId`, replacing its own predecessor, never a
signaling target for an offer.

The earlier issue of this brief warned that giving the daemon `'pi'` would kick
the media script off its socket on every reconnect. That cannot happen any
more, because **the media script no longer has a socket**. The `'pi'` role has
no client on the device at all now and you can drop it if you have one.

What has not changed is that **only `device` may send events**, and that from
any other role they must be *silently ignored* rather than rejected — silence
is your transient-failure signal, and the device retries into it forever.

## Media — LiveKit Cloud

**Decided: LiveKit Cloud.** Managed, so there is no SFU to run, and its access
tokens already express per-participant permissions this product needs.

The app server's only job here is minting tokens — it is still never in the
media path. Every mint goes through one internal token interface,
`mintMediaToken(kind, { deviceId, identity })`, so moving to self-hosted
LiveKit or to mediasoup later changes one module and no callers.

One room per device. Three kinds of token, and the differences between them
are load-bearing:

| token | identity | canPublish | canSubscribe |
|---|---|---|---|
| **device publisher** | `device:<deviceId>:pub` | video + audio (camera and mic) | **false** |
| **device listener** | `device:<deviceId>:sub` | false | true |
| **viewer** | `user:<userId>` | audio only, active talker only | true |

**Why the Pi holds two tokens rather than one.** A single token carrying both
rights is one credential with the full set. Split, the publishing half
*literally cannot* subscribe: `canSubscribe: false` is not a policy the app
server enforces, it is a claim inside the token that LiveKit itself rejects on.
A leaked publisher token cannot be turned into a way to watch the house. The
split also lands cleanly on the two processes already running on the Pi.

**Why viewers are subscribe-only by default.** 1–5 viewers, typically one
talking. All of them watch and hear. Only the single active talker is minted a
token with `canPublish` for **audio and nothing else** — never video, never
screen share. Handing talk rights to five people at once turns a doorbell into
a conference call nobody asked for.

Who holds the talk floor is app server state: it decides, and it mints the one
talking token. A viewer whose turn ends is simply not minted another, and the
one they have expires on its own.

Tokens are short-lived — a minute or two — and scoped to one room. That, plus
the membership check at connect time, is what makes "removing someone from a
group revokes their access immediately" true rather than aspirational.

### Device-authenticated token endpoints

The Pi fetches its own two tokens with its own credential. No user is involved:

```
POST /api/devices/<deviceId>/media-token/publisher
POST /api/devices/<deviceId>/media-token/listener
Authorization: Bearer <device credential>
```

Each returns:

```json
{
  "token": "<livekit jwt>",
  "url": "wss://<project>.livekit.cloud",
  "room": "device-porch-1",
  "identity": "device:porch-1:pub",
  "expiresAt": "2026-09-20T17:07:31.412Z"
}
```

Two endpoints rather than one with a `role` parameter, deliberately: a
parameter is a thing that can be passed wrong, and these two have deliberately
different rights. The publisher endpoint has no path that mints a subscriber.

The viewer token has its own user-authenticated endpoint and is not the
device's concern.

**What this means for `webrtc-video.py`.** SDP no longer crosses the signaling
socket in either direction, and the script no longer has a socket to us at all.
It POSTs for both tokens with the device credential and negotiates with LiveKit
directly. `viewer-requested` on the ServerLink is unchanged: it is the cue to
stop recording and join the room, not an offer.

### What the device does with the two tokens — and what the viewer app must know

Both endpoints are called on **every connection attempt**, never cached: a
token is needed to join and for nothing afterwards, so one that went stale
while the Pi was offline is simply never used again. A reconnect mid-call mints
fresh ones. There is nothing to refresh and nothing to invalidate.

**The device joins the room twice, so it appears as two participants:**

| identity | publishes | subscribes |
|---|---|---|
| `device:<id>:pub` | camera + microphone | nothing — `canSubscribe: false` |
| `device:<id>:sub` | **nothing at all** | viewers' microphones |

That is the direct consequence of splitting the tokens, and it is right — but
it has one consequence outside the device:

**The viewer app must not treat `device:<id>:sub` as a publisher that has gone
quiet.** It joins, publishes nothing, ever, and stays for the whole call. A UI
that shows "Waiting for the camera…" until *some* participant publishes will
clear correctly; one that waits on *every* remote participant to publish will
wait forever. Key the live view on the `:pub` identity, or on a video track,
not on participant count.

The device applies the mirror image of this rule to decide when the call is
over: it counts participants whose identity does **not** start with `device:`,
and stops publishing a few seconds after the last one leaves.

Three notes on the token shape, none of them blocking:

- **`canPublishData: false` on both.** Nothing on the device wants a data
  channel, so this is right. It does mean there is no in-band way for a viewer
  to tell the Pi anything — if that is ever wanted, it has to come through you.
- **`nbf` is the mint time**, which puts it very slightly in LiveKit's future.
  go-jose's default leeway is a minute so it is harmless, but it does make the
  app server's clock load-bearing: if it ever drifts ahead by more than that,
  every token is rejected as not-yet-valid and nothing says why.
- **No `canPublishSources` on the device publisher**, so it could publish a
  screen share if it were ever asked to. Viewers are constrained to
  `["microphone"]` and the device is not constrained at all. Not worth
  changing unless you want the symmetry.

## Settled — treat as given

- Users have accounts and belong to **device groups**; a group grants access to
  particular Pis. Adding a user to a group is the only step needed to grant
  access, and removing them revokes it immediately.
- Access is carried by **tokens the app issues**: scoped to a single device,
  short-lived. The user installs and configures nothing.
- Each Pi authenticates with its **own device credential**, set at
  provisioning. A Pi cannot be impersonated.
- **Device secrets are stored hashed and shown exactly once.** The plaintext
  credential is displayed at provisioning and is unrecoverable afterwards —
  the server keeps only a hash, so a dump of the database does not let anyone
  speak as a doorbell. A credential that is lost is re-provisioned, never
  looked up. This holds for every secret the server issues to a device.
- The app server is **never in the media path**.
- 1–5 viewers per Pi, typically one talking. The Pi publishes one stream
  whatever the audience.
- All of a peer's tracks share one connection and one transport.

## Open — your calls to make

- **The app ↔ server API.** Entirely undefined and belongs with the app.
- Final video bitrate and resolution. 640×480 is the target.
- How the talk floor is handed over when two viewers both want it — explicit
  request, or last-press-wins. Either way the server mints one talking token.

Closed since the first issue of this brief, both of them written up above:
**the SFU is LiveKit Cloud**, behind a token interface, and **clips go to
object storage through pre-signed URLs with a confirm**. The clip decision
changes what the device must implement — see the rewritten upload section.

## Not your problem

- **Carrying media.** That is the SFU.
- **Any device-side policy** — cooldowns, de-dup windows, which clips are worth
  keeping, when to chime. All of that is implemented, tested and verified on
  hardware in the device repo. Do not re-litigate it server-side.
- **Echo cancellation.** It runs on the Pi, whose microphone and speaker are
  one sound card.

## One lesson worth carrying over

Three bugs in the device work passed every check on the development machine and
only appeared on the real Pi: a blocked signal mask that stopped every clip
being saved, missing pipeline queues that silently dropped a quarter of the
audio, and a compiler warning class that only appears in optimised builds.

A green build is not evidence that something works. Exercise the failure paths
deliberately — a device that is offline for an hour, an ack that never comes, a
clip whose alert was dropped for being too old — because the device does all of
those on purpose and you will meet them in production.
