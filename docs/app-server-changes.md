# App server changes: connecting, and connecting again

Written for whoever maintains the app server. It is a follow-up to
[`server-brief.md`](server-brief.md), not a replacement: everything there still
holds. This one covers the two paths that were never written down — **a new
doorbell's first connection**, and **what happens when the network goes away
and comes back** — and lists only what has to change on your side.

Most of what follows is small. The signaling layer is already close to right,
and several items below are *"this is correct, and here is why it must not be
simplified later"* rather than work. Each item is marked:

| | |
|---|---|
| **[required]** | the device now depends on it, or will misbehave without it |
| **[recommended]** | a real gap, but the device survives it |
| **[do not regress]** | already correct; the reason is not obvious from the code |

Nothing here needs a database migration or a new dependency.

## What changed on the device

So you know what you are matching. All of it is in the device repository and
none of it needs anything from you beyond what is below.

- **`pi/provision.sh`** — takes a Pi from a fresh OS install to a connected
  doorbell in one pass, and ends by proving the connection three ways. It is
  the caller for most of part 1.
- **`server-bridge.py --probe`** — opens the signaling socket once, says hello,
  waits for `hello-ok`, and exits non-zero on anything else. `--check` (the
  HTTP credential test) proves the credential; this proves the socket. A
  network can carry one and drop the other, and until now that failure had no
  name.
- **A hello watchdog.** If the socket opens and `hello-ok` does not arrive
  within ten seconds, the device closes it and reconnects. It is the mirror of
  your own `HELLO_TIMEOUT`, and it exists because an open TCP connection that
  nothing is reading looks exactly like a working one.
- **The bridge is restarted with a backoff**, 2 s doubling to 60 s, reset by a
  connection that actually reached `hello-ok`. It used to restart instantly,
  which for a bridge that dies on exec was a fork loop.
- **A distinct LED pattern for a permanent refusal.** Close code 4001 or 4002,
  or a missing credential, now shows as *lit with two gaps* instead of the
  ordinary offline *dark with two flashes*. The difference is whether waiting
  fixes it. See [B4](#b4-4002-stops-a-doorbell-until-someone-walks-up-to-it-required).
- **The call carries its own reconnect budget**, `--reconnect-timeout`, 60 s by
  default. A call rides out a bad minute rather than ending.

---

# Part 1 — from a boxed Pi to a first connection

The path today: mint a credential, write it onto the card, install, start, and
find out whether it worked by watching a log. The changes below make each of
those steps answerable.

## A1. `mint-device.mjs --json` [recommended]

`scripts/mint-device.mjs` prints the credential in a formatted block meant for
a human. Provisioning is a script now, and copying a secret out of a terminal
by eye is where it gets truncated.

Add a `--json` flag that writes one object to stdout and nothing else:

```json
{
  "deviceId": "porch-1",
  "credential": "pl_porch-1_...",
  "shareCode": "PORCH-7K2M9P",
  "record": "66f1...",
  "apiUrl": "https://porchlight.example"
}
```

`apiUrl` from whatever environment variable already holds the public origin;
omit the field if there is none rather than guessing. Keep the human output as
the default — the one-shot warning is the point of it.

On the Pi that becomes:

```bash
# on the server, once per device
node scripts/mint-device.mjs --device-id porch-1 --json > porch-1.json

# on the Pi
jq -r .credential porch-1.json > /tmp/cred
./pi/provision.sh --url https://porchlight.example --device-id porch-1 \
  --credential-file /tmp/cred --apply
shred -u /tmp/cred porch-1.json
```

The credential never reaches a command line either way: `provision.sh` refuses
to take one as an argument, because arguments are world-readable in `/proc` and
end up in shell history.

## A2. Say "never connected" differently from "offline" [recommended]

`Device.connected` is a boolean, so a doorbell that has never been plugged in
and one that is unplugged look identical. That is exactly the distinction the
person holding a freshly flashed Pi needs, and they are usually not the person
looking at the app.

You already store what answers it. `lastContactAt` is null until the hardware
authenticates for the first time, so:

```js
// Device.toJSON, or wherever the device is serialized for the app
status: !device.deviceId ? 'unprovisioned'
      : device.connected ? 'online'
      : device.lastContactAt ? 'offline'
      : 'never-connected'
```

Four states, and each has a different next action: mint a credential, nothing,
check the doorbell's power and wifi, check that the credential was written onto
the right card.

## A3. Record contact when the socket connects, not only on HTTP [recommended]

`requireDevice` writes `lastContactAt` (throttled to a minute), so every
media-token mint and clip upload updates it. The signaling hello does not — it
writes `connected` and nothing else.

A doorbell can therefore hold a socket for a week, with nothing happening at
its door, and report a `lastContactAt` a week old. Worse for A2: a device's
*first* connection is a socket, so `lastContactAt` stays null and the app goes
on saying "never connected" about a doorbell that is connected right now.

In `signaling/index.js`, `setConnected(device, true)` should set both:

```js
{ $set: { connected: value, ...(value ? { lastContactAt: new Date() } : {}) } }
```

The `$ne` guard on `connected` currently makes that write a no-op when the
value has not changed — which is the case you most want the timestamp from, so
the filter has to loosen for the connect case. It is one write per device
connection, not per frame.

## A4. `hello-ok` is now part of provisioning [do not regress]

`--probe` waits for `{"type":"hello-ok"}` and treats anything else as a
failure. That frame was previously only interesting to a browser.

Keep sending it to `role: "device"`, and keep `deviceId` on it: the probe prints
it back as *"the server knows this device as porch-1"*, which is how the person
provisioning finds out they wrote porch-2's credential onto porch-1's card.

## A5. There is still no enrolment endpoint, and there should not be [do not regress]

`hardwareRoutes.js` says this already. It is worth restating here because
provisioning is now a script, and a script is exactly the thing that invites an
"auto-register on first boot" endpoint.

A device that can enrol itself is a device anyone can enrol. The credential is
minted by a person, written onto the card by a person, and never negotiated.
`provision.sh` prompts for it and refuses to continue without one.

---

# Part 2 — the network goes away and comes back

The device's behaviour here is settled and tested: alerts queue in a bounded
ageing queue, clips wait on disk, the chime never waits for anything, and the
bridge reconnects with jittered backoff forever. What follows is what the
server has to do so that coming back is as complete as going away was clean.

## B1. A viewer who asked while the doorbell was offline is never served [recommended]

Today, `handleWatch` answers `watch-error` when the device is not connected, and
`getViewerToken` returns `deviceOnline: false`. Both are honest. But if the
doorbell reconnects eight seconds later — which is the common case, because the
bridge's backoff starts at one second — nothing re-sends the cue. The viewer is
sitting in front of a LiveKit room the device was never told to join, and the
only way out is pressing Watch again.

Hold the intent briefly and replay it:

```js
// in signaling/index.js, exported so mediaController can record one too
const WATCH_INTENT_TTL_MS = 30_000;
const pendingWatch = new Map(); // deviceKey -> { peer, at }

export function noteWatchIntent(deviceKey, peer) {
  pendingWatch.set(deviceKey, { peer, at: Date.now() });
}

// handleHello, after broadcastPresence(deviceKey, true)
const waiting = pendingWatch.get(deviceKey);
pendingWatch.delete(deviceKey);
if (waiting && Date.now() - waiting.at < WATCH_INTENT_TTL_MS) {
  requestViewer(deviceKey, waiting.peer);
}
```

**Both entry points have to record it**, and the one that matters most is not
the socket. `handleWatch` is the `watch` frame; `getViewerToken` in
`mediaController.js` is what the app actually calls, and it already branches on
`isDeviceOnline` — that `else` is where the intent belongs, alongside the
`deviceOnline: false` it already returns.

Thirty seconds and one entry per device is the whole of it. Two things make it
safe: `viewer-requested` is idempotent on the device (a second one while a call
is running is a no-op), and an intent that expires costs nothing — the call
simply never starts, which is what happens today anyway.

Do **not** queue these indefinitely. A `viewer-requested` delivered ten minutes
late starts a camera, takes the sound card, blocks the chime, and gives up after
`idle_timeout` because nobody came. The device cannot tell a stale cue from a
fresh one; the freshness has to be decided here.

## B2. Release the talk floor when the *device* drops [recommended]

`handleClose` calls `releaseIfHeld` for a browser, and correctly: a closed tab
is how a turn usually ends. A device that drops mid-call releases nothing, so
the floor stays held by whoever had it, through the device's reconnect and
after the call is long over.

Release it when the last device socket for that doorbell goes — the same place
presence is written:

```js
if (role === ROLE_DEVICE && removed && !registry.isDeviceOnline(deviceKey)) {
  setConnected(device, false);
  broadcastPresence(deviceKey, false);
  releaseIfHeld(deviceKey, whoIsTalking(deviceKey));  // nobody is listening
}
```

`talkFloor.js` exports `releaseIfHeld(deviceKey, userId)` and
`whoIsTalking(deviceKey)`, so this composes from what is there — though a
`releaseWhoeverHolds(deviceKey)` beside them would say what it means, and the
two callers would stop having to name a user they do not care about.

The call is over from the device's point of view whether or not the viewers know
yet: `webrtc-video.py` exits when the room empties or when it cannot rejoin, and
`porchlightd` learns that from a pidfd, not from you.

## B3. Terminate on the second missed ping, not the first [recommended]

`HEARTBEAT_INTERVAL_MS` is 30 s and a single missed pong terminates the socket.
The device answers pings from inside the read loop of `websocket-client`, which
is normally instant — but that loop is in the same process as a clip upload
saturating a home uplink.

A false terminate is not free: it costs a reconnect, a presence flap to every
browser, and a fresh `hello`. Counting to two costs at most thirty extra
seconds of believing in a doorbell that has gone:

```js
socket.missedPings = 0;
socket.on('pong', () => { socket.missedPings = 0; });

// in the interval
if (socket.missedPings >= 2) { socket.terminate(); continue; }
socket.missedPings++;
socket.ping();
```

The device is not relying on your heartbeat to notice a dead link — it runs its
own 20 s ping with a 10 s timeout, so it detects a failure faster than you do
either way.

## B4. 4002 stops a doorbell until someone walks up to it [required]

This is the item most worth reading twice.

`CLOSE_UNAUTHORIZED` (4002) means *permanent* to the device. It stops
reconnecting for the lifetime of the process, lights the fault pattern on the
LED, and waits for a human. That is correct for a credential that has been
re-minted — retrying would be a doorbell hammering a server that will never say
yes — and it is why the code is separate from 1013.

The consequence is the part to hold on to: **any refusal you send as 4002
requires a site visit.** Today `handleHello` returns it for every
`result.error`, which is fine because all of them are permanent for a given
build. It stops being fine the first time a refusal is temporary. Specifically:

- A user action such as "pause this doorbell" or "disable this device" must
  **not** close with 4002. That device would stay dark until somebody restarted
  it on site — an unpausable pause. Use 1013, or leave the socket up and stop
  acting on what it sends.
- Rate limiting, maintenance windows, a deploy, a database that is down: all
  1013 or a thrown error, never 4002. The existing `catch` around
  `authenticate()` has this exactly right.
- Deleting a `Device` record does close the door permanently on that hardware,
  by design. Worth a confirmation in the UI that says so.

**And when a device reconnects, displace the old socket — never refuse the
new one.** `registry.add` returns the previous socket and the caller closes
*that* one with 4001. If it were ever inverted — reject the newcomer because
one is already registered — a doorbell whose socket died without a close frame
would be permanently locked out by its own ghost, and would stop trying. The
identity check in `add`, and the `removed` guard in `remove`, are what make a
reconnect look like a reconnect instead of a disconnect. Neither is decoration.

## B5. Silence is still the transient answer [do not regress]

`ingestEvent` throws on anything that might succeed later and returns `REJECTED`
only for what will still be wrong next time; `handleEvent` sends no ack for the
first and `ok: false` for the second. That is the contract, and it is right.

Two things that follow, now that reconnects are in scope:

- **The device's ack timeout is 10 s** and it has exactly one alert in flight at
  a time. A slow ack is not a lost alert, but it does stall the queue behind it,
  so acknowledge on the durable write and let notification fan-out happen after.
  `notify()` is deliberately not awaited — keep it that way.
- **After an outage the device delivers its backlog one alert at a time, up to
  200 of them**, each with the original `at` and each deduped on
  `(eventId, kind)`. That is a burst of upserts against the same index, not of
  new documents. A backlog of forty needed three retry rounds against the live
  server, which is normal and is why the device's queue is measured in a day
  rather than in minutes.

## B6. Media tokens must keep working while the socket is down [do not regress]

`getPublisherToken` and `getListenerToken` sit behind `requireDevice` alone and
check nothing about presence. Leave it that way.

The media script has no socket to you and never did. A call can be running —
and reconnecting to LiveKit, minting a fresh token per attempt — during a window
where the daemon's signaling socket is down and `isDeviceOnline` is false. A
presence check on those endpoints would turn a recoverable wobble into a dead
call.

For the same reason, token mints are the *only* device-authenticated traffic
during a reconnect, so they are also the thing that will notice a clock problem
first: `nbf` is the mint time, and a server clock more than a minute ahead of
LiveKit's makes every token not-yet-valid with nothing saying so.

## B7. Do not treat a minted token as proof the call will start [required]

`requestViewer` returns whether the frame was written to a socket. It cannot
return whether anything read it. Between a doorbell losing power and your
heartbeat noticing, there is up to thirty seconds in which `send()` succeeds
into a socket with nobody on the other end.

So the viewer app must not show "connecting" indefinitely on the strength of
`deviceOnline: true`. Key the live view on **a video track, or on the
`device:<id>:pub` participant appearing in the room**, with a timeout of
fifteen to twenty seconds, after which it says the doorbell did not answer and
offers to try again. Pressing Watch again is cheap and idempotent.

This is also the rule from the brief that catches people: the device joins the
room **twice**, and `device:<id>:sub` publishes nothing, ever. A UI waiting for
every remote participant to publish waits forever, on a call that is working.

## B8. Nothing comes back when the call ends [do not regress]

Still true, and reconnects do not change it. The device decides the call is over
by counting participants whose identity does not begin with `device:`, and then
stops publishing. There is no "call ended" frame on the signaling socket in
either direction, and adding one would give you a second source of truth that
disagrees with LiveKit during exactly the network events this document is about.

---

# How to test it

The device does all of this on purpose, so every one of these is a drill you can
run rather than a race you wait for.

| Drill | How | What should happen |
|---|---|---|
| First connect | `provision.sh --apply` on a clean Pi | three proofs pass, the unit is enabled, the app shows the doorbell online |
| Wrong credential on the card | write porch-2's credential as porch-1 | `provision.sh` refuses it before installing anything |
| Credential re-minted under a running device | `mint-device.mjs --force` | the daemon is closed with 4002, stops retrying, and shows the fault pattern — **and does not come back on its own** |
| A brief outage | unplug the Pi's network for 60 s | alerts queue, the chime still rings, the link returns within a minute, the backlog drains in order with its original `at` values |
| A long outage | unplug for an hour, ring the doorbell several times | the most recent alerts arrive, not the oldest; clips upload after the alerts |
| Watch while offline | press Watch with the Pi unplugged, then plug it in | with B1: the call starts on its own within a few seconds. Without it: nothing, forever |
| Half-open socket | `kill -STOP` the daemon, then press Watch | the token is minted, no call starts; with B7 the UI says so instead of spinning |
| Server restart mid-alert | restart the API while an alert is in flight | no ack, the device retries, exactly one event row afterwards |

The one asymmetry worth remembering while reading those: **an undelivered alert
has a short useful life and an undelivered clip does not.** Alerts live in
memory and are dropped when they age out; clips live on disk, survive a restart,
and are only deleted when you confirm them.

---

# Summary

| | Change | Where |
|---|---|---|
| A1 | `mint-device.mjs --json` | `scripts/mint-device.mjs` |
| A2 | a four-state `status` on the device | `models/Device.js` |
| A3 | write `lastContactAt` on the socket hello | `signaling/index.js` |
| A4 | keep `hello-ok` as it is | `signaling/index.js` |
| A5 | still no enrolment endpoint | `routes/hardwareRoutes.js` |
| B1 | replay a watch intent on reconnect | `signaling/index.js` |
| B2 | release the talk floor when the device drops | `signaling/index.js` |
| B3 | two missed pings before terminate | `signaling/index.js` |
| B4 | 4002 only for what a site visit can fix | `signaling/index.js` |
| B5 | silence stays the transient answer | `services/events/ingestEvent.js` |
| B6 | no presence check on token mints | `controllers/mediaController.js` |
| B7 | the viewer waits on a track, not on a token | the app |
| B8 | no "call ended" frame | everywhere |

Required: **B4**, **B7**. Everything else is a gap the device survives, in the
sense that nothing is lost — only that somebody presses a button twice, or reads
"offline" about a doorbell that has never been plugged in.
