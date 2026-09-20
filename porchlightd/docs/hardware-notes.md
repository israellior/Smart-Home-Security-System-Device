# Hardware notes

Two constraints that the fake backends hide completely. Everything passes on
WSL2 and the same code fails on the Pi, so they are written down here rather
than discovered twice.

Both come from the same fact: **a device node opens once.** An ALSA `hw:` device
admits one process, and so does a V4L2 camera. The daemon shares both with
`webrtc-video.py`, which is a separate process it does not control.

## The chime can fail while a call holds the speaker

The WM8960 HAT carries the microphones *and* the speaker on one card, and
`webrtc-video.py` opens it as `hw:CARD=wm8960soundcard` for the duration of a
call ([webrtc-video.py:188](../../pi/webrtc-video.py#L188)). While that call is
live, a second process cannot open the card for playback.

The rules require the chime to play on every button press, including during a
call. On the Pi, that specific case will return `EBUSY` and no sound will come
out.

**Decision: accept it, log it, never let it be fatal.** The chime executor is
best-effort. A busy device is a warning line, not an error, and not a missed
alert — the alert goes out over a different path entirely.

The real fix needs `webrtc-video.py` to stop using `hw:` and both processes to
share the card through an ALSA `dmix`/`dsnoop` plugin. That means editing the
media script, which is out of scope. Revisit it when the local socket to that
script is built, since both changes touch the same file.

What saves the rest of the audio path is that the media script builds its
pipeline per session and tears it down on close
([webrtc-video.py:583-597](../../pi/webrtc-video.py#L583-L597)). The card is
free whenever no call is live, so the recorder's `alsasrc` is fine — as long as
nothing tries to record and call at the same time, which is the next note.

## Recording during a call is impossible with the real camera

Today the recorder uses `videotestsrc`, which has no device behind it. Recording
while a call is live works on the bench and will keep working right up until
Step 5 swaps in `libcamerasrc` and `/dev/video0` — which, like the sound card,
opens once.

So these two rules are not preferences about what a user should see:

- *Motion or button during a live call: send the alert, skip the clip.*
- *A viewer arriving during a recording: stop the recording, then start the call.*

They are the only reachable behaviour once there is a camera. The live viewer
wins because a person is waiting on it.

**Decision: treat both as hardware facts, not policy.** Any future proposal to
relax either one is a question about device access, and the answer is no until
something upstream can share the camera.

This is also why the recording machine needs its `stopping` state and why
`StartCall` waits for `RecordingFinished` rather than being emitted alongside
`StopRecording`. EOS has to flush through `mp4mux` before the recorder's process
exits and releases the devices. Emitting both actions at once would hand the
camera to the call pipeline while the recorder still held it — on WSL2 that
races harmlessly and on the Pi it fails.
