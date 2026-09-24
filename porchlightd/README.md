# porchlightd

The device-side control daemon for Porchlight. It decides what the doorbell
does — chime, alert, record, call — and carries none of the media itself.

The live call is a **separate program**, `webrtc-video.py`, with its own
pipeline. This daemon starts it and learns that it stopped, and that is the
whole interface — there is no socket between them and nothing to say on one.

It no longer has a WebSocket of its own either. The call publishes to LiveKit,
so the script fetches its own short-lived tokens with the device credential and
negotiates with LiveKit directly. **This daemon's socket is the only one the
device holds**, and it signs in as `role: "device"`.

## Where we are

Each step adds exactly one thing that can break.

| Step | What | Status |
|---|---|---|
| 0 | git init, first commit | **done** |
| 1 | Schema doc, skeleton: CMake, loop, logging, config, signals | **done** |
| 2 | The core and its unit tests | **done — 65 passing** |
| 3 | Fake backends, so the whole flow runs from the keyboard | **done** |

Verified on both machines: WSL2 GCC 13.3 and the Pi's GCC 14.2, Debug and
Release, 0 warnings — 48/48 each when the Pi last ran them, **83/83 on WSL**
since the live call went in. There are two test binaries: `core_tests` links
only `porchlight_core`, which is what keeps "no I/O in the core" true rather
than merely intended, and `runtime_tests` reaches into `porchlight_runtime` to
check the GStreamer command line the recorder builds and the argv the daemon
runs a call with — both strings, so neither needs a camera, a sound card, a
network or a Pi. Build Release before believing a clean build —
`-Wmaybe-uninitialized` does nothing at `-O0`, and that hid 22 reports for a
while.

The recorder is confirmed on the Pi against real GStreamer 1.26.2:
`gst-discoverer-1.0` reads back a seekable 14.72 s clip, H.264 constrained
baseline at 640×480/30 under a 2048 kbit/s ceiling, and mono AAC at 48 kHz.
WSL2 has no GStreamer at all, so that check can only ever happen on the Pi.
The WM8960 microphone is confirmed too, and cost two fixes worth remembering.

Each branch feeding the muxer needs a `queue`. Without one the audio chain
runs in `alsasrc`'s own thread — resample, AAC encode and the push into
`mp4mux` all happen before it can read the card again — and a quarter of the
samples were dropped. And the recorder deliberately uses a **200 ms** ALSA
buffer, not the 40 ms `webrtc-video.py` uses: a call trades buffer for
latency, a recording has no latency requirement at all.

That broken clip still passed every check the daemon makes — exit 0, plausible
size, valid moov atom, right duration. Only GStreamer's own warnings on stderr
gave it away, which is why the child's output is not redirected.

| 4 | Recorder: gst-launch as a child, real playable MP4 | **done** |
| 5 | Server link: alerts to the real app server | **done** |
| 6 | Clips: sidecar, real upload, spool cap | **done, against a stub** |
| 7 | The same against the real `/api/clips/...` | next, and not ours alone |
| — | Camera: real footage in the clips | **done 2026-09-22** |
| — | Media: `viewer-requested` really starts a call | **done 2026-09-22, untested on hardware** |
| — | LED, button, PIR | **written 2026-09-23 — never run on the Pi** |

**The live call (2026-09-22).** `backends.media: "script"` runs
`webrtc-video.py` as a child process on `StartCall`, watched through a pidfd
exactly as the recorder and the uploader are. Three things about it are rules
rather than details, and all three are asserted in
[`tests/media_test.cpp`](tests/media_test.cpp):

- **The interpreter is configurable and the script is not exec'd directly.**
  The LiveKit SDK is a pip package and `python3-gi` is an apt one, so the call
  runs under a venv built with `--system-site-packages`. A shebang would find
  the system python and die on `from livekit import rtc`.
- **The credential travels as a path, never as a value.** Arguments are
  world-readable in `/proc`.
- **One process serves every viewer.** A second `viewer-requested` while it is
  running is nothing to act on: the Pi publishes one stream and LiveKit copies
  it out.

The call ends by itself — it is the half that can see who is in the room — and
the process exiting is the only thing this side ever observes. Every viewer it
was started for gets one `CallEnded` then, because a viewer left in the core's
set keeps the LED on `live` and holds back every clip upload.

**The camera (2026-09-22).** `recorder.video_source: "libcamera"` now records a
real Camera Module 3 Wide instead of `videotestsrc`, and the example config is
set that way. Four things had to go with it, all of them learned the hard way in
`webrtc-video.py` the same day and all of them asserted in
[`tests/pipeline_test.cpp`](tests/pipeline_test.cpp):

- **`alsasrc provide-clock=false`, but only when the source is the camera.**
  `alsasrc` offers the sound card as the pipeline clock and `libcamerasrc`
  offers none, so with both present GStreamer picks the card's — while
  `libcamerasrc` goes on timestamping from the system monotonic clock whatever
  was chosen. Every video buffer then carries a running time worked out across
  two time bases. In `webrtc-video.py` that stalls the video branch within a
  second, silently, while audio runs on perfectly. There the fix is
  `pipeline.use_clock()`; from a `gst-launch` command line there is no such
  call, so the card declines the job instead. The `videotestsrc` path is left
  exactly as it was verified, because `videotestsrc` follows whichever clock
  the pipeline picked and never had the problem.
- **`sensor-config` pinned to 2304×1296.** Otherwise libcamera picks the
  imx708's binned 1536×864 mode, which reads only the centre 3072×1728 of the
  array — a third of the frame width gone, on the lens bought for its width.
- **`af-mode=continuous`.** It defaults to *manual* at `lens-position` 0, and
  0 dioptres is infinity: an unconfigured camera films the horizon while the
  caller stands a metre away.
- **`format=I420` on the camera caps**, or it negotiates NV21 and the
  `videoconvert` de-interleaves every frame instead of passing it through.

Two warnings at startup rather than silent surprises: a non-16:9 `width`/
`height` (the imx708 is 16:9, so 4:3 is cropped by the ISP), and
`encoder: "v4l2"`, which **stalls outright when fed by `libcamerasrc`** — even
a bare `libcamerasrc ! videoconvert ! v4l2h264enc ! fakesink` produces nothing,
where `x264enc` runs at 30 fps. Not yet investigated; `x264` is the tested path
with the camera.

Out of scope for now: libgpiod, the kernel driver, pre-roll recording,
first-boot provisioning.

## Build

Develop on WSL2 Ubuntu (GCC 13), deploy to Raspberry Pi OS Lite 64-bit on a
Pi 4 (GCC 14). C++20.

```bash
cmake -S . -B build
cmake --build build -j4
./build/tests/core_tests
```

`nlohmann/json` and GoogleTest are found if installed and fetched if not. On
the Pi, skip the tests and the download with `-DBUILD_TESTING=OFF`.

Sanitizers, for the test build:

```bash
cmake -S . -B build-asan -DPORCHLIGHT_SANITIZE=ON
cmake --build build-asan -j4
./build-asan/tests/core_tests
```

## Run it from the keyboard

With the default config every backend is a fake, so the whole doorbell runs on
a laptop with no camera, no button and no server:

```bash
./build/porchlightd ./porchlightd.example.json
```

| Type | What it stands for |
|---|---|
| `button` | someone pressed the doorbell |
| `motion` | the PIR fired |
| `viewer <peer>` | someone opened the live view |
| `call-ended [peer]` | they closed it |
| `online` / `offline` | the server link came up or went down |
| `quit` | SIGTERM |

The last two have no real equivalent — they exist because the alert queue, the
ageing and the backoff are otherwise unreachable without unplugging something.

`porchlightd.example.json` uses the real recorder (`"recorder": "gstreamer"`)
and needs GStreamer; `porchlightd.demo.json` uses the fake one and runs
anywhere. To see the command the recorder will run without running it:

```bash
./build/porchlightd --print-pipeline ./porchlightd.example.json
```

Worth trying: type `motion`, then `button` two seconds later, and watch one
event get upgraded to a ring — same id, second alert, no second clip.

## Talking to the app server

`backends.server = "bridge"` is the real link.
[`pi/server-bridge.py`](../pi/server-bridge.py) holds the WebSocket and the
daemon exchanges JSON lines with it over pipes — C++ has no WebSocket, and this
Pi already runs `python3-websocket` for the media script, so the socket lives
where the library is and the daemon keeps no network dependencies.

**Verified on hardware against the live server**: `role: "device"` accepted,
`motion` and `ring` both delivered from a button press with no hand-typed JSON,
and the app shows them.

Three rules the transport must honour, all exercised rather than assumed:

| server says | device does |
|---|---|
| `ok: true` | done, never sent again |
| `ok: false` | **permanent** — dropped, never retried, reason logged |
| *no reply at all* | **transient** — retried with backoff, forever |

Getting the last two the wrong way round either loses real doorbell presses or
retries garbage forever. `at` is the moment the sensor fired and stays
unchanged across every retry, so an alert held through an outage still reports
when the person was at the door.

Both Python halves live in `/usr/local/lib/porchlight/` — `server-bridge.py`
for the socket and `upload-clip.py` for the clips, named by `server.bridge_path`
and `server.uploader_path`. `server.js` serves both by name, so the Pi fetches
them the same way it fetches everything else:

```bash
cd /usr/local/lib/porchlight
sudo curl -fO http://192.168.0.219:3000/server-bridge.py
sudo curl -fO http://192.168.0.219:3000/upload-clip.py
sudo chmod +x server-bridge.py upload-clip.py
```

Set your own settings in `/etc/porchlight/porchlightd.json` rather than editing
the tracked example, or every `git pull` will fight you.

## Deploying to the Pi for real

Four things on the device, and only the credential is secret.

```bash
sudo install -m 0755 build/porchlightd /usr/local/bin/porchlightd

sudo mkdir -p /usr/local/lib/porchlight /etc/porchlight
sudo install -m 0755 ../pi/server-bridge.py ../pi/upload-clip.py /usr/local/lib/porchlight/

sudo cp porchlightd.example.json /etc/porchlight/porchlightd.json
sudo nano /etc/porchlight/porchlightd.json      # set server.base_url, and
                                                # backends.input/led to "gpio"
```

The example config still ships `input: "stdin"` and `led: "console"`, because
they are what a binary built without `-DPORCHLIGHT_GPIO=ON` can run. The pin
numbers in its `gpio` block are the real ones.

**The credential is typed onto the device and lives nowhere else.** It is not
in this repository, it is not fetched over HTTP, and the server keeps only a
hash of it — a lost one is re-minted, never looked up.

```bash
printf '%s' 'pl_porch-1_...' | sudo tee /etc/porchlight/credential > /dev/null
sudo chmod 0600 /etc/porchlight/credential
sudo chown "$(whoami)" /etc/porchlight/credential     # see the ownership trap below
```

`printf` rather than `echo` because the credential is compared verbatim;
`server-bridge.py` strips whitespace but nothing should depend on that. Check it
before starting anything:

```bash
/usr/local/lib/porchlight/server-bridge.py --url <SERVER_URL> \
  --device-id porch-1 --credential-file /etc/porchlight/credential --check
```

That prints the device's name and location straight from the server, and proves
the credential, the URL and the network in one call without opening a socket.

**Two traps in the systemd unit.**

The unit runs as `User=porchlight`, so a credential at `0600` owned by anyone
else is unreadable and the daemon fails with a permission error rather than an
authentication one. `chown porchlight` it when the unit is what starts the
daemon, and `chown` it to yourself while you are running it by hand.

Second, the unit sets `StandardInput=null`, so a unit left on
`backends.input: "stdin"` would **start, connect, and never see an event** —
nothing can type at it. Set `input` and `led` to `"gpio"` before enabling it,
and build the binary with `-DPORCHLIGHT_GPIO=ON` or it will refuse to start
and say which. The unit already grants `SupplementaryGroups=audio video gpio`,
which is what lets `porchlight` open `/dev/gpiochip0`.

To run it in a terminal instead — which is still how the fake backends are
driven:

```bash
/usr/local/bin/porchlightd /etc/porchlight/porchlightd.json
```

The spool is the other thing systemd normally provides: `StateDirectory=porchlight`
creates `/var/lib/porchlight` and gives it to the right user. Running by hand,
either create it yourself or point `spool.path` somewhere you own.

## Clips

`backends.uploader = "script"` is the real one. It writes `<eventId>.json`
beside the MP4, spawns [`pi/upload-clip.py`](../pi/upload-clip.py), and believes
its **exit code and nothing else**: 0 means the confirm answered 2xx, and that
is the only thing that permits deleting the local copy. Anything else leaves
both files where they are for the next attempt, which starts again at step 1
because signed URLs expire.

The sidecar *is* the confirm body. Four of its fields are known only to the
core at the moment a recording ends, which is why `UploadClip` carries them:

| field | where it comes from |
|---|---|
| `kind` | the event, raised to `ring` if a press upgraded it |
| `at` | when the **sensor** fired, never when the upload happened |
| `durationMs` | what the recorder actually produced |
| `partial` | something stopped the recording early — a viewer, or a shutdown |

`at` stays the motion's time even when a press upgrades the event: the kind is
raised because somebody is at the door, but the clip still begins where the
recording did.

`spool.max_bytes` is now enforced, in the core rather than by sweeping the
directory — the core is the only thing that knows which clips are still owed to
the server, and a sweep would eventually delete the file underneath a running
upload. Oldest first, and never the one in flight.

**Verified against [`tools/clip-stub.js`](../../tools/clip-stub.js), not
against the real server** — see below. What is left:

1. **A startup rescan** of the spool. Still deferred, and it needs a way to
   inject a found clip back into the core. Until it exists, a clip whose daemon
   died between the recording and the upload is never sent and never deleted.
2. **A clip whose file has gone wedges the queue.** The core retries the front
   of the queue forever and only pops it on success, so a missing file means
   `upload-clip.py` fails every time and nothing behind it moves. It takes an
   outside hand deleting from the spool, so it is not urgent — but the fix is
   an outcome the uploader can report as permanent, the way alerts already
   distinguish a rejection from a failure.

**The real `/api/clips/...` endpoints return 404 today.** The shapes are agreed
and still free to move; [`docs/server-brief.md`](../../docs/server-brief.md) is
what the other side is building from.

### Running it against the stub

The stub is the three endpoints and a bucket, and — more usefully — the ways
they fail, which a real server will not do on request.

```bash
node tools/clip-stub.js --out ./received     # on the server host
node tools/clip-stub.js --fail-once confirm  # the retry is the interesting case
node tools/clip-stub.js --fail confirm       # nothing ever finishes
```

Then point `server.base_url` at it. A confirmed clip disappears from the spool
and appears under `--out`, with its sidecar as the server received it.

## The chime

`backends.chime` picks one of three: `console` logs `ding`, `busy` always
refuses (standing in for a card held by a live call), and `alsa` plays a file
through the speaker with `aplay`.

It is best effort by design. A missing file, a missing `aplay` and a busy
device are each one warning and nothing more — the alert goes out over a
different path, so a silent doorbell still notifies.

There is no sound file in the repo. To make a two-tone one on the Pi:

```bash
gst-launch-1.0 -e concat name=c ! audioconvert ! wavenc \
  ! filesink location=chime.wav \
  audiotestsrc wave=sine freq=784 num-buffers=18 \
    ! audio/x-raw,rate=48000,channels=1,format=S16LE ! c. \
  audiotestsrc wave=sine freq=622 num-buffers=32 \
    ! audio/x-raw,rate=48000,channels=1,format=S16LE ! c.
```

```bash
sudo mkdir -p /usr/local/share/porchlight
sudo mv chime.wav /usr/local/share/porchlight/
```

Any WAV will do — `chime.sound` in the config points wherever you like, and
the device defaults to `plughw:` rather than `hw:` so ALSA converts a file at
the wrong rate instead of refusing it.

Two things follow from one sound card. The chime **cannot** play while a call
holds the speaker, and because a button press starts a recording at the same
moment, the chime ends up **inside** every ring clip.

## The button, the PIR and the LED

Everything sits behind an interface, and one file chooses the implementation:
[`src/backends.cpp`](src/backends.cpp). Adding a real backend is a branch there
and a line of JSON. **No core change**, and the fake stays available to fall
back to when something misbehaves at 11pm.

| Part | Config change | Files |
|---|---|---|
| LED | `backends.led: "gpio"` | **written 2026-09-23** — `io/gpio_led.*`, `io/led_patterns.h` |
| Button + PIR | `backends.input: "gpio"` | **written 2026-09-23** — `io/gpio_input.*` |
| Camera | `recorder.video_source: "libcamera"` | **done** — see "The camera" above |
| Real recorder | `backends.recorder: "gstreamer"` | step 4 |
| Live call | `backends.media: "script"` | **done** — `io/script_media.*` |

An unknown backend name is a startup error naming the key, never a silent
default — and `"gpio"` in a binary built without it gets a *different* error
saying so, because reading "not compiled in" as "misspelt" costs an hour.

**Nothing here has run on a Pi.** It is written against libgpiod 2.2's real
header and compiles clean under `-Wall -Wextra -Wpedantic -Wshadow`, which
proves the API and nothing about the wiring.

### Building it

The backends are behind a CMake option, **off by default**, so the Windows and
WSL2 development machines never need a dependency they cannot use:

```bash
sudo apt install -y libgpiod-dev          # must be 2.x
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPORCHLIGHT_GPIO=ON
```

**libgpiod 1.x will not do**, and the likely failure is not "missing" but
"found 1.6": its C API is an unrelated one. Debian 13 trixie, which the Pi
runs, has 2.x; Debian 12 and Ubuntu 24.04 both still ship 1.6.3. CMake says
which it found rather than leaving pkg-config to word it.

### The wiring, as built

BCM numbering, and confirmed against `gpioinfo` rather than assumed. They
avoid GPIO 2–3 and 18–21, which the WM8960 HAT holds for its control I²C and
its I²S.

| Line | Part | Rests | Bias |
|---|---|---|---|
| 24 | button | high | the Pi's internal **pull-up** — there is no resistor on the switch |
| 23 | PIR (AM312) | low | **disabled** — the AM312 drives both ways and a pull would fight it |
| 25 | LED | off | output, straight into a series resistor and on to ground |

**Both inputs are read as asserted on a *rising* edge**, and the wiring is
reconciled by `active_low` per line rather than by picking a different edge for
each. That is not a guess about the kernel: `linux/gpio.h` defines the v2 edge
flags logically — `EDGE_RISING` is "rising (**inactive to active**) edges" —
and `ACTIVE_LOW` as "line active state is physical low". So with
`button_active_low`, the switch's physical fall to ground *is* the rising edge.
Get it backwards and the doorbell rings on release, which reads as a laggy
button rather than as a polarity mistake.

`button_active_low`, `motion_active_low` and `led_active_low` are all config,
so re-wiring any of the three is a JSON change and not a rebuild.

**Debounce is the kernel's**, asked for through the line request
(`debounce_ms`, applied to the button alone — a PIR has nothing to bounce).
That is why the `io/debounce.h` the plan called for was never written: the v2
uAPI debounces in the kernel, and a userspace copy would only add latency and
a second thing to be wrong.

Both lines go into **one request and therefore one descriptor** — the kernel
queues edges from both and stamps each with the line it came from, so the
reactor watches a single thing. Both are requested with `EDGE_BOTH` even
though only the rising edge raises an event: the release is what makes a hold
*measurable*, and `input: motion released after 2.31s` at debug level is how
you confirm an AM312 rather than infer it. A press with no release means the
line is inverted.

### What the LED does

One colour and no PWM, so six states have to be told apart by rate and shape
alone. The table is [`io/led_patterns.h`](src/io/led_patterns.h), kept free of
libgpiod precisely so the tests can check it on a machine with no GPIO:

| Pattern | Blink |
|---|---|
| Ring | 125 ms on, 125 ms off — 4 Hz |
| Live | solid |
| Recording | 1 s on, 1 s off |
| Offline | two 120 ms flashes, then dark for 1.64 s |
| Idle, Off | dark |

Ring against Recording is the pair that costs most to confuse — someone is at
the door, against the camera is running — so a test asserts the factor of four
between them. Offline is a *shape* rather than a rate so it cannot be misread
as a slow version of either.

The core emits `SetLed` only when the pattern **changes**, so everything that
blinks is kept going by the backend: `GpioLed` owns a timer of its own, the
reactor's single one belonging to the core. It restarts at the first phase on
every change, so a ring always begins lit — a pattern that began dark would
look like a dropped press. And the destructor drives the line low before
releasing it, because a released line keeps its last level and a daemon stopped
mid-call would otherwise leave the porch lit for good.

## The layout

```
src/core/        the rules. No I/O, ever. What the tests exercise.
src/io/          one interface per thing in the world, plus today's fakes.
src/             the machinery: reactor, config, logging, daemon wiring.
tests/           83 unit tests: no hardware, no GStreamer, no GPIO, no Pi.
docs/            decisions that outlive the code that implements them.
systemd/         the unit file.
```

Two documents are worth reading before changing anything:

- [docs/protocol.md](docs/protocol.md) — the alert shape, the `(eventId, kind)`
  dedupe that makes the ring upgrade expressible, and what `viewer-requested`
  does now that there is no SDP on the socket and no second socket to collide
  with.
- [docs/hardware-notes.md](docs/hardware-notes.md) — the two constraints the
  fakes hide, which will only ever fail on the Pi.

## Design notes

**The core is pure.** `Core::handle(event, now)` returns a list of actions and
touches nothing else. The clock and the id source are injected, which is why
the tests run in milliseconds without hardware.

**One event at a time.** Events are queued and drained, never handled where
they arise, so a backend reporting a result from inside an action it is still
carrying out cannot re-enter the core mid-decision.

**Everything is a file descriptor.** epoll, with a timerfd for deadlines and a
signalfd for SIGINT/SIGTERM. The recorder child joins as a pidfd at step 4.
Single-threaded throughout, so there is not a mutex in the codebase.

**Deadlines are steady.** A wall-clock correction must never shorten a cooldown
or fire a retry early. The one wall-clock value in the system is an alert's
timestamp, derived at send time.

## What step 4 will encode with

Probed on the Pi, GStreamer 1.26.2, all present:

| Element | From | Rank | For |
|---|---|---|---|
| `avenc_aac` | gst-libav | none (0) | the audio encoder we use |
| `voaacenc` | gst-plugins-bad | secondary (128) | fallback; takes S16LE only |
| `mp4mux` | gst-plugins-good | primary (256) | the muxer |

`avenc_aac` takes a wider input range (`channels: [1,16]`) and encodes speech
better at low bitrates, so it stays the default in the config.

Its **rank is none**, which means autoplugging will never choose it on its own.
That is harmless while the pipeline names every element explicitly, and a trap
the day anything here reaches for `encodebin` or `decodebin`.

WSL2's GStreamer is still unprobed, so the real recorder may turn out to be
testable only on the Pi.
