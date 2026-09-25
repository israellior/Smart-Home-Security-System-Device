# App server changes: connecting, and connecting again

Written for whoever maintains the app server. It is a follow-up to
[`server-brief.md`](server-brief.md), not a replacement: everything there still
holds. This one covers the two paths that were never written down — **a new
doorbell's first connection**, and **what happens when the network goes away
and comes back** — and lists only what has to change on your side.

**Updated 2026-09-25, and the ground moved.** The device is being built to
ship to customers: one image for every unit, a per-unit identity written at the
factory, wi-fi given to it from a phone, and the doorbell claimed in the app.
Steps 1 and 2 of that are written and in the device repo. What changed here is
that three items below stopped being nice to have. **A1 now has a file format
the device parses**, and **A2 and A3 are what the customer stares at while they
wait**. There is also a new one, A6, and it is the only place this design has a
hole if it is got wrong.

Most of the rest is small. The signaling layer is already close to right, and
several items below are *"this is correct, and here is why it must not be
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

Since 2026-09-25, and the reason this document changed:

- **`pi/porchlight-firstboot.py`** — reads a birth certificate off the FAT boot
  partition at first boot, installs the credential, patches the config, sets
  the hostname and destroys the file. It is what makes one image become a
  particular doorbell, and it defines the JSON you have to produce. See A1.
- **`pi/porchlight-setup.py`** — if the device is on no network it becomes an
  access point, serves a page, takes an SSID and a password, joins, and
  verifies in stages. It shows the claim code on that page, which is what
  makes A6 load-bearing.
- **The verification's last stage is `GET /api/devices/<id>/self`.** Not a new
  endpoint — it already exists — but it is now the moment a doorbell first
  speaks to you, and it happens **before** the daemon's socket is up. It is
  your "setup complete" signal, and you get it for free. See A3.

---

# Part 1 — from a boxed Pi to a first connection

The path today: mint a credential, write it onto the card, install, start, and
find out whether it worked by watching a log. The changes below make each of
those steps answerable.

## A1. Mint in batches, and emit a birth certificate [required]

`scripts/mint-device.mjs` prints the credential in a block meant for a human
reading a terminal while holding one Raspberry Pi. That is no longer the shape
of the job: fifty identical cards get flashed, and then each one gets a single
file written onto its FAT boot partition.

**The device parses that file, so its field names are a contract.** This is
what `porchlight-firstboot.py` reads from `/boot/firmware/porchlight.json`:

```json
{
  "deviceId":      "porch-1",
  "credential":    "pl_porch-1_...",
  "url":           "https://porchlight.example",
  "name":          "Front Door",
  "claimCode":     "7K2M9P",
  "setupSsid":     "Porchlight-7K2M",
  "setupPassword": "eight characters or more"
}
```

- `deviceId`, `credential` and `url` are **required**. Without them the device
  refuses the card and lights the fault pattern rather than half-provisioning
  itself.
- **The device checks the credential is its own** before installing anything:
  it must begin `pl_<deviceId>_`. Minting a batch is exactly where two rows get
  crossed, and the alternative is discovering it as close code 4002 at a
  customer's house.
- `name` is what the setup page calls the doorbell. Cosmetic, and nice.
- `claimCode`, `setupSsid` and `setupPassword` are new. See A6.

So: **`--json`, and `--batch N`.** `--json` writes one object and nothing else.
`--batch` writes N of them, one file per device, plus whatever your label
printer wants: device id, claim code, setup SSID, setup password, and a QR of
the claim code. That is the sticker on the back of the case.

The production line becomes: flash N cards from one image, copy one JSON onto
each boot partition, stick on the matching label. Two seconds a unit, no
per-unit image, no re-flash.

## A2. Say "never connected" differently from "offline" [required]

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
check the doorbell's power and wi-fi, check that the credential was written
onto the right card.

**Required rather than recommended now.** The customer enters a claim code and
then watches a screen until something changes. "Never connected" is the state
that screen is in for the whole of setup, and `connected: false` cannot tell
*it has not been plugged in yet* from *it was, and something went wrong*.

## A3. Record contact when the socket connects, not only on HTTP [required]

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

**And the good news, which saves you an endpoint.** The setup service's last
check before it declares success is `GET /api/devices/<id>/self` with the
device credential, and `requireDevice` already writes `lastContactAt` on that
call. So **a doorbell that has just been given a wi-fi password touches you
over HTTPS seconds before its socket comes up**, and that write is the "setup
complete" signal. The app can flip from "waiting for your doorbell" on
`lastContactAt` alone, and the device needs to tell you nothing new.

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

## A6. The claim code is not the share code [required]

The one item here with a security consequence, and the easiest to get wrong by
doing the obvious thing.

The device has to show a code somebody types into the app to make the doorbell
theirs. You already have a code that does that: `shareCode`, and
`POST /api/devices/join`. Reusing it is one line of work and it is wrong.

**A share code is permanent and a doorbell is bolted to the outside of a
house.** It goes on the sticker, it is on the setup page, and it is printed on
the box. Anyone who photographs the back of the case — a delivery driver, a
guest, someone who takes a picture of the packaging in a recycling bin — can
join that doorbell to their own account and watch the door, forever, with no
interaction with the owner at all.

So, two codes with two lifetimes:

| | who it is for | lifetime |
|---|---|---|
| **claim code** | the first owner, once | **consumed by the first successful claim** |
| **share code** | a partner, a flatmate, a neighbour | generated in the app, revocable, rotatable |

What that means concretely:

- **Mint generates `claimCode` and stores it hashed**, exactly as the device
  credential is stored. It is a secret that grants ownership; it should not be
  readable out of a database dump any more than the credential is.
- **A claim consumes it.** `POST /api/devices/claim { claimCode }` with a
  signed-in user: on success create the owner Membership, mark the code used,
  and refuse it from then on. A second person with the same photograph gets
  "that code has already been used".
- **`shareCode` stops being a factory artifact.** Generate it on demand from
  inside the app, let the owner revoke and regenerate it, and do not print it
  on anything. The existing `/join` flow keeps working for it unchanged.
- **Rate-limit claims per account and per code.** The code is short enough to
  be typed, which means it is short enough to be guessed at scale.

There is a real trade here and it is worth stating rather than discovering: a
claim code that is consumed means **a doorbell that changes hands needs the
seller to release it**, or support to re-mint the code. A permanent code has no
such friction, which is exactly why it is unsafe. Releasing a device is an app
feature; a stranger watching a door is not a feature.

### While we are here: the setup access point

`setupSsid` and `setupPassword` come from the same mint and go on the same
sticker. Two notes:

- **The access point must have a password.** An open one with no internet makes
  iOS give up on it and bounce the phone back to cellular within seconds, and
  the customer never sees the page. WPA2 needs eight characters or more.
- **It is worth being per-device**, for the same reason as everything else
  here: one shared setup password across a product line is one disclosure away
  from anyone being able to talk to any unconfigured doorbell in range. It is a
  short window — the AP only exists before setup — but it is a window in which
  the device will accept a network to join.

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
| A1 | `--json` and `--batch`, and the birth-certificate shape | `scripts/mint-device.mjs` |
| A2 | a four-state `status` on the device | `models/Device.js` |
| A3 | write `lastContactAt` on the socket hello | `signaling/index.js` |
| A4 | keep `hello-ok` as it is | `signaling/index.js` |
| A5 | still no enrolment endpoint | `routes/hardwareRoutes.js` |
| A6 | a one-time claim code, not the share code | mint, `models/Device.js`, a new route |
| B1 | replay a watch intent on reconnect | `signaling/index.js` |
| B2 | release the talk floor when the device drops | `signaling/index.js` |
| B3 | two missed pings before terminate | `signaling/index.js` |
| B4 | 4002 only for what a site visit can fix | `signaling/index.js` |
| B5 | silence stays the transient answer | `services/events/ingestEvent.js` |
| B6 | no presence check on token mints | `controllers/mediaController.js` |
| B7 | the viewer waits on a track, not on a token | the app |
| B8 | no "call ended" frame | everywhere |

Required: **A1**, **A2**, **A3**, **A6**, **B4**, **B7**.

A1 because the device parses the file you write. A2 and A3 because they are the
whole of what the customer sees during setup. A6 because getting it wrong hands
a stranger a camera pointed at a front door. B4 because a wrong close code turns
a software state into a site visit, and B7 because a token is not a call.

The rest are gaps the device survives, in the sense that nothing is lost — only
that somebody presses a button twice, or reads "offline" about a doorbell that
has never been plugged in.
