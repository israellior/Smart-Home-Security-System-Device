# porchlightd

The device-side control daemon for Porchlight. It decides what the doorbell
does — chime, alert, record, call — and carries none of the media itself.

The live call is a **separate program**, `webrtc-video.py`, with its own
WebSocket and its own pipeline. This daemon only asks it to start and stop.
Neither that script nor `server.js` is modified by this project.

## Where we are

Each step adds exactly one thing that can break.

| Step | What | Status |
|---|---|---|
| 0 | git init, first commit | **done** |
| 1 | Schema doc, skeleton: CMake, loop, logging, config, signals | **done** |
| 2 | The core and its unit tests | **done — 48 passing** |
| 3 | Fake backends, so the whole flow runs from the keyboard | **done** |
| 4 | Recorder and spool: real GStreamer, real MP4 | next |
| 5 | Server link and uploader | |
| — | LED, button, PIR, camera | waiting on parts |

Out of scope for now: libgpiod, the kernel driver, the real server protocol,
changes to the media script, pre-roll recording, multi-viewer relay, first-boot
provisioning.

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

Worth trying: type `motion`, then `button` two seconds later, and watch one
event get upgraded to a ring — same id, second alert, no second clip.

## When the hardware arrives

Everything sits behind an interface, and one file chooses the implementation:
[`src/backends.cpp`](src/backends.cpp). Adding a real backend is a branch there
and a line of JSON. **No core change, no test change**, and the fake stays
available to fall back to when something misbehaves at 11pm.

| Part | Config change | New files |
|---|---|---|
| LED | `backends.led: "gpio"` | `io/gpio_led.*` |
| Button + PIR | `backends.input: "gpio"` | `io/gpio_input.*`, `io/debounce.h` |
| Camera | `recorder.video_source: "libcamera"` | none — step 4 already branches |
| Real recorder | `backends.recorder: "gstreamer"` | step 4 |

An unknown backend name is a startup error naming the key, never a silent
default.

libgpiod backends will be behind a CMake option (`PORCHLIGHT_GPIO`), off on
WSL2, so the development machine never needs a dependency it cannot use.

Before wiring anything: check the PIR's output voltage is 3.3 V, and run
`gpioinfo` to get the real line numbers — do not trust the defaults in the
example config.

## The layout

```
src/core/        the rules. No I/O, ever. What the tests exercise.
src/io/          one interface per thing in the world, plus today's fakes.
src/             the machinery: reactor, config, logging, daemon wiring.
tests/           48 unit tests, no hardware and no GStreamer needed.
docs/            decisions that outlive the code that implements them.
systemd/         the unit file.
```

Two documents are worth reading before changing anything:

- [docs/protocol.md](docs/protocol.md) — the alert shape, the `(eventId, kind)`
  dedupe that makes the ring upgrade expressible, and why this daemon must
  never sign in to the signaling server as `role: 'pi'`.
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

## Open

- No git remote yet.
- The AAC encoder element is unverified on both the Pi and WSL2. Step 4 needs
  `gst-inspect-1.0 avenc_aac` (and `voaacenc`, `faac`, `fdkaacenc`) run on the
  Pi before its pipeline can be written against anything but a guess.
