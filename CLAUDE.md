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
| 4 | Echo cancellation (`webrtcdsp`) | **next, and now unavoidable** |
| 5 | Swap `videotestsrc` for `libcamerasrc` | needs a camera + `gstreamer1.0-libcamera` |

Then Phase 2: print the SDP, hand-write RTP packetisation, hand-write a jitter
buffer, drive V4L2 directly (`/dev/video0`, then the encoder at `/dev/video11`).

**Step 1 result (2026-09-18):** ICE reached `completed`, connection `connected`,
video decoding in the browser with the Pi's clock overlay visible. Measured delay
**≈ 150 ms** with the software encoder at 640×480/30. Host-to-host candidates, so
the media goes straight between the two machines and never through `server.js`.
The hardware encoder (`--encoder v4l2`) has not been tried yet.

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

- three `offer: m=...` lines, the last two flagged `port 0 + a=bundle-only: normal`
- `2 transceiver(s) sending, 1 receiving`
- all three `answer:` lines with a real port, the third `[sendonly; 98 OPUS/48000/2]`
- `connection connected`, `ICE completed`
- `talking back through …`, then `talkback:` lines with a real dBFS figure
- in the browser, one `route` row for all three tracks (one candidate pair =
  bundling working) and `your mic out` counting bytes

## The machines

**The Pi** — `192.168.0.129`, user `israellior1`. SSH is open but **password only**;
no key is installed, so Claude cannot run commands there. To change that:
`ssh-keygen -t ed25519` then append the public key to `~/.ssh/authorized_keys` on the Pi.

- Raspberry Pi 4 Model B Rev 1.5, kernel 6.18.50+rpt-rpi-v8 (aarch64), Debian 13 trixie
- GStreamer 1.26.2. Verified present: `webrtcbin`, `opusenc`/`opusdec`,
  `rtpopuspay`/`rtpopusdepay`, `alsasrc`/`alsasink`, `audioconvert`, `audioresample`,
  `webrtcdsp`, `videotestsrc`, `textoverlay`, `rtph264pay`, `v4l2h264enc`, `x264enc`
- Python bindings `Gst`, `GstWebRTC`, `GstSdp` all import
- **`v4l2h264enc` works — this is a Pi 4, which has a hardware H.264 encoder.**
  `/dev/video11` is the encoder, `/dev/video10` the decoder. (Do not repeat the
  earlier mistake of assuming a Pi 5; a Pi 5 has no H.264 encode block, a Pi 4 does.)
- Not installed: `libcamerasrc` (`gstreamer1.0-libcamera`), only needed at Step 5
- Audio out available besides the HAT: `card 3` = the Pi's own headphone jack,
  plus two HDMI outputs. **`card 3` produced no sound when tested 2026-09-18**:
  `speaker-test -D hw:CARD=Headphones` opened it, negotiated 48 kHz stereo, ran
  its full period and reported `PCM … 78% … [on]` — and nothing was audible. So
  ALSA is fine and the fault is past it; most likely nothing is plugged into the
  Pi's own jack, the HAT having its own. Do not use `card 3` as a known-good
  output to test against until that is settled.

**The server host** — Windows 11, `192.168.0.219`, Node 24.11.1, npm 11.6.2.
Wi-Fi is classified `Public`; the Node firewall rules do cover that profile.
Dependencies are only `express` and `ws`.

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
                        163 lines, and carries no media at all.
public/webrtc.html      Steps 1-3: RTCPeerConnection, video + Opus both ways, a
                        level meter per direction, mute, live stats with A/V skew.
                        Warns when it is not a secure context.
pi/webrtc-video.py      Steps 1-3: videotestsrc -> H.264 and a mic -> Opus out, the
                        browser's mic -> alsasink back, all on one webrtcbin, plus
                        signaling. The name is stale; it is the whole Pi client now.
pi/server-bridge.py     porchlightd's WebSocket to the app server, as a child
                        process: C++ has no WebSocket, python3-websocket is here.
pi/upload-clip.py       One clip, in three steps - signed url, PUT, confirm. Judged
                        by its exit code alone; only the confirm earns a 0.
pi/check-audio.sh       Read-only hardware audit: card, driver conflicts, mixer, a real
                        recording with levels, GStreamer elements, Python bindings.
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

`server.js` serves `pi/*` scripts by name (`/check-audio.sh`, `/fix-wm8960.sh`,
`/webrtc-video.py`, `/server-bridge.py`, `/upload-clip.py`) so the Pi can
`curl -fO` them. Add new Pi scripts to that list.

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
curl -fO http://192.168.0.219:3000/fix-wm8960.sh && chmod +x fix-wm8960.sh && ./fix-wm8960.sh
curl -fO http://192.168.0.219:3000/webrtc-video.py && chmod +x webrtc-video.py
./webrtc-video.py 192.168.0.219                    # video + mic in + speaker out
./webrtc-video.py 192.168.0.219 --no-talkback      # Step 2 again, one way only
./webrtc-video.py 192.168.0.219 --audio test       # a tick, while the HAT is down
./webrtc-video.py 192.168.0.219 --audio none       # Step 1 again, video only
./webrtc-video.py 192.168.0.219 --encoder v4l2     # the Pi 4's hardware encoder
./webrtc-video.py 192.168.0.219 --print-sdp        # dump the offer and answer
./webrtc-video.py 192.168.0.219 --pattern smpte    # colour bars instead of the ball
./webrtc-video.py 192.168.0.219 --speaker-device hw:CARD=Headphones
```

`--mic-device` and `--speaker-device` both default to `hw:CARD=wm8960soundcard`.
Audio needs `gstreamer1.0-alsa` as well as the plugin sets Step 1 wanted.

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
