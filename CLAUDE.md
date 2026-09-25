# CLAUDE.md

Working notes for this project. Read this first; update it at the end of any session
that changes something here. Last updated 2026-09-25.

**Also read [DESIGN.md](DESIGN.md)** and
[docs/server-brief.md](docs/server-brief.md). This file describes the present.
DESIGN.md describes how the finished system is meant to work — remote access,
auth tokens, an SFU, 1–5 viewers per Pi — and those decisions are settled. The
brief is the contract with the app server, and **where this file and that one
disagree, that one wins**.

The five-step plan below is finished. What replaced it is a doorbell whose call
publishes to LiveKit Cloud, and the single most important thing to know is that
**none of the LiveKit work has run on the Pi yet** — see "Read this first".

## What this is

A learning project, not production software. It began as a crude PCM-over-HTTP
intercom, became a hand-negotiated WebRTC call between one browser and the Pi,
and is now a doorbell: `porchlightd` decides what the device does, and the live
call publishes to **LiveKit Cloud** so that it works from anywhere rather than
across one LAN.

The Pi and the viewers both connect *outward* to LiveKit, so no home router has
to accept an incoming connection and no TURN relay is needed. The app server
mints tokens and is never in the media path.

Phase 2 — opening the boxes and rewriting RTP, the jitter buffer, V4L2 capture
and signaling by hand — is still the point of the exercise. Moving to LiveKit
moved it further away rather than cancelling it: there is no `webrtcbin` in the
tree any more, and what replaced it hides more, not less. What that code taught
is under "What the webrtcbin version taught" below, and the code itself is in
the git history.

**The original PCM intercom was deleted on 2026-09-19.** It streamed raw 16-bit
LE PCM over plain HTTP — `arecord | curl` → `POST /stream` → WebSocket → Web
Audio, with `GET /talk` for the way back, and a hand-written jitter buffer and
resampler in the browser. WebRTC replaced every part of it, so it went, along
with `public/index.html`, `public/mic-worklet.js`, `pi/intercom.sh`,
`tools/tone.js`, `tools/talk-monitor.js`, `tools/talk-test.js`, the old
`README.md`, and about half of `server.js`.

Phase 2 was going to use it as a reference for hand-writing those layers again.
If that turns out to matter, it is in this conversation's history and nowhere
else — there is still no git repository (open problem 2).

## Where we are

The five-step plan below is finished, and then the ground moved: the call does
not negotiate with a browser any more. It publishes to **LiveKit Cloud**, and
the viewer is the app rather than a page served from here.

| Step | What | Status |
|---|---|---|
| 0 | Read the existing project | **done** |
| — | Install GStreamer on the Pi, verify hardware | **done** |
| 1 | Video only, `videotestsrc`, Pi → browser | **done**, and since replaced |
| 2 | Add the Pi's microphone (Opus) | **done**, and since replaced |
| 3 | Add browser mic → Pi speaker | **done** — it was a call |
| 5 | Swap `videotestsrc` for `libcamerasrc` | **done 2026-09-22 — a real camera** |
| 4 | Echo cancellation (`webrtcdsp`) | **written 2026-09-22 — never run on the Pi** |
| — | The call publishes to LiveKit | **written 2026-09-22 — never run on the Pi** |
| — | `porchlightd` starts the call for real | **written 2026-09-22 — never run on the Pi** |
| — | The button, the PIR and the LED on real GPIO | **written 2026-09-23 — never run on the Pi** |
| — | Provisioning, and a link that comes and goes | **written 2026-09-25 — never run on the Pi** |
| — | A device that ships: identity at the factory, wi-fi from a phone | **written 2026-09-25 — never run on the Pi** |

### Read this first: nothing after Step 5 has run on hardware

Steps 1–3 and 5 were confirmed on the real Pi. Everything after them — the
LiveKit rewrite of `webrtc-video.py`, echo cancellation, `porchlightd`'s
`script` media backend, its `gpio` input and LED backends, and now
`pi/provision.sh` and everything about reconnecting — was written on the
Windows host and **has never run on the Pi**. Do not read the confident tone of
the sections below as evidence.

What *was* verified, and how:

| Verified | How |
|---|---|
| Both token endpoints, and exactly what their claims grant | POSTed to the live app server on `localhost:4000` |
| A rejected credential is permanent; an unreachable server is not | same, both paths exercised |
| The LiveKit SDK's real API surface | installed `livekit` 1.1.19 on the dev host and read it |
| That the SDK takes the *address* of a video buffer rather than copying it | read `_utils.get_address` — which is why frames are copied with `bytes()` |
| `build_media_command`, the config reader, the I420 size rule | `ctest`, 91 passing |
| The fault LED, and that a later plain outage cannot cancel it | `ctest` — it is core logic, so it needs no link at all |
| The LED blink table | `ctest` — it is deliberately free of libgpiod so it can be |
| That the GPIO code matches libgpiod's real v2 API | compiled against upstream 2.2's own `gpiod.h`, `-Wall -Wextra -Wpedantic -Wshadow`, clean |
| That a rising edge means "pressed" on an active-low line | read it out of `linux/gpio.h`: v2 edges are logical, "inactive to active" |
| **The GStreamer pipeline** | **not at all.** There is no GStreamer on this host or in WSL |
| **Any GPIO line actually moving** | **not at all.** No GPIO on this host, and libgpiod 2.x is not even packaged for Ubuntu 24.04 |
| **`pi/provision.sh`** | **only the survey.** `bash -n`, and a dry run in WSL that printed the 19 changes it would make. Nothing has been applied anywhere |
| The bridge's restart backoff, and the fault LED end to end | **drilled in WSL.** Started the daemon with no credential: fault at once, restarts at 1, 2, 4, 8 s, no fork loop. Wrote the credential while it ran and it came up on the next tick with no restart |
| That arming a timerfd for zero seconds disarms it | the same drill printed `starting the bridge again in 0s`, which is how the truncation was found. Both ends of that subtraction now use one `now` |
| **`--probe`** | **not at all.** It needs a server and a socket, and the one thing this host cannot do is reach its own port from WSL |

So on the Pi, in this order — each one isolates a different half:

```bash
./check-livekit.sh http://192.168.0.219:4000 porch-1   # the parts, before the whole
./webrtc-video.py --url … --device-id … --credential-file … --dry-run
./webrtc-video.py … --check
./webrtc-video.py … --video test
./webrtc-video.py …
```

`--dry-run` builds and runs the pipeline with **no LiveKit and no token**, and
`--check` fetches both tokens with **no camera**. They exist because the two
halves fail in completely different ways and this host can only test one.

### How the call works now

```
libcamerasrc → clock overlay → I420 → appsink ─┐
                                                ├→ publisher token → LiveKit → viewers
alsasrc → 48 kHz mono → webrtcdsp → appsink ───┘   (canPublish, canSubscribe false)

alsasink ← level ← webrtcechoprobe ← appsrc ←───── listener token ← LiveKit ← a viewer
                                                   (canSubscribe, canPublish false)
```

**Two tokens, two connections, one process.** The app server mints a publisher
token (`canPublish`, `canSubscribe: false`) and a listener token (the reverse),
both two minutes long and both scoped to `device-porch-1`. `canSubscribe: false`
is a claim *inside* the token that LiveKit rejects on, not a policy the app
server enforces — so a leaked publisher token cannot be turned into a way to
watch the house. They are not interchangeable and must not be merged.

**Tokens are fetched fresh on every connection attempt and never cached.** A
token is needed to join and for nothing after that, so expiry needs no handling
beyond not keeping one: a reconnect mints another.

**The Pi appears in the room as two participants**, `device:porch-1:pub` and
`device:porch-1:sub`. The `:sub` half publishes nothing, ever. Anything counting
who is in the room has to skip identities starting `device:` — `is_viewer()`
does, and the viewer app must too, or the Pi looks like its own audience and the
call never ends. This is written up for the app in `docs/server-brief.md`.

**The call ends itself.** The listener connection is the only thing that can see
who is in the room, so it decides: `--linger` seconds (3) after the last viewer
leaves, or `--idle-timeout` (30) if nobody ever arrived, the process exits, and
`porchlightd` learns that from a pidfd. There is no "call ended" message on any
socket.

**There is no signaling socket of ours in the media path at all.** The daemon
holds the device's one WebSocket, as `role: "device"`. `viewer-requested` is
still the cue, still delivered there, and still not an offer; `peer` is kept
only so the daemon knows which viewer a later `CallEnded` belongs to, and is
never passed to the media process.

### Route A, and why not WHIP

The choice was participant tokens with the LiveKit SDK (Route A) against WHIP
ingress with `whipsink` (Route B). **Route A**, and the deciding facts were not
the ones either side of the argument started with:

- **`whipsink` is in `gst-plugins-rs`, and Debian 13 does not package it.** So
  is `livekitwebrtcsink`. Both GStreamer routes need a Rust toolchain and an
  hour of `cargo` on a Pi 4; WHIP is not the cheap option, it is the same cost
  plus a server change.
- **WHIP is publish-only.** The downlink is *subscribing*, which needs a
  participant connection anyway — so Route B is Route A plus a second transport
  and a second kind of credential, not instead of it.
- **The encoder chain Route B would have protected is software x264 anyway.**
  `v4l2h264enc` stalls when fed by `libcamerasrc` (see Step 5), so the hardware
  encoder was already unusable with this camera. What the SDK encodes with
  internally is the same kind of software encode it replaces.

What Route A costs is real and worth stating: **frames go through Python**.
640×360 I420 is 345 KB a frame, copied once into Python and once into the FFI,
and the tuned `x264enc tune=zerolatency` chain is gone. What it keeps is
`webrtcdsp` — capture and playback stay in one GStreamer pipeline in one
process, which is exactly what the echo canceller needs.

**If the pure-GStreamer pipeline is ever wanted back**, `livekitwebrtcsink` is
the upgrade path and it takes `signaller::auth-token` — *the same participant
token the server already mints*, and it requires `canSubscribe: false`, which
is already what the publisher token says. The server would not change. Only the
Pi would, and only after `gst-plugins-rs` is built there.

### What the webrtcbin version taught

Steps 1–3 hand-negotiated with a browser over `server.js`. That code is gone
from the tree and lives in the git history; these are the parts worth not
re-learning, because Phase 2 means writing this layer by hand again.

- **Port 0 in an *offer* is not a rejection.** Under `max-bundle` every m-line
  after the first is offered with port 0 and `a=bundle-only`. Port 0 means
  refusal only in an *answer*.
- **Chrome will receive mono Opus but will not send it.** Caps with no
  `encoding-params` emit `a=rtpmap:98 OPUS/48000`, which SDP reads as one
  channel; Chrome's encoder only offers `opus/48000/2`, so it answers
  `m=audio 0 … 0` — its way of saying *unusable*, not *unwanted*. One refused
  m-line stalls the whole BUNDLE group.
- **Every payload type on a bundled transport must be distinct across m-lines**,
  which is why the three tracks were 96, 97 and 98.
- **`addTrack` picks the wrong transceiver** when one is already sending the
  other way: it must be matched by `mid` and attached with `replaceTrack`
  before `createAnswer`.
- **`pad-added` proves nothing about traffic.** webrtcbin takes the SSRC from
  the SDP, so the pad exists before a single RTP packet does. Everything is
  counted at the pad for that reason, and the habit survives into the LiveKit
  version: `video:`, `mic:` and `talk:` are counters, not inferences.
- **The silence that cost an evening was not WebRTC at all** — it was playing
  into `hw:CARD=Headphones` with nothing plugged into it. The HAT has its own
  jack and is a different card.

### The clock: still the most expensive lesson here, and still live code

**`alsasrc` provides a clock and `libcamerasrc` does not.** With both in one
pipeline GStreamer makes the sound card's clock the pipeline clock, while
`libcamerasrc` goes on timestamping from the system monotonic clock regardless.
Every video buffer then reaches the sink with a running time computed by
subtracting a base time in one clock's units from a timestamp in another's, and
**the video branch stalls within a second** — the camera keeps delivering 30 fps
and nothing comes out the other end — while audio, stamped against its own
clock, runs perfectly and hides the cause.

The fix is one line, `pipeline.use_clock(Gst.SystemClock.obtain())`, and it is
still in `webrtc-video.py` for exactly the same reason. `porchlightd`'s recorder
cannot call it from a `gst-launch` command line, so it makes the card decline
the job instead with `provide-clock=false` — asserted in `pipeline_test.cpp`.

`videotestsrc` paces itself off whichever clock the pipeline chose, so Steps 1–3
were immune by accident and this only appeared at Step 5. Every cheap test
misses it:

| Test | Result | Why it proves nothing |
|---|---|---|
| `libcamerasrc ! … ! x264enc ! fakesink sync=false` | 30 fps | no `alsasrc`, so no clock conflict |
| the same with `sync=true` | 30 fps | still no `alsasrc` |
| `--video test` into the real pipeline | works | `videotestsrc` follows the pipeline clock |
| `--audio none` with the camera | works | removes the other clock |
| camera + `--audio alsa` | **stalls** | the only combination that has both |

**So the bisect that finds it is `--audio none`, not any amount of
`gst-launch`.**

### The camera

Four decisions, all still in force:

**`libcamerasrc`, not `v4l2src`.** `/dev/video0` is the sensor and what comes
out of it is raw Bayer — the Pi's ISP is a *separate* device that turns that
into a picture. libcamera drives both halves as one unit. (Phase 2 drives them
by hand; that is the whole point of `/dev/video0` then `/dev/video11`.)

**The imx708 is 16:9 (4608×2592), so the camera default is 640×360, not
640×480.** Asking a 16:9 sensor for 4:3 makes the ISP crop the sides off, which
on the **wide** lens throws away exactly the field of view the wide lens was
bought for. The same default is now in `porchlightd`'s `media` config, and both
warn when they are given something that is not 16:9.

**The full sensor mode is forced.** Left alone, libcamera picks the binned
1536×864 mode for any small request, and that mode reads only the centre
3072×1728 of the array — about 120° of diagonal field of view reduced to about
98. `sensor-config` asks for 2304×1296, which reads the whole array and still
runs at 56 fps.

**Autofocus is set in Python, not in the pipeline string.** libcamera exposes
each sensor control as a GObject property and which ones exist depends on the
sensor and the libcamera version, so a missing one in `parse_launch` would take
the whole pipeline down. `set_focus` and `set_sensor_mode` read
`list_properties()` first and print a note instead. `tune_dsp` now does the same
for `webrtcdsp`, for the same reason. **`af-mode` defaults to *manual* at
lens-position 0, which is infinity**, so leaving focus alone is not a neutral
choice — a doorbell does not move, and `--focus 1.5` is probably what this wants
in the end, since continuous AF visibly hunts on a static scene.

**A new constraint from LiveKit: the picture size must have a width that is a
multiple of 8 and an even height.** GStreamer pads I420 rows up to a multiple of
four and the SDK reads the planes tightly packed. They agree at 640×360, 640×480
and 1280×720 and can disagree elsewhere, and the symptom would be a picture
sheared diagonally rather than an error — so `parse_size` refuses it, the
config reader refuses it, and the frame is dropped with a message if it ever
gets past both.

**`v4l2h264enc` still does not work with `libcamerasrc`** — a plain
`libcamerasrc ! videoconvert ! v4l2h264enc ! fakesink` stalls outright where
`x264enc` runs at 30 fps. It no longer blocks the call, which does not encode in
GStreamer at all, but it still constrains the recorder. Not yet investigated.

### Echo cancellation

`webrtcdsp` on the capture branch and `webrtcechoprobe` on the playback branch,
both in **one pipeline in one process** — they find each other by element name,
and putting them in one pipeline is stronger than it needs to be and removes the
question entirely. `--aec off` turns both off, and is automatic when there is no
playback to cancel against.

Only `echo-cancel` is set in the launch string. `delay-agnostic`,
`high-pass-filter`, `noise-suppression` and `gain-control` go through
`tune_dsp`, which checks `list_properties()` first — the set differs between
versions and a missing property in `parse_launch` is fatal.

Until this is confirmed working, the way to keep the mics from hearing the
speaker is still to run headphones-only on the HAT — the WM8960 drives the two
amps separately:

```bash
amixer -c 2 sset "Speaker" 0%     # restore with 82%
```

### Getting a device online, and keeping it there

**Written 2026-09-25, and none of it has run on a Pi.**

Provisioning is one script now. `pi/provision.sh` surveys by default and
changes nothing; `--apply` carries it out; `--verify` runs only the proofs at
the end, which is what to run when a doorbell that used to work has stopped.

```bash
./pi/provision.sh --url https://porchlight.example --device-id porch-1
./pi/provision.sh --url https://porchlight.example --device-id porch-1 --apply
```

**The three proofs are the point of it**, and they are in the order they fail
because in any other order they are indistinguishable:

| | What it proves | What it cannot see |
|---|---|---|
| `server-bridge.py --check` | the credential, the URL, the route | the socket |
| `server-bridge.py --probe` | the WebSocket handshake, to `hello-ok` | the camera, the tokens |
| `webrtc-video.py --check` | both LiveKit tokens | the camera, the card |

A network can carry the first and drop the second. A proxy that allows HTTPS
and refuses a WebSocket upgrade is otherwise diagnosed as "the daemon just sits
there", which is where the evening goes.

**The credential is never an argument.** `provision.sh` prompts for it, or
reads a file, and checks that it names *this* device — it is
`pl_<deviceId>_<secret>`, so porch-2's credential on porch-1's card is caught
before anything is installed rather than showing up as close code 4002 an hour
later. It also fixes the ownership trap: a credential at 0600 owned by whoever
typed it is unreadable to the `porchlight` user the unit runs as, and that
fails as an authentication error.

**Four things now separate "the link is down" from "the link is refusing us".**

- `ServerOffline` carries a `permanent` flag, and the LED has a seventh
  pattern, `Fault`, for it: **Offline's exact negative**, lit with two short
  gaps where Offline is dark with two flashes. Offline passes when the network
  comes back; Fault does not, and somebody has to walk up to the doorbell.
  Close 4001, close 4002, and a missing credential all raise it.
- **The daemon checks it can read the credential before it starts the bridge.**
  A bridge that dies on a missing file dies instantly, and restarting that
  immediately was a fork loop.
- **The bridge is restarted with a backoff** (`server.restart_backoff_*`, two
  seconds to sixty, reset by a connection that reached `hello-ok`). The bridge
  reconnects on its own, so it *exiting* never means the network went away.
- **The bridge gives up on an unanswered hello after ten seconds.** An open TCP
  connection that nothing is reading looks exactly like a working one.

Everything the server has to do to match is in
[docs/app-server-changes.md](docs/app-server-changes.md) — written against the
real backend, so it names files. The two that are not optional: **4002 means a
site visit**, so nothing transient may ever use it; and **a minted viewer token
is not proof a call started**, because a frame can be written into a half-open
socket for up to thirty seconds.

### A device that ships, and the two programs that make it one

**Written 2026-09-25. None of it has run on a Pi, and the half that needs a
radio cannot be run anywhere else.**

The device is being built to go to customers, which invalidated the way it was
provisioned and nothing else. The credential model survives intact — per
device, hashed on the server, shown once — and what changed is that no human
types it.

**One image, and a birth certificate.** Every unit is flashed from the same
image. What makes a unit itself is one file on the FAT boot partition, written
by the production line after flashing, because FAT is the only partition a
laptop can write without help:

```json
/boot/firmware/porchlight.json
{ "deviceId": "porch-1", "credential": "pl_porch-1_...",
  "url": "https://...", "name": "Front Door", "claimCode": "7K2M9P",
  "setupSsid": "Porchlight-7K2M", "setupPassword": "..." }
```

`porchlight-firstboot.py` moves the credential to `/etc/porchlight/credential`
at 0600 owned by `porchlight`, writes the non-secret half to `identity.json`
for the setup page, patches `device_id` and `server.base_url` into
`porchlightd.json`, sets the hostname, and then **overwrites and removes the
file from the boot partition** — which is best effort on a wear-levelled card,
and the honest reason it is safe is that the credential is revocable by
re-minting, not that the bytes are gone.

It **checks the credential names this device** (`pl_<deviceId>_`) before
installing anything. Minting fifty cards is exactly where two rows get crossed,
and the alternative is finding out as close code 4002 at somebody's house.

**Wi-fi comes from a phone.** `porchlight-setup.py` runs before the daemon at
every boot. Connected already? It exits and is never noticed. Not connected? It
becomes a WPA2 access point, serves a page, takes an SSID and a password,
joins, and verifies. Three things about it are rules rather than details:

- **Scan before the access point.** One radio cannot be an access point and
  look for networks at the same time, so the SSID list is captured while the
  device is still a client and cached for the page. Get this backwards and the
  page has nothing to offer.
- **The phone loses the device the moment it tries.** Joining means dropping the
  access point the phone is on, so the POST answers *before* anything happens
  and the outcome is remembered for when the customer comes back. A reply
  written after the radio moves reaches nobody.
- **Wait before deciding.** At boot NetworkManager takes seconds to associate.
  A device that gave up after two would raise an access point in a house whose
  wi-fi works, and never join it again — nobody has any reason to come and tell
  it anything. So: saved profiles means wait (45 s); no saved profiles means
  there is nothing to wait for.

**Verification is staged on purpose**: link, then DNS, then
`GET /api/devices/<id>/self`. A wrong password, a network with no internet and a
network that cannot reach Porchlight are three different things for a customer
to do about, and "could not connect" is none of them. A TLS failure with an
implausible clock is reported as the clock, because a Pi has no battery and
that failure otherwise reads as a broken network.

**The LED is the only output while the phone is elsewhere.** The setup service
holds the GPIO line and porchlightd starts only when it exits `Before=`, no
IPC. Two patterns live in the Python and nowhere else, cross-referenced from
`led_patterns.h` so the whole vocabulary is readable in one place.

**What can be tested here, and what cannot.** `pi/tests/firstboot-drill.sh` is
27 checks over every path of firstboot, and passes — it only writes files.
`pi/tests/setup-drill.sh` drives the whole setup flow against `--fake-nm`, a
pretend radio, and passes: the page, the captive-portal probes, a refused
password coming back as a sentence, and a good one ending the process with exit
0. **`nmcli`, the access point, the scan ordering, the dnsmasq redirect and the
LED are not tested by anything and cannot be** on a machine with no radio.

Everything the app server has to change for this is in
[docs/app-server-changes.md](docs/app-server-changes.md), and the one with a
security consequence is **A6: the claim code is not the share code.** A share
code is permanent and a doorbell is bolted to the outside of a house; anyone
who photographs the sticker could otherwise join it to their own account
forever.

### A healthy run

- `camera: sensor mode 2304x1296` and `camera: continuous autofocus`
- `audio: echo cancellation on (echo-cancel, delay-agnostic, …)`
- libcamera's own `Selected sensor format: 2304x1296-SBGGR10_1X10/RAW` and
  `configuring streams: (0) 640x360-YUV420`
- `publisher: joined device-porch-1 as device:porch-1:pub`, then
  `publisher: camera published` and `publisher: microphone published`
- `listener: joined device-porch-1 as device:porch-1:sub`
- `listener: 1 viewer(s): user:…`
- `video:` lines reading `30 fps from the source, 30 published/s`, and
  **staying** that way — this is the line that catches the clock fault
- `mic: 100 frames/s`
- `talk: nothing arriving` until someone holds the button, then a real dBFS
- `room: 1 viewer(s): user:…`

`video: 30 fps from the source, but none reaching LiveKit` is the line that
turns "the picture froze" into a located fault, and it is the direct descendant
of the pad counters from Step 5.

## The machines

**The Pi** — `192.168.0.129`, user `israellior1`. SSH is open but **password only**;
no key is installed, so Claude cannot run commands there. To change that:
`ssh-keygen -t ed25519` then append the public key to `~/.ssh/authorized_keys` on the Pi.

- Raspberry Pi 4 Model B Rev 1.5, kernel 6.18.50+rpt-rpi-v8 (aarch64), Debian 13 trixie
- GStreamer 1.26.2. Verified present: `opusenc`/`opusdec`, `alsasrc`/`alsasink`,
  `audioconvert`, `audioresample`, `webrtcdsp`, `videotestsrc`, `textoverlay`,
  `v4l2h264enc`, `x264enc` — and `webrtcbin`, `rtpopuspay`/`rtpopusdepay`,
  `rtph264pay`, which nothing uses any more
- `appsink`/`appsrc` and `webrtcechoprobe` are what the call needs now, and
  **neither has been confirmed on this Pi** — `check-livekit.sh` is what checks
- Python bindings `Gst` imports; `GstWebRTC` and `GstSdp` did too, and are no
  longer used
- **`v4l2h264enc` exists — this is a Pi 4, which has a hardware H.264 encoder.**
  `/dev/video11` is the encoder, `/dev/video10` the decoder. (Do not repeat the
  earlier mistake of assuming a Pi 5; a Pi 5 has no H.264 encode block, a Pi 4 does.)
  It **stalls when fed by `libcamerasrc`**, though, so `recorder.encoder: "v4l2"`
  is not usable with the camera — see "The camera". `x264enc` carries 640×360/30
  easily. This constrains the **recorder** only: the call does not encode in
  GStreamer at all, so the hardware encoder is no longer on its critical path.
- `libcamerasrc` (`gstreamer1.0-libcamera` 0.7.2) installed 2026-09-22, with
  `rpicam-apps` 1.13.0 already present.
- Audio out available besides the HAT: `card 3` = the Pi's own headphone jack,
  plus two HDMI outputs. **`card 3` produced no sound when tested 2026-09-18**:
  `speaker-test -D hw:CARD=Headphones` opened it, negotiated 48 kHz stereo, ran
  its full period and reported `PCM … 78% … [on]` — and nothing was audible. So
  ALSA is fine and the fault is past it; most likely nothing is plugged into the
  Pi's own jack, the HAT having its own. Do not use `card 3` as a known-good
  output to test against until that is settled.

**The server host** — Windows 11, `192.168.0.219`, Node 24.11.1, npm 11.6.2.
Dependencies are only `express` and `ws`.

**The Wi-Fi network is classified `Private` as of 2026-09-22, and this broke the
Pi's `curl`.** The two `Node.js JavaScript Runtime` inbound rules are scoped to
the **`Public`** profile only, so when the classification changed nothing on the
LAN could reach port 3000 any more. The fix, from an **administrator**
PowerShell, is a rule for the port rather than for `node.exe` — the app rules
would grant every future Node process inbound access on the home network:

```powershell
New-NetFirewallRule -DisplayName "Pi intercom signaling (3000)" -Direction Inbound `
  -Action Allow -Protocol TCP -LocalPort 3000 -Profile Private -RemoteAddress LocalSubnet
```

To check which way round it is now:

```powershell
Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory
Get-NetFirewallApplicationFilter | Where-Object Program -like '*node*' |
  ForEach-Object { $_ | Get-NetFirewallRule | Select-Object DisplayName, Profile, Action }
```

**The camera** — Raspberry Pi Camera Module 3, **wide** (120° FOV, f/2.2). Sensor
IMX708, 4608×2592, which is **16:9, not 4:3**. It has phase-detect autofocus, which
the fixed-lens Camera Module 2 did not — so `--focus` is a real control here and
would have been meaningless before. Fitted and working 2026-09-22.

It goes in the socket silkscreened **CAMERA**, not the identical-looking one
silkscreened DISPLAY, with the Pi powered off and unplugged. These are bottom-
contact ZIF sockets: the ribbon's silver contacts face **down** into the socket
and the blue stiffener faces up, and the latch has to be pressed back down evenly
on both sides. A ribbon that looks seated but whose latch never clamped is the
most common failure, and it is invisible from software. Reversing a CSI ribbon
does no damage, so when in doubt it costs nothing to try it the other way.

A Pi 4 takes the 15-pin cable the module ships with; the 22-pin adapter cable is
a Pi 5 thing and is not needed here.

The WM8960 HAT covers the GPIO header, not the CSI connector, but it makes the
ribbon awkward to route — seat the ribbon before the HAT goes back on.

If the sensor is not detected, the ribbon is the cause far more often than
anything else, and `check-camera.sh` distinguishes "kernel found no sensor" from
"libcamera cannot open it" from "GStreamer cannot".

**Telling a dead cable from a software fault, in one command.** `camera_auto_detect`
runs in the VideoCore firmware before Linux exists; when it finds nothing there is
no overlay, so no sensor in the device tree, no camera I²C bus, no `/dev/video0`
and `No cameras available!` — five symptoms, one cause, and none of them say
*why*. Forcing the overlay by hand skips the firmware's decision and makes the
kernel address the chip directly:

```bash
sudo dtoverlay imx708 && sleep 2 && dmesg | tail -30
sudo i2cdetect -y 10          # the muxed bus the overlay creates; NOT i2c-0
```

It is not persistent, so a reboot undoes it. Read the result like this:

- `imx708 10-001a: failed to read chip id 708, with error -5` — `-EIO`, no ACK on
  the bus. The overlay, the drivers and the regulator are all fine and the module
  is not electrically reachable. **Hardware: seating, cable, or the module.**
  `dw9807 10-000c` failing the same way is the autofocus motor, and confirms the
  overlay matched a Camera Module 3 rather than something else.
- `/dev/video0` appears — the camera is fine and only auto-detect failed; pin it
  with a fixed `dtoverlay=imx708` in `config.txt`.

Three false leads worth not repeating, all of which cost time on 2026-09-22:

- **`dtoverlay -l` lists only overlays applied at *runtime*.** The five in
  `config.txt` never appear there, so an empty list proves nothing either way.
- **`vcgencmd get_camera` is useless here.** All three of its fields —
  `supported=`, `detected=` and `libcamera interfaces=` — read `0` on this OS with
  a fully working camera that `rpicam-hello` was listing at the same moment. It
  briefly looked like the decisive test and is not a test at all.
- **`dtoverlay imx708` failing with "Failed to apply overlay" is good news.** It
  means the firmware already applied it at boot, i.e. the camera *was* detected.
  A reboot is what cleared the hand-applied one, so this error appearing after a
  reseat is the first sign it worked.

**What is actually decisive:** `/dev/video0` existing, a sensor node under
`/proc/device-tree`, and `rpicam-hello --list-cameras` naming the camera.

**Resolved 2026-09-22.** The cause was the ribbon. With it reseated, the firmware
detects the camera at boot on its own and `rpicam-hello` reports
`imx708_wide [4608x2592 10-bit RGGB]` — the *wide* tuning file, which is the
correct one for this module. No `config.txt` change was needed; `camera_auto_detect=1`
does the whole job.

`camera_auto_detect=1` in `/boot/firmware/config.txt` is what loads the sensor
overlay; it is the default, and `check-camera.sh` checks it.

**The audio HAT** — Waveshare WM8960 Hi-Fi Sound Card HAT. An I²S codec on the GPIO
header, not USB. Two microphones on the board, and speaker + headphone out, so both
directions run through one card — which is why Step 4's echo cancellation is
mandatory rather than optional. ALSA card 2; always address it as
`hw:CARD=wm8960soundcard`, never `hw:2,0`, because card numbers move between boots.

Mixer routing is **correct** and is not the problem. Verified on: `Capture`,
`Left`/`Right Input Mixer Boost`, `Left Boost Mixer LINPUT1`, `Right Boost Mixer
RINPUT1`, `Left`/`Right Output Mixer PCM`. The mics are on INPUT1; LINPUT2/3 and
RINPUT2/3 being off is correct.

**Capture gain is too high, though (2026-09-18).** `Capture` sits at `100%` with
both `Input Boost Mixer *INPUT1` at `100%`, and a 3 s recording peaked at full
scale on both channels (RMS −19.9 dBFS). RMS is a healthy speaking level, so the
peaks are headroom, not loudness — but clipped samples cannot be un-clipped, and
Step 4's `webrtcdsp` cancels a clipped echo far worse than a clean one. Lower
`Capture` first, one control at a time, and re-run the recording test:

```bash
amixer -c 2 sset "Capture" 80%
./check-audio.sh                 # want peak < 99%, RMS still around -20 dBFS
sudo alsactl store               # only once it is right
```

`ADC High Pass Filter` is `off`. Turning it on removes DC and low rumble and is
usually right for voice, but change it separately from the gain or neither result
means anything.

**The button, the PIR and the LED** — fitted 2026-09-23, on BCM lines **24**,
**23** and **25**. They avoid GPIO 2–3 and 18–21, which the WM8960 HAT holds
for its control I²C and its I²S, so the HAT and the doorbell coexist on one
header.

- **Button** — a plain switch to ground on GPIO24, with **no external
  resistor**, so the Pi's internal pull-up holds it high between presses.
  Active low. Debounced by the kernel at 30 ms through the line request.
- **PIR** — an **AM312** on GPIO23, powered from the Pi's **3.3 V** rail, OUT
  to the line, output 3.3 V logic. Active high, and its pulse measures
  **2–2.5 s**. Bias disabled: the AM312 drives both ways and a pull would fight
  it. No debounce — it is a clean digital output, not a contact, and repeats
  are `motion_cooldown_seconds` to handle. (An HC-SR501 would need care here
  instead: its delay pot sets the pulse width, and it wants 5 V.)
- **LED** — one colour, GPIO25 straight into a series resistor and on to
  ground. No transistor, not addressable. Six states, no PWM, so they are told
  apart by blink rate and shape; the table is `io/led_patterns.h`.

**Both inputs are read as asserted on a rising edge**, and polarity is
reconciled per line by `active_low` in the config rather than by choosing a
different edge for each. This is kernel behaviour, not a guess: `linux/gpio.h`
defines the v2 edge flags logically — `EDGE_RISING` is "rising (*inactive to
active*) edges" — and `ACTIVE_LOW` as "line active state is physical low", so
the button's physical fall to ground **is** the rising edge. Backwards, the
doorbell rings on release, which reads as a laggy button rather than as a
polarity fault.

**libgpiod 2.x is required, and 1.x is not a fallback** — the C API is an
unrelated one. Debian 13 trixie has 2.x; Debian 12 and Ubuntu 24.04 both still
ship 1.6.3, which is why none of this could be compile-checked against a
packaged header on the dev host. The backends are behind
`-DPORCHLIGHT_GPIO=ON`, **off by default**, so WSL2 never needs a dependency it
cannot install.

## Open problems

**1. ~~The WM8960 cannot be opened.~~ It records. Do not "fix" it.**

**Resolved 2026-09-18 — the EINVAL is not reproducible.** `check-audio.sh` opened
`hw:2,0` at 48 kHz stereo on the first try and captured real audio at
**−19.9 dBFS RMS**, and `webrtc-video.py` built the `alsasrc → opusenc` pipeline
against `hw:CARD=wm8960soundcard` without complaint. No repair was applied; the
card was in exactly the state described below when it worked.

Two earlier claims here were simply wrong and have been struck:

- "its PCM was never fully built" — card 2 has both `pcm0c` and `pcm0p`, and
  `asoc-simple-card` is bound to `soc:sound`.
- "`snd_pcm_open` returns `EINVAL` … at every rate" — it does not. **Why the
  original test failed was never established** and is now moot. If it ever comes
  back, do not trust this section: re-test with a plain `arecord` first.

What survives is a real but non-blocking fragility, which is why `fix-wm8960.sh`
is kept: the kernel is tainted, two machine drivers still collide at every boot,
and DKMS will rebuild Waveshare's module on the next kernel update. It works
today; it may not survive an upgrade. **Working beats clean — leave it alone
until it actually breaks.**

The collision, from `dmesg`:

```
snd_soc_wm8960: loading out-of-tree module taints kernel.
Error: Driver 'asoc-simple-card' is already registered, aborting...
```

Waveshare's installer left out-of-tree `snd_soc_wm8960*` modules plus a
`wm8960-soundcard.service`. They register a machine driver under the same name as
the kernel's in-tree `simple-audio-card`; whichever loads second aborts. Kernel 6.18
has all of this in-tree, so Waveshare's copies should be removed and the in-tree
overlay (`dtoverlay=wm8960-soundcard`, already in `/boot/firmware/config.txt`) left
to do the work.

**Confirmed on the Pi, 2026-09-18:**

- the modules are DKMS-managed: `wm8960-soundcard/1.0 … installed (Original modules
  exist)`, living at `/lib/modules/$(uname -r)/updates/dkms/`, and `modprobe` picks
  them over anything else
- **nothing** is left under `kernel/sound/soc/codecs/`. That is not damage: DKMS
  moves aside any in-tree module it shadows and restores it on `dkms remove`, which
  is what "(Original modules exist)" means. So `dkms remove … --all` is the step
  that hands the card back to the kernel's own driver.
- `/boot/firmware/overlays/wm8960-soundcard.dtbo` **differs** from the
  distribution's at `/usr/lib/linux-image-$(uname -r)/overlays/`. Waveshare replaced
  it, and it asks for their machine driver, so it has to go back or nothing binds.
- `wm8960-soundcard.service` is enabled and active (and marked executable, which
  systemd complains about at boot)

**`pi/fix-wm8960.sh` is the tool if it does break.** Not needed now. It replaces the
hand-run recon that used to be listed here, because the recon and the cleanup have
to agree about what they found.

```bash
scp pi/fix-wm8960.sh israellior1@192.168.0.129:   # nothing serves it any more
chmod +x fix-wm8960.sh
./fix-wm8960.sh              # survey. Read-only, and the default.
sudo ./fix-wm8960.sh --apply # then: sudo reboot
sudo ./fix-wm8960.sh --restore   # if the card is worse afterwards
```

`--apply` **never deletes**. Everything it takes away is moved to
`/var/backups/wm8960-fix` with a manifest, and `--restore` puts all of it back,
which is what makes the "a wrong `rm` costs a reflash" risk survivable.

The survey decides each module's origin two ways and makes them agree: where the
file sits (`updates/` and `extra/` are Waveshare's, `kernel/sound/soc/` is the
kernel's) *and* what `modinfo -F intree` says about it. That second test is the one
that matters — it catches an installer that **overwrote** the in-tree module rather
than shadowing it, which needs `apt-get install --reinstall` and not a move. On that
finding the script refuses to `--apply` and names the package.

It checks the overlay the same way: on Raspberry Pi OS the `.dtbo` files in
`/boot/firmware/overlays` are *copies* that dpkg does not own, so the pristine
original under `/usr/lib/linux-image-$(uname -r)/overlays/` is the yardstick for
whether Waveshare replaced it.

After the reboot the in-tree driver starts from **its own** mixer defaults, so the
routing verified below will not have carried over. Run `./check-audio.sh`, apply the
`amixer` lines it prints, then `sudo alsactl store`.

**2. ~~Still not a git repository.~~ Fixed 2026-09-20** — but it cost something
first. The PCM intercom was deleted on 2026-09-19, eight files and half of
`server.js`, with no way to get any of it back. Work goes on `development` and
reaches `main` when it is confirmed working.

**3. ~~A secure context is needed from Step 3 on.~~ Not ours any more.**
`getUserMedia` still refuses outside a secure context, but nothing on this side
calls it: the Pi does not use a browser, and the viewer is the app, which is
served over HTTPS by the app server. This was only ever a constraint on
`public/webrtc.html`, which was deleted on 2026-09-25.

It is recorded because the reasoning applies to the **device's own setup page**,
which is the next thing to be built: a page the doorbell serves over plain HTTP
on its own access point, to be given a Wi-Fi password. **`http://localhost` is a
secure context and `http://192.168.4.1` is not**, so anything on that page that
needs a secure context — `getUserMedia`, `crypto.subtle` — will refuse, and it
has to be designed around rather than discovered. A form post is fine; a QR
scanner in the browser is not.

**4. ~~Restart the server after pulling changes.~~ There is no dev file server
any more, and nothing replaced it.** `server.js` went on 2026-09-25 with the
rest of the LAN stub, so the Pi's scripts and the daemon's tarball arrive by
`scp` and by nothing else. That is a gap rather than a decision: a shipped
device cannot be updated by hand at all, and the answer is the factory image
and an updater, neither of which exists.

**What survives is how a connection to the server host fails**, because the app
server on :4000 is reached the same way and the symptoms still tell the causes
apart. Reading one as another costs an hour:

| What you see | What it is |
|---|---|
| fast connection refused | nothing is listening — the server is not running |
| **hangs, 0 bytes, no error** | nothing answers the SYN: the Windows firewall, see "The server host" |

A hang is never a wrong path. Check `Get-NetTCPConnection -LocalPort 4000
-State Listen` on the server host first: if something *is* listening and the Pi
still hangs, it is the firewall profile every time.

**5. The Windows clock, which is now load-bearing rather than cosmetic.**
Measured 2026-09-18 at **1.76 s fast** and never synced. Re-measured
2026-09-22: **+0.28 s**, so it has been resynced at some point since and is
fine.

It matters more than it did. It used to inflate only the on-screen glass-to-
glass delay, which is read by a human who can subtract. The app server now
mints LiveKit tokens on this machine with **`nbf` set to the mint time**, so if
its clock ever runs ahead of LiveKit's, every token is *not yet valid* on
arrival — and nothing on the Pi would say so in those words. go-jose's default
leeway is a minute, so there is a lot of room, but the failure mode is
silent-looking and worth knowing exists.

```powershell
w32tm /stripchart /computer:time.windows.com /samples:3 /dataonly   # measure
w32tm /resync                            # if the service is already configured
net start w32time; w32tm /resync         # if it is not running
```

A **negative** stripchart offset means the local clock is **ahead** of true
time, which is the direction that breaks tokens.

**6. The chime cannot play during a call, and nothing handles that.**
`porchlightd`'s `AlsaChime` opens `plughw:CARD=wm8960soundcard` and the call
holds the same card for as long as it is up, so a doorbell press during a live
view will fail to chime. The daemon's own rule is that *the chime never waits
for anything* — it is a local effect and plays whether or not the server has
ever been reachable — and this quietly breaks that. Neither half knows about
the other. Options are a `dmix` plug so both can open it, or routing the chime
through the call's own pipeline. Not yet decided, and it is a real gap.

**7. ~~Nothing installs the call on a Pi.~~ `pi/provision.sh` does, as of
2026-09-25 — and it has never been run.** It installs the packages, the
`porchlight` user and its groups, the venv with `--system-site-packages`, all
three Python halves, the config, the credential, the chime and the unit, then
proves the link three ways and refuses to enable the service until it passes.
Survey by default, `--apply` to act, `--verify` for the proofs alone.

Two things it deliberately does not do. **It does not build the daemon**: a
build takes minutes and fails in ways worth reading — a missing libgpiod 2.x
above all — and burying that in a provisioning run turns one clear compile
error into "the script failed". And **it does not fetch anything over HTTP**:
it works from the source tree beside it, so it does not care whether the tree
arrived by tarball, by scp or by clone, and it needs `porchlightd/` there
anyway for the unit file and the example config.

What remains is packaging: there is still no `.deb` and no image, so the tree
has to reach the Pi somehow before any of this runs.


## The codebase

```
CLAUDE.md               This file: what exists, and how to work on it.
RUNBOOK.md              Cold start, in order: which terminal, which machine,
                        which command. Assumes everything is already installed.
DESIGN.md               How the finished system is meant to work. Settled decisions.
docs/architecture.md    The three flows as built - alerts, clips, live stream -
                        across the Pi, the app server and the browser. Written
                        from the code. What server-brief.md specifies, this one
                        describes.
docs/server-brief.md    What the app server has to provide, written for whoever
                        builds it. Alerts, the three clip steps, LiveKit tokens.
                        Where this repo and that document disagree, that one wins.
docs/app-server-changes.md
                        The follow-up to the brief: what the app server has to
                        change for a device's first connect, and for a network
                        that comes and goes. Written against the real backend
                        rather than from the brief, so it names files. Two items
                        are required (4002 is a site visit; a minted token is
                        not proof a call started) and the rest are gaps the
                        device survives.
porchlightd/            The C++20 doorbell daemon, with its own README and its own
                        step table. Decides when to alert, record, chime and call.
                        src/io/gpio_input.* is the button and the PIR on one
                        libgpiod request; src/io/gpio_led.* drives the LED and
                        owns the timer that blinks it; src/io/led_patterns.h is
                        the blink table, free of libgpiod so the tests can read
                        it on a machine with no GPIO. All behind -DPORCHLIGHT_GPIO.
public/porchlightd.tar.gz   Not tracked. `git archive` output, so the daemon's
                        source can reach the Pi - it has no clone. Stale by
                        default: regenerate it after every change. Nothing
                        serves it any more; it is scp'd. See Conventions.
pi/webrtc-video.py      The live call. Camera and mic into LiveKit under a publisher
                        token, a viewer's mic back out of the speaker under a
                        listener token, webrtcdsp between them. No SDP, no socket
                        of ours. --dry-run is the pipeline alone, --check the
                        tokens alone. The name is stale and the references are not.
pi/server-bridge.py     porchlightd's WebSocket to the app server, as a child
                        process: C++ has no WebSocket, python3-websocket is here.
                        --check proves the credential over HTTP, --probe proves
                        the socket - a network can carry one and drop the other.
pi/upload-clip.py       One clip, in three steps - signed url, PUT, confirm. Judged
                        by its exit code alone; only the confirm earns a 0.
pi/porchlight-firstboot.py
                        One image becomes one doorbell. Reads the birth
                        certificate off the FAT boot partition, installs the
                        credential 0600, patches device_id and base_url into
                        porchlightd.json, sets the hostname, destroys the file.
                        Idempotent; exit 2 means the card was never minted.
pi/porchlight-setup.py  Wi-fi from a phone. If the device is on no network it
                        becomes one - WPA2 access point, captive portal, a page
                        that lists what it scanned *before* the AP came up -
                        takes an SSID and a password, joins, and verifies in
                        stages so a wrong password and a network with no
                        internet are different sentences. Owns the LED until it
                        exits, which is when porchlightd starts.
pi/systemd/             The two units for those two, ordered before the daemon.
pi/tests/               Shell drills for both, no Pi needed. firstboot-drill.sh
                        looks at the files; setup-drill.sh drives the whole flow
                        against --fake-nm, a pretend radio.
pi/provision.sh         A fresh Pi to a connected doorbell, in one pass: packages,
                        user, venv, scripts, config, credential, unit - then the
                        three proofs, and it will not enable the service until
                        they pass. Surveys by default like fix-wm8960.sh; --apply
                        acts, --verify runs only the proofs. Never takes the
                        credential as an argument. --image is the factory
                        build: enable the boot units, expect no credential.
pi/check-audio.sh       Read-only hardware audit: card, driver conflicts, mixer, a real
                        recording with levels, GStreamer elements, Python bindings.
pi/check-camera.sh      The same for the camera: overlay, sensor driver, what
                        libcamera sees, a real still, and timed GStreamer capture
                        through each encoder. Read-only.
pi/check-livekit.sh     The same for the call: the venv and the SDK, the GStreamer
                        elements, the credential and what the two tokens grant,
                        and whether anything else holds the camera or the card.
                        Read-only. Run it before the first call on a Pi.
pi/fix-wm8960.sh        Surveys Waveshare's installer and undoes it reversibly.
                        --apply moves files to /var/backups/wm8960-fix, --restore
                        puts them back. Refuses to act on anything ambiguous.
```

**The LAN development stub was deleted on 2026-09-25**: `server.js`, which
served the Pi's scripts by name and ran a signaling switchboard nothing used;
`public/webrtc.html`, the Steps 1-3 viewer that no Pi had answered since the
call moved to LiveKit; `tools/signal-test.js`, the fake peer for exercising
that switchboard; and `tools/clip-stub.js`, the faked clip endpoints. All four
are in the git history and nothing in the tree is Node any longer —
`package.json` still declares express, `ws` and a `start` script for a file
that is gone.

What went with them is the **delivery mechanism**: nothing serves `pi/*` or the
tarball, so both reach the Pi by `scp`. For a device that ships to somebody's
house that was never going to be the answer anyway; the factory image and an
updater are, and neither exists yet.

## The signaling protocol

**This is the *development* protocol. Nothing in this repository speaks it any
more, and nothing implements it either** — `server.js`, `public/webrtc.html` and
`tools/signal-test.js` were the three sides of it and all three were deleted on
2026-09-25. The protocol the Pi actually speaks to the real app server is in
[porchlightd/docs/protocol.md](porchlightd/docs/protocol.md): one socket, held
by the daemon as `role: "device"`, carrying alerts out and `viewer-requested`
in, and no SDP in either direction.

It is written down here because Phase 2 means writing this layer by hand again,
and because the shapes below are what the hand-negotiated version used. Treat
it as a record, not as an interface: there is no code behind it.

All JSON text frames on `/ws`. **The server never parses SDP or candidates** —
it assigns ids, remembers which socket is the Pi, and forwards by `to`, stamping `from`.

Client → server:

- `{type:'hello', role:'pi'|'browser'}` — sign in. A second Pi replaces the first.
- `{type:'request-offer', to}` — a browser asks the Pi to start.
- `{type:'offer'|'answer', to, sdp}`
- `{type:'ice', to, candidate:{candidate, sdpMLineIndex}}`
- `{type:'bye', to}`

Server → client:

- `{type:'welcome', id}` — your own peer id.
- `{type:'status', pi}` — `pi` is the Pi's id or `null`. Broadcast on every peer
  change, so **guard against asking for an offer twice**.
- `{type:'peer-left', id, role}`
- `{type:'signal-error', about, to}` — the named peer was not connected.
- Anything relayed, with `from` added.

The Pi makes the offer (it has the media), the browser answers. ICE candidates from
GStreamer can overtake the offer, so the page queues candidates until
`setRemoteDescription` has run.

**GStreamer gotcha, already hit once:** read `offer.sdp.as_text()` *before* passing
the description to `set-local-description`. That call empties the Python wrapper's
`.sdp`, and the text is then unrecoverable. The same will apply to `create-answer`
in later steps.

**No STUN or TURN.** Both machines are on one LAN, so ICE host candidates are enough.
`iceServers: []` in the page was deliberate.

**Two media, one connection.** `bundle-policy=max-bundle` puts both m-lines on one
ICE transport and one UDP port pair, so there is a single candidate pair to watch
in the stats. The browser builds its own `MediaStream` and adds every incoming
track to it rather than trusting `e.streams[0]`: one element rendering one stream
is what lets it hold the two tracks together.

## Running it

Nothing from this repository runs on the server host any more — the app
server is a separate project, and `RUNBOOK.md` is what starts it.

On the Pi. **`pi/provision.sh --apply` does everything below**, and checks it;
what follows is the same list by hand, which is what to read when one step of
it fails. There is no git clone there and nothing serves the files any more, so
the tree arrives by `scp`:

```bash
git archive --format=tar.gz --prefix=porchlight/ HEAD -o /tmp/porchlight.tar.gz
scp /tmp/porchlight.tar.gz israellior1@192.168.0.129:
# then on the Pi: tar xzf porchlight.tar.gz && cd porchlight
```

The LiveKit SDK is a pip package and `python3-gi` is an apt one, so the call
runs under a venv that can see both. **`--system-site-packages` is not
optional** — without it the SDK imports and `import gi` does not, which reads
as a GStreamer problem and is not one:

```bash
sudo apt install -y gstreamer1.0-libcamera rpicam-apps gstreamer1.0-alsa      gstreamer1.0-plugins-bad python3-gi python3-gst-1.0
sudo python3 -m venv --system-site-packages /opt/porchlight/venv
sudo /opt/porchlight/venv/bin/pip install livekit
```

Then, in this order — each one takes a different half out of the picture:

```bash
./check-livekit.sh http://192.168.0.219:4000 porch-1    # the parts, before the whole

PY=/opt/porchlight/venv/bin/python3
ARGS="--url http://192.168.0.219:4000 --device-id porch-1 --credential-file ./credential"

$PY webrtc-video.py $ARGS --dry-run       # pipeline only: camera, card, caps. No network.
$PY webrtc-video.py $ARGS --check         # tokens only: the credential. No camera.
$PY webrtc-video.py $ARGS --video test    # the whole call, camera taken out of it
$PY webrtc-video.py $ARGS                 # the real thing
```

and the rest of the switches:

```bash
$PY webrtc-video.py $ARGS --focus 1.5             # fix the lens, no AF hunting
$PY webrtc-video.py $ARGS --size 1280x720         # width must be a multiple of 8
$PY webrtc-video.py $ARGS --codec vp8             # if H.264 misbehaves in the app
$PY webrtc-video.py $ARGS --aec off               # it will howl; proves AEC is the cause
$PY webrtc-video.py $ARGS --no-talkback           # one way, but still watches the room
$PY webrtc-video.py $ARGS --audio test            # a tick, while the HAT is down
$PY webrtc-video.py $ARGS --audio none            # video only
$PY webrtc-video.py $ARGS --speaker-device hw:CARD=Headphones
$PY webrtc-video.py $ARGS --pattern smpte         # --video test: bars, not the ball
```

Normally none of this is typed: `porchlightd` runs it on `viewer-requested`,
with `backends.media: "script"` and the `media` block in its config saying which
interpreter and which switches.

**The camera and the sound card each go to one process at a time.** A stray
`rpicam-hello`, a second copy of the call, or `porchlightd`'s own recorder will
hold either, and the failure reads as "cannot open" rather than "something else
has it". `check-livekit.sh` names the pid. It is also why the core stops a
recording and waits for its EOS before it starts a call — and why the chime
cannot play while a call is up, which is a real gap and not yet solved.

`--mic-device` and `--speaker-device` both default to `hw:CARD=wm8960soundcard`.

**The Steps 1–3 viewer is gone.** `public/webrtc.html` asked a Pi for an offer,
and no Pi has answered since the call moved to LiveKit; it was deleted on
2026-09-25 with the rest of the stub. The git history is the record of what
those steps were.

## Conventions

- **Comments say why, not what.** The existing code explains the non-obvious
  decisions (why a video frame is copied with `bytes()` rather than passed as a
  memoryview, why the credential travels as a path, why `provide-clock=false` is
  on the recorder's `alsasrc` and nowhere else). Match that.
- **Counted, never inferred.** A track publishing, a pad appearing and a
  bitrate in the viewer's browser can all look healthy while nothing moves. Every
  direction has a counter at the point where it hands over, and the two-second
  report prints them. This habit has located three separate faults.
- **A property that may not exist is set in Python, not in a launch string.**
  `set_focus`, `set_sensor_mode` and `tune_dsp` all read `list_properties()`
  first and print a note. A missing property inside `parse_launch` takes the
  whole pipeline down.
- **No build step, no framework, no bundler.** Plain files served statically.
  The one exception is the Pi's venv, which the LiveKit SDK forces — see open
  problem 7.
- **"One at a time, and a new one replaces the old."** Still how the daemon's
  socket signs in (`role: "device"`, replacing its own predecessor). It no
  longer applies to the call: **one media process serves every viewer**, because
  the Pi publishes one stream and LiveKit copies it out. 1–5 viewers per Pi is
  the target, and that now costs the device nothing.
- **Scripts reach the Pi by `curl` from the server**, never by paste — an indented
  heredoc terminator silently breaks a pasted script. Files here use LF endings; if
  one ever arrives with CRLF, `sed -i 's/\r$//'`.
- **`porchlightd`'s source reaches the Pi the same way, as a tarball.** There is no
  git clone on the Pi, so `git pull` there is not a thing — it fetches an archive
  from `public/`, which express already serves statically. **Regenerate it after
  every change or the Pi silently builds yesterday's code**, which is exactly what
  happened on 2026-09-22:

  ```bash
  git archive --format=tar.gz --prefix=porchlightd/ HEAD:porchlightd \
    -o public/porchlightd.tar.gz
  ```

  ```bash
  scp public/porchlightd.tar.gz israellior1@192.168.0.129:   # nothing serves it
  tar xzf porchlightd.tar.gz && cd porchlightd
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
  ctest --test-dir build --output-on-failure
  ```

  `git archive` takes only tracked files, so the `build*/` directories and their
  vendored GoogleTest never go near it. The tarball is gitignored.

  **`pi/provision.sh` needs more than that tarball**, which holds `porchlightd/`
  alone. It works from a tree with both `pi/` and `porchlightd/` in it — it
  installs the three Python halves, and it reads the unit file and the example
  config out of `porchlightd/`. Whole-repo archive, then:

  ```bash
  git archive --format=tar.gz --prefix=porchlight/ HEAD -o public/porchlight.tar.gz
  ```

  It fetches nothing over HTTP itself, deliberately: however the tree arrived
  — tarball, scp or clone — is not its business.
- **Nothing here is Node any more.** The last of it went with the LAN stub on
  2026-09-25. `package.json` and `node_modules/` are still in the tree and
  describe a `server.js` that is not.

## Keeping this file current

At the end of a session that changes any of the above, update: the step table, the
open problems, the file map, and the protocol section if message shapes moved.
Prefer editing a line over appending; this is a description of the present, not a log.
