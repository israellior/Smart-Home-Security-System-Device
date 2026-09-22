# CLAUDE.md

Working notes for this project. Read this first; update it at the end of any session
that changes something here. Last updated 2026-09-22.

**Also read [DESIGN.md](DESIGN.md).** This file describes the present. That one
describes how the finished system is meant to work — remote access, auth tokens,
an SFU, 1–5 viewers per Pi — and those decisions are settled. The five-step plan
below still stands; what changes is what happens after it.

## What this is

A learning project, not production software. The goal is to replace a working but
crude PCM-over-HTTP intercom with real WebRTC, and then — once it works — to open
the boxes and rewrite the interesting layers (RTP, the jitter buffer, V4L2 capture,
signaling) by hand.

Browser and Pi exchange SDP and ICE over a WebSocket, then send RTP directly to
each other. The server carries no media.

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

Following a five-step plan. Each step adds exactly one thing that can break.

| Step | What | Status |
|---|---|---|
| 0 | Read the existing project | **done** |
| — | Install GStreamer on the Pi, verify hardware | **done** |
| 1 | Video only, `videotestsrc`, Pi → browser | **done, working end to end** |
| 2 | Add the Pi's microphone (Opus) | **done** |
| 3 | Add browser mic → Pi speaker | **done — it is a call** |
| 4 | Echo cancellation (`webrtcdsp`) | **next, and the only step left** |
| 5 | Swap `videotestsrc` for `libcamerasrc` | **done 2026-09-22 — a real camera** |

Then Phase 2: print the SDP, hand-write RTP packetisation, hand-write a jitter
buffer, drive V4L2 directly (`/dev/video0`, then the encoder at `/dev/video11`).

**Step 1 result (2026-09-18):** ICE reached `completed`, connection `connected`,
video decoding in the browser with the Pi's clock overlay visible. Measured delay
**≈ 150 ms** with the software encoder at 640×480/30. Host-to-host candidates, so
the media goes straight between the two machines and never through `server.js`.
The hardware encoder (`--encoder v4l2`) has still not been made to work: it
stalls outright with `libcamerasrc` (see Step 5).

**Step 2 (2026-09-18):** a second track was added to the *same* `webrtcbin`, not a
second connection — one transport, one pipeline clock, one set of RTCP sender
reports, which is what will hold sound and picture together at Step 5. Audio path
is `alsasrc → audioconvert → audioresample → 48 kHz mono → opusenc → rtpopuspay
→ webrtc.`, payload type **97** (video already owns 96).

`--audio` chooses the source and defaults to **`alsa`**, the real microphone —
that is the point of the step. `--audio test` substitutes a once-a-second tick from
`audiotestsrc`, which proves everything downstream of the card (negotiation, Opus,
the second track) while the HAT is down. `--audio none` reproduces Step 1 exactly,
for bisecting. If the mic will not open, the pipeline error names `fix-wm8960.sh`.

**Confirmed working 2026-09-18** against the real WM8960.

**Step 3 result (2026-09-18):** **working — both directions at once.** A third
m-line, pointing the other way.

    browser mic -> Opus -> (pt 98) -> webrtcbin -> rtpopusdepay -> opusdec -> alsasink

It is built unlike the other two on purpose, and the difference is the lesson of
this step. The send branches exist in the `parse_launch` string, and each sink pad
they request creates a transceiver. The return path has no branch, because nothing
on the Pi produces it: it is asked for with `add-transceiver(RECVONLY, caps)`
before the pipeline leaves NULL — that is what puts it in the offer — and
**webrtcbin only grows a src pad for it once the browser answers and starts
sending**. So the speaker chain is assembled in `on_incoming_stream`, added to a
pipeline that is already PLAYING, and needs `sync_state_with_parent()` or it sits
in NULL and stays silent with no error.

Payload type **98**: 96 and 97 are taken, and under BUNDLE every payload type on
the one shared transport must be distinct even across different m-lines.

**The browser cannot use `addTrack` here.** `addTrack` picks the first audio
transceiver whose sender has no track — which is the one carrying the Pi's
microphone *towards* us, not the one waiting for ours. Attaching there would make
m-line 1 `sendrecv` (which the Pi ignores, being SENDONLY) and leave m-line 2
`inactive`, so nothing reaches the speaker and nothing says why. `webrtc.html`
instead parses the offer, finds the audio m-line marked `a=recvonly`, matches it
by `mid`, then `replaceTrack` + `direction = 'sendonly'` — all before
`createAnswer`, since an answer is a snapshot of the transceivers at that moment.

**First run, 2026-09-18 — the third m-line was refused.** Two things learned:

**Port 0 in an offer is not a rejection.** Under `max-bundle` every m-line after
the first is offered with port 0 and `a=bundle-only`: the track is real, it just
may not be used on a transport of its own. Port 0 only means refusal in an
*answer*. The first version of `show_media_lines` did not know the difference and
reported the Pi's own healthy offer as rejected twice over. It now prints the
`a=bundle-only` flag, the direction and the rtpmap, and only cries rejection when
it is one.

**Chrome will receive mono Opus but will not send it.** The answer came back as
`m=audio 0 UDP/TLS/RTP/SAVPF 0` — port 0 *and* payload type 0, which is Chrome's
signature for "no usable codec here", not "don't want it". The cause was the caps
handed to `add-transceiver`: with no `encoding-params`, webrtcbin writes
`a=rtpmap:98 OPUS/48000`, and SDP reads a missing channel count as **1**. Chrome's
Opus encoder only offers `opus/48000/2`, so nothing matched and it refused the
m-line. The send direction got away with mono because Chrome only had to *decode*
it. `RETURN_CAPS` now carries `encoding-params=(string)2`.

That was indeed the whole problem: with `encoding-params=2` the answer came back
`m=audio 9 … 98  [sendonly; 98 OPUS/48000/2]`, `connection connected`, `ICE
completed`, and video and the Pi's microphone returned with it. The refused m-line
had been stalling the entire BUNDLE group.

**The silence that followed was not WebRTC at all.** The return path was working
the whole time; it was being played into `hw:CARD=Headphones`, the Pi's own 3.5 mm
jack, with nothing plugged into it. The HAT has its own jack and is a different
card. Playing to the default `hw:CARD=wm8960soundcard` instead, **Step 3 works
completely — both directions at once.**

The lesson worth keeping: **`pad-added` proves nothing about traffic.** webrtcbin
takes the SSRC from the SDP, so the pad, the linked bin and `talking back
through …` all appear before a single RTP packet does. `Session` now counts
buffers on that pad and runs a `level` element after the decoder, printing one
line every two seconds, which separates the three causes that look identical at
a silent speaker:

- `talkback: nothing arriving` → the browser is not sending
- packets but `dBFS - that is silence` → arriving and decoding, but empty
- packets and a real level → it is the output, not WebRTC

**Step 5 result (2026-09-22): working — a real camera, both directions, one
connection.** A Camera Module 3 Wide at 640×360/30 into the same `webrtcbin` as
the microphone and the speaker. The only thing that changed downstream of the
caps was nothing at all: `videotestsrc` came out and `libcamerasrc` went in, and
the encoder, payloader, transceivers and signalling were untouched.

Two faults stood between those two facts, and both are written up below: the
**clock** one, which is the important lesson of this step, and the ribbon, which
was not a software problem at all — see "The camera" under The machines.

`--video` chooses the source and defaults to **`camera`**, for the same reason
`--audio` defaults to `alsa`. `--video test` is `videotestsrc` again, unchanged,
and is the bisect tool for "is it the camera or is it everything else".

Three decisions worth keeping:

**`libcamerasrc`, not `v4l2src`.** `/dev/video0` is the sensor, and what comes out
of it is raw Bayer — the Pi's ISP is a *separate* device that turns that into a
picture. libcamera drives both halves as one unit. (Phase 2 drives them by hand;
that is the whole point of `/dev/video0` then `/dev/video11`.)

**The imx708 is 16:9 (4608×2592), so the camera default is 640×360, not 640×480.**
Asking a 16:9 sensor for 4:3 makes the ISP crop the sides off to match — which on
the **wide** lens throws away exactly the field of view the wide lens was bought
for. `--size 1280x720` is there, and wants `--encoder v4l2`; the script says so
itself when the software encoder is asked for more than 640×480.

**No pixel format is pinned on the camera caps.** The encoder branch already asks
for `I420`, and that preference negotiates back up through `videoconvert`, so
libcamera hands over I420 directly and `videoconvert` becomes a passthrough.
Pinning it at the source would turn a free conversion into a hard failure on any
size the ISP will not produce I420 at. `check-camera.sh` prints the format that
was actually negotiated, which is how to tell whether that worked.

A `queue max-size-buffers=2 leaky=downstream` sits between the camera and the
overlay. `leaky=downstream` drops the **oldest** buffer, not the newest, so an
encoder that cannot keep up costs dropped frames instead of a picture that falls
further behind the sound every second. The symptom of an overloaded encoder is
therefore a jerky picture, not a growing delay.

**The one that cost a whole session: `alsasrc` provides a clock, `libcamerasrc`
does not.** With both in one pipeline GStreamer makes the *sound card's* clock the
pipeline clock, while `libcamerasrc` goes on timestamping from the system
monotonic clock regardless. Every video buffer then reaches `webrtcbin` with a
running time computed by subtracting a base time in one clock's units from a
timestamp in another's, and **the video branch stalls within a second** — the
camera keeps delivering 30 fps and nothing leaves the payloader — while audio,
stamped against its own clock, runs perfectly and hides the cause. `Session` now
calls `pipeline.use_clock(Gst.SystemClock.obtain())`.

`videotestsrc` paces itself off whichever clock the pipeline chose, so Steps 1–3
were immune by accident and this only appeared at Step 5.

What made it hard to find is that every cheap test misses it:

| Test | Result | Why it proves nothing |
|---|---|---|
| `libcamerasrc ! … ! x264enc ! fakesink sync=false` | 30 fps | no `alsasrc`, so no clock conflict |
| the same with `sync=true` | 30 fps | still no `alsasrc` |
| `--video test` into the real pipeline | works | `videotestsrc` follows the pipeline clock |
| `--audio none` with the camera | works | removes the other clock |
| camera + `--audio alsa` | **stalls** | the only combination that has both |

**So the bisect that finds it is `--audio none`, not any amount of `gst-launch`.**
Two plausible theories were eliminated first and neither was right: the pinned
`I420` making `videoconvert` a passthrough (tested — the branch runs fine either
way), and `libcamerasrc`'s timestamps being unusable (tested with `sync=true` —
they are fine, against the *right* clock).

**`v4l2h264enc` does not work with `libcamerasrc`** — a plain
`libcamerasrc ! videoconvert ! v4l2h264enc ! fakesink` stalls outright, where
`x264enc` runs at 30 fps. So `--encoder v4l2` is not currently an option with the
camera, and any move to 720p needs that understood first. Not yet investigated.

Autofocus is set in Python, not in the pipeline string: libcamera exposes each
sensor control as a GObject property, and which ones exist depends on the sensor
and the libcamera version, so a missing one in `parse_launch` would take the whole
pipeline down. `set_focus` reads `list_properties()` first and prints a note
instead. `--focus` takes `continuous` (the default), `default` (leave libcamera
alone), or a distance in metres — **a doorbell does not move, and continuous AF
visibly hunts on a static scene**, so a fixed `--focus 1.5` is probably what this
wants in the end.

To run headphones-only on the HAT and keep the mics from hearing the speaker
(worth it until Step 4 exists — the WM8960 drives the two amps separately):

```bash
amixer -c 2 sset "Speaker" 0%     # restore with 82%
```

Also seen, and not yet explained: the offer says `[sendrecv]` on m-lines 0 and 1
even though `transceiver.direction = SENDONLY` is set on both before the pipeline
leaves NULL. Harmless — the browser answers `recvonly` either way — but it means
that assignment is not taking effect on webrtcbin 1.26.

`pipeline warning: Can't record audio fast enough` appears once at startup. That is
`alsasrc` overrunning — the capture side, and unrelated to anything above. It may
mean `buffer-time=40000` is tighter than this Pi likes; raise it if the Pi→browser
audio ever breaks up.

A healthy run looks like this, and is the quickest way to tell what broke:

- `camera: sensor mode 2304x1296` and `camera: continuous autofocus`
- libcamera's own `Selected sensor format: 2304x1296-SBGGR10_1X10/RAW` and
  `configuring streams: (0) 640x360-YUV420` — the second confirms the I420 pin took
- three `offer: m=...` lines, the last two flagged `port 0 + a=bundle-only: normal`
- `2 transceiver(s) sending, 1 receiving`
- all three `answer:` lines with a real port, the third `[sendonly; 98 OPUS/48000/2]`
- `connection connected`, `ICE completed`
- `video:` lines reading `30 fps from the source, ~100 RTP packets/2s`, and
  **staying** that way — this is the line that catches the clock fault
- `talking back through …`, then `talkback:` lines with a real dBFS figure
- in the browser, one `route` row for all three tracks (one candidate pair =
  bundling working) and `your mic out` counting bytes

Both directions are counted at the pad, not inferred, and for the same reason:
negotiation, a pad appearing and a bitrate figure in the browser can all look
healthy while nothing moves. `video: 30 fps from the source, but no RTP leaving`
is what turned "the picture froze" into a located fault.

## The machines

**The Pi** — `192.168.0.129`, user `israellior1`. SSH is open but **password only**;
no key is installed, so Claude cannot run commands there. To change that:
`ssh-keygen -t ed25519` then append the public key to `~/.ssh/authorized_keys` on the Pi.

- Raspberry Pi 4 Model B Rev 1.5, kernel 6.18.50+rpt-rpi-v8 (aarch64), Debian 13 trixie
- GStreamer 1.26.2. Verified present: `webrtcbin`, `opusenc`/`opusdec`,
  `rtpopuspay`/`rtpopusdepay`, `alsasrc`/`alsasink`, `audioconvert`, `audioresample`,
  `webrtcdsp`, `videotestsrc`, `textoverlay`, `rtph264pay`, `v4l2h264enc`, `x264enc`
- Python bindings `Gst`, `GstWebRTC`, `GstSdp` all import
- **`v4l2h264enc` exists — this is a Pi 4, which has a hardware H.264 encoder.**
  `/dev/video11` is the encoder, `/dev/video10` the decoder. (Do not repeat the
  earlier mistake of assuming a Pi 5; a Pi 5 has no H.264 encode block, a Pi 4 does.)
  It **stalls when fed by `libcamerasrc`**, though, so `--encoder v4l2` is not
  usable with the camera — see Step 5. `x264enc` carries 640×360/30 easily.
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
curl -fO http://192.168.0.219:3000/fix-wm8960.sh && chmod +x fix-wm8960.sh
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

**3. A secure context is needed from Step 3 on — but not necessarily HTTPS.**
`getUserMedia` refuses outside one, so plain `http://192.168.0.219:3000` can receive
media but never send it. Steps 1–2 were unaffected.

**`http://localhost` is already a secure context**, with no certificate anywhere.
The browser has been running on the server host all along, so Step 3 needs nothing
but a different URL:

```
http://localhost:3000/webrtc.html          works, sends the microphone
http://192.168.0.219:3000/webrtc.html      receives only, silently
```

`webrtc.html` checks `window.isSecureContext` on load and says so in a banner
rather than letting the permission call fail with a bare error, and it still runs
receive-only so the difference is visible rather than fatal.

HTTPS is only needed to call in from **another** machine — a phone, a laptop. Not
needed yet; decide when something other than the server host wants to talk.

**4. Restart the server after pulling changes.** A long-running `node server.js`
keeps serving the old code, including the old `/ws` handler with no signaling.

**The Pi's `curl` fails three different ways, and the symptom tells them apart.**
Reading one as another costs an hour:

| What you see | What it is |
|---|---|
| fast `404` | the server is running old code, or the script is not in `server.js`'s list |
| fast connection refused | the server is not running — `npm start` |
| **hangs, 0 bytes, no error** | nothing answers the SYN: the Windows firewall, see "The server host" |

A hang is never a stale route. Check `Get-NetTCPConnection -LocalPort 3000
-State Listen` on the server host first: if something *is* listening and the Pi
still hangs, it is the firewall profile every time.

**5. The Windows clock is wrong, which corrupts the on-screen delay measurement.**
Measured 2026-09-18: **1.76 s fast**, never synced (`Source: Local CMOS Clock`,
`Last Successful Sync Time: unspecified`). The browser clock therefore reads ~1.76 s
higher than the Pi's, and the apparent glass-to-glass delay is inflated by exactly
that much. Subtract it, or fix the clock from an **administrator** PowerShell:

```powershell
w32tm /resync                            # if the service is already configured
net start w32time; w32tm /resync         # if it is not running
w32tm /stripchart /computer:time.windows.com /samples:3 /dataonly   # re-measure
```

A negative stripchart offset means the local clock is **ahead** of true time.

## The codebase

```
CLAUDE.md               This file: what exists, and how to work on it.
DESIGN.md               How the finished system is meant to work. Settled decisions.
docs/server-brief.md    What the app server has to provide, written for whoever
                        builds it. Alerts, the three clip steps, LiveKit tokens.
                        Where this repo and that document disagree, that one wins.
porchlightd/            The C++20 doorbell daemon, with its own README and its own
                        step table. Decides when to alert, record, chime and call.
server.js               Express + ws. Static files and the signaling switchboard.
                        164 lines, and carries no media at all.
public/webrtc.html      Steps 1-3: RTCPeerConnection, video + Opus both ways, a
                        level meter per direction, mute, live stats with A/V skew.
                        Warns when it is not a secure context.
pi/webrtc-video.py      Steps 1-3 and 5: a camera -> H.264 and a mic -> Opus out, the
                        browser's mic -> alsasink back, all on one webrtcbin, plus
                        signaling. The name is stale; it is the whole Pi client now.
pi/server-bridge.py     porchlightd's WebSocket to the app server, as a child
                        process: C++ has no WebSocket, python3-websocket is here.
pi/upload-clip.py       One clip, in three steps - signed url, PUT, confirm. Judged
                        by its exit code alone; only the confirm earns a 0.
pi/check-audio.sh       Read-only hardware audit: card, driver conflicts, mixer, a real
                        recording with levels, GStreamer elements, Python bindings.
pi/check-camera.sh      The same for the camera: overlay, sensor driver, what
                        libcamera sees, a real still, and timed GStreamer capture
                        through each encoder. Read-only. Run it before Step 5.
pi/fix-wm8960.sh        Surveys Waveshare's installer and undoes it reversibly.
                        --apply moves files to /var/backups/wm8960-fix, --restore
                        puts them back. Refuses to act on anything ambiguous.
tools/signal-test.js    Fake WebRTC peer, either role. Speaks signaling, no media.
                        The way to test server.js on its own - which matters more,
                        not less, once it grows rooms and auth (see DESIGN.md).
tools/clip-stub.js      The app server's clip endpoints and a bucket, faked - and
                        the ways they fail, which a real server will not do on
                        request. --fail / --fail-once / --expire-after.
```

`server.js` serves `pi/*` scripts by name (`/check-audio.sh`, `/check-camera.sh`,
`/fix-wm8960.sh`, `/webrtc-video.py`, `/server-bridge.py`, `/upload-clip.py`) so
the Pi can `curl -fO` them. Add new Pi scripts to that list.

## The signaling protocol

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
`iceServers: []` in `public/webrtc.html` is deliberate.

**Two media, one connection.** `bundle-policy=max-bundle` puts both m-lines on one
ICE transport and one UDP port pair, so there is a single candidate pair to watch
in the stats. The browser builds its own `MediaStream` and adds every incoming
track to it rather than trusting `e.streams[0]`: one element rendering one stream
is what lets it hold the two tracks together.

## Running it

```
npm start                                  # server on :3000, or PORT=3100 npm start
node tools/signal-test.js --role pi        # fake peers, to test signaling alone
node tools/signal-test.js
node tools/clip-stub.js --out ./received   # the app server's clip endpoints, faked
```

On the Pi:

```bash
curl -fO http://192.168.0.219:3000/check-audio.sh && chmod +x check-audio.sh && ./check-audio.sh
curl -fO http://192.168.0.219:3000/check-camera.sh && chmod +x check-camera.sh && ./check-camera.sh
curl -fO http://192.168.0.219:3000/fix-wm8960.sh && chmod +x fix-wm8960.sh && ./fix-wm8960.sh
curl -fO http://192.168.0.219:3000/webrtc-video.py && chmod +x webrtc-video.py
./webrtc-video.py 192.168.0.219                    # camera + mic in + speaker out
./webrtc-video.py 192.168.0.219 --video test       # videotestsrc again, to bisect
./webrtc-video.py 192.168.0.219 --size 1280x720 --encoder v4l2
./webrtc-video.py 192.168.0.219 --focus 1.5        # fix the lens, no AF hunting
./webrtc-video.py 192.168.0.219 --no-talkback      # Step 2 again, one way only
./webrtc-video.py 192.168.0.219 --audio test       # a tick, while the HAT is down
./webrtc-video.py 192.168.0.219 --audio none       # Step 1 again, video only
./webrtc-video.py 192.168.0.219 --encoder v4l2     # the Pi 4's hardware encoder
./webrtc-video.py 192.168.0.219 --print-sdp        # dump the offer and answer
./webrtc-video.py 192.168.0.219 --pattern smpte    # --video test: bars, not the ball
./webrtc-video.py 192.168.0.219 --speaker-device hw:CARD=Headphones
```

A camera goes to one process at a time, exactly like an ALSA `hw:` device — a
stray `rpicam-hello` will make `webrtc-video.py` fail to open it, and the other
way round.

`--mic-device` and `--speaker-device` both default to `hw:CARD=wm8960soundcard`.
Audio needs `gstreamer1.0-alsa` as well as the plugin sets Step 1 wanted, and the
camera needs `gstreamer1.0-libcamera`:

```bash
sudo apt install -y gstreamer1.0-libcamera rpicam-apps
```

**Until Step 4, the Pi will hear itself.** One card, speaker centimetres from the
microphones, no cancellation. Use headphones on the Pi, keep the volume down, or
`--speaker-device hw:CARD=Headphones` (`card 3`, the Pi's own jack) to prove both
directions work before any of that matters. The script prints this warning itself
when mic and speaker are the same device.

Then open **`http://localhost:3000/webrtc.html`** on the server host and press
Connect. Not the LAN address: `getUserMedia` needs a secure context and `localhost`
is one — see open problem 3.

An ALSA `hw:` device goes to one process at a time, so stop anything else using
the card before a test that touches it.

## Conventions

- **Comments say why, not what.** The existing code explains the non-obvious
  decisions (why `encoding-params=2` is not cosmetic, why `pad-added` proves
  nothing about traffic, why every audio node must stay referenced). Match that.
- **No build step, no framework, no bundler.** Plain files served statically.
- **"One at a time, and a new one replaces the old."** Used for the Pi's signaling
  socket and its WebRTC session, so a half-open connection never locks out the
  reconnect. Note that DESIGN.md changes this: 1–5 viewers per Pi is the target.
- **Scripts reach the Pi by `curl` from the server**, never by paste — an indented
  heredoc terminator silently breaks a pasted script. Files here use LF endings; if
  one ever arrives with CRLF, `sed -i 's/\r$//'`.
- Node 24, so the global `WebSocket` is available in `tools/signal-test.js`.

## Keeping this file current

At the end of a session that changes any of the above, update: the step table, the
open problems, the file map, and the protocol section if message shapes moved.
Prefer editing a line over appending; this is a description of the present, not a log.
