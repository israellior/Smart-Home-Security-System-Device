#!/usr/bin/env python3
"""The live call: the Pi publishes to LiveKit, and plays a viewer back.

There is no SDP here and no signaling socket of ours. The app server mints two
short-lived LiveKit tokens over HTTPS, this joins the room with them, and
LiveKit negotiates directly with LiveKit.

    libcamerasrc -> clock overlay -> I420 -> appsink --\\
                                                        >-- publisher token -> LiveKit
    alsasrc -> 48 kHz mono -> webrtcdsp -> appsink ----/    (canPublish, canSubscribe false)

    alsasink <- level <- webrtcechoprobe <- appsrc <----- listener token <- LiveKit
                                                           (canSubscribe, canPublish false)

Two tokens, two connections, on purpose. The publishing half carries
`canSubscribe: false` as a claim inside the token, so a leaked publisher
credential cannot be turned into a way to watch the house. Do not try to make
one token do both: LiveKit rejects on the claim, not on our good manners.

**The two halves appear in the room as two participants**, `device:<id>:pub`
and `device:<id>:sub`. The `:sub` one publishes nothing. Anything counting who
is in the room - here, or in the viewer app - has to skip identities beginning
`device:`, or the Pi looks like its own audience and the call never ends.

The GStreamer pipeline stops at the appsinks: LiveKit's SDK does the encoding.
That costs the tuned x264 chain this file used to carry, and gains a Pi that
needs no Rust toolchain - see "Route A" in the report and in CLAUDE.md. What it
does NOT cost is echo cancellation, which is the point of keeping capture and
playback in one GStreamer pipeline in one process: webrtcdsp and its probe have
to be, and here they are.

    ./webrtc-video.py --url http://192.168.0.219:4000 --device-id porch-1 \\
        --credential-file /etc/porchlight/credential

    ... --check                  fetch both tokens, print the claims, exit
    ... --video test             videotestsrc, when the camera is the suspect
    ... --audio none             no microphone at all
    ... --no-talkback            do not play viewers out of the speaker
    ... --aec off                no echo cancellation (it will howl)
    ... --speaker-device hw:CARD=Headphones

Normally porchlightd starts this, on `viewer-requested`, and stops it by
killing it. It exits by itself when the last viewer leaves.

Needs:  sudo apt install -y python3-gi python3-gst-1.0 gstreamer1.0-alsa \\
          gstreamer1.0-plugins-base gstreamer1.0-plugins-good \\
          gstreamer1.0-plugins-bad gstreamer1.0-libcamera
        and the LiveKit SDK, which is a pip package:
          python3 -m venv --system-site-packages /opt/porchlight/venv
          /opt/porchlight/venv/bin/pip install livekit
        --system-site-packages is not optional: python3-gi is an apt package
        and a sealed venv cannot see it.
"""

import argparse
import asyncio
import base64
import json
import signal
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

try:
    from livekit import rtc
except ImportError:  # --check must still work, to prove the credential
    rtc = None

# Exit codes. The daemon only reads "zero or not", but the journal reads these.
EXIT_OK = 0
EXIT_CONFIG = 1  # permanent: bad credential, missing element, no camera
EXIT_MEDIA = 2  # could not join, or could not stay joined

# The WM8960 HAT. Always by name: card numbers move between boots, so hw:2,0 is
# right until the day it silently points at the headphone jack instead.
DEFAULT_MIC = "hw:CARD=wm8960soundcard"

# The same card plays as well as records, which is the whole reason webrtcdsp is
# not optional here: what comes out of it goes straight back into the microphone
# beside it. hw:CARD=Headphones is the Pi's own 3.5 mm jack.
DEFAULT_SPEAKER = "hw:CARD=wm8960soundcard"

# Opus is 48 kHz natively and LiveKit's audio path is 48 kHz, so everything in
# this file is, and there is exactly one resample: whatever the card offers, up
# or down to this.
AUDIO_RATE = 48000
AUDIO_CHANNELS = 1

# The imx708 is 4608x2592 - 16:9. Ask it for 4:3 and the ISP crops the sides off
# to match, which on the *wide* lens throws away exactly the field of view the
# wide lens was bought for.
CAMERA_SIZE = "640x360"
TEST_SIZE = "640x480"
DEFAULT_FPS = 30

# libcamera chooses a sensor mode to satisfy the requested output size, and for
# anything at or below 1536x864 it picks the imx708's binned 1536x864 mode. That
# mode reads only the centre 3072x1728 of the 4608x2592 array, which on the wide
# lens is about 120 degrees of diagonal field of view reduced to about 98.
# 2304x1296 reads the full array and still runs at 56 fps.
FULL_FRAME_MODE = "sensor/config,width=2304,height=1296,depth=10"

# format=I420 is pinned because it is what LiveKit's VideoFrame wants and what
# the Pi's ISP produces directly, so videoconvert below becomes a passthrough.
# The queue is what stops latency growing when the SDK's encoder falls behind:
# leaky=downstream discards the *oldest* buffer, so a slow encoder costs dropped
# frames rather than a picture that drifts further behind the sound every second.
CAMERA = (
    "libcamerasrc name=camera"
    " ! video/x-raw,format=I420,width={width},height={height},framerate={fps}/1"
    " ! queue max-size-buffers=2 leaky=downstream"
)

# A moving pattern by default, so a frozen picture is obvious at a glance rather
# than looking like a still that arrived correctly.
TEST_SOURCE = (
    "videotestsrc name=camera is-live=true pattern={pattern} animation-mode=running-time"
    " ! video/x-raw,format=I420,width={width},height={height},framerate={fps}/1"
)

# The Pi's wall clock, burnt into the picture with millisecond resolution. Put
# the viewer's own clock next to the video and the difference is the delay.
#
# **Off by default, because it is a measuring instrument and not a feature.**
# The text changes every 50 ms, so textoverlay re-renders a Pango layout twenty
# times a second and alpha-blends it onto all thirty frames, on the same CPU
# that is software-encoding H.264 and running an echo canceller. That is a real
# cost to pay for a clock nobody is reading.
OVERLAY = (
    ' ! textoverlay name=clock font-desc="Mono 18" halignment=left valignment=top'
    " xpad=12 ypad=12 shaded-background=true"
)

# videoconvert stays even though the caps above already say I420: it costs
# nothing when the formats match, and it is what lets the pipeline still
# negotiate on a libcamera that will not give I420 at some future size.
VIDEO_SINK = (
    " ! videoconvert"
    " ! video/x-raw,format=I420"
    " ! appsink name=vsink emit-signals=true max-buffers=2 drop=true sync=false"
)

AUDIO_SOURCES = {
    # The device name is quoted because it contains an '=' of its own
    # (hw:CARD=...), which the pipeline parser would otherwise have to guess at.
    #
    # alsasrc's default buffer-time is 200 ms, which is 200 ms of delay bought
    # for nothing. 40 ms of ring buffer read in 10 ms slices is still far more
    # headroom than one 10 ms frame needs.
    "alsa": 'alsasrc name=mic device="{device}" buffer-time=40000 latency-time=10000',
    # wave=ticks is one short beep a second. A continuous tone proves just as
    # much and is unbearable within a minute; a tick also makes a dropout
    # audible as a missed beat, which a steady tone hides completely.
    "test": "audiotestsrc name=mic is-live=true wave=ticks freq=880",
}

# webrtcdsp works in S16LE and wants its buffers at the rate its probe sees, so
# the caps are pinned on both sides of the canceller rather than left open. The
# source caps are still open: whatever rate the card offers, audioresample
# brings it here, and pinning the card would turn a resample into a failure.
AUDIO_CAPS = f"audio/x-raw,format=S16LE,rate={AUDIO_RATE},channels={AUDIO_CHANNELS},layout=interleaved"

CAPTURE_AUDIO = (
    " ! queue max-size-time=100000000 leaky=downstream"
    " ! audioconvert ! audioresample"
    f" ! {AUDIO_CAPS}"
    "{dsp}"
    " ! appsink name=asink emit-signals=true max-buffers=8 drop=true sync=false"
)

# The way back. There is no jitter buffer here because LiveKit already ran one:
# what comes out of an AudioStream has been decoded and paced by NetEq, and all
# this has to do is hand it to the card.
#
# do-timestamp=true stamps each buffer against the pipeline clock as it is
# pushed, which is what keeps a push-to-talk gap from being read as a timestamp
# discontinuity. is-live=true is what stops alsasink trying to catch up after
# one.
PLAYBACK = (
    # The caps are quoted because they contain commas of their own, which the
    # pipeline parser would otherwise read as more appsrc properties.
    "appsrc name=aplay is-live=true format=time do-timestamp=true"
    f' caps="{AUDIO_CAPS}"'
    " ! queue max-size-time=200000000"
    " ! audioconvert ! audioresample"
    "{probe}"
    " ! level name=talklevel interval=1000000000 post-messages=true"
    # async=false is what makes this pipeline startable at all. A sink normally
    # refuses to finish going to PLAYING until it has prerolled on a first
    # buffer - and the first buffer here arrives only when a viewer holds the
    # talk button, which may be never. Without this the speaker sits in PAUSED
    # indefinitely, posting no error because nothing has failed.
    #
    # It costs nothing: async only governs waiting for preroll. sync stays on,
    # so what does arrive is still played against the pipeline clock.
    ' ! alsasink name=speaker async=false device="{device}"'
    " buffer-time=40000 latency-time=10000"
)

# webrtcdsp finds its probe by element name, and the two have to be in one
# process. Putting both in one pipeline is stronger than it needs to be and
# removes the question entirely.
DSP = " ! webrtcdsp name=aec probe=aecprobe"
PROBE = " ! webrtcechoprobe name=aecprobe"

# Everything past echo-cancel is a judgement call about a doorbell rather than a
# requirement, and which of them this webrtcdsp has depends on its version - so
# they are set through list_properties() rather than in the launch string, where
# a missing one would take the whole pipeline down.
DSP_TUNING = {
    "echo-cancel": True,
    # The delay between what the probe sees and what the microphone hears is not
    # knowable here: it is ALSA buffering, the amplifier and the air. This makes
    # the canceller estimate it instead of trusting a number nobody measured.
    "delay-agnostic": True,
    "high-pass-filter": True,
    "noise-suppression": True,
    # A doorbell is talked to from a doorstep, at whatever distance and volume
    # the caller chooses. Levelling that is worth more than it costs.
    "gain-control": True,
}


# WebRTC's own verdict on why the encoder is not delivering what it was asked
# for. "cpu" and "bandwidth" want opposite fixes, and guessing between them is
# how an evening goes missing.
LIMITATION = {0: "none", 1: "cpu", 2: "bandwidth", 3: "other"}

# What the encoder sacrifices first when the link will not carry the stream.
# "resolution" keeps the picture size and lets the frame rate fall, which is
# the right trade for looking at a doorstep; "balanced" is WebRTC's default and
# shrinks the picture instead.
DEGRADATION = {"resolution": 2, "framerate": 1, "balanced": 0}


class TokenError(Exception):
    """The app server would not mint a token. Says whether it is worth retrying."""

    def __init__(self, message, permanent=False):
        super().__init__(message)
        self.permanent = permanent


def fetch_token(base_url, device_id, kind, credential, timeout=15):
    """POST /api/devices/<id>/media-token/<kind> with the device credential.

    No body at all - the device comes from the credential and the kind from the
    path. Tokens last about two minutes and are only needed to join, so this is
    called fresh on every connection attempt and the result is never cached: a
    token fetched, held, and used four minutes later is the one failure mode
    this shape has, and not caching is the whole fix.
    """
    url = f"{base_url.rstrip('/')}/api/devices/{device_id}/media-token/{kind}"
    request = urllib.request.Request(
        url,
        data=b"",  # POST with Content-Length: 0, not a GET
        method="POST",
        headers={"Authorization": f"Bearer {credential}"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            grant = json.loads(response.read())
    except urllib.error.HTTPError as error:
        body = error.read().decode(errors="replace")[:200]
        # 401 and 403 will fail exactly the same way in ten seconds' time, and
        # in ten minutes'. Anything else might not.
        raise TokenError(
            f"{kind} token refused: HTTP {error.code} {body}",
            permanent=error.code in (401, 403),
        )
    except urllib.error.URLError as error:
        raise TokenError(f"cannot reach {base_url}: {error.reason}")
    except ValueError:
        raise TokenError(f"{kind} token response was not JSON")

    for field in ("token", "url", "room"):
        if not grant.get(field):
            raise TokenError(f"{kind} token response has no {field!r}", permanent=True)
    return grant


def token_claims(token):
    """The JWT payload, without verifying anything - this is for printing.

    Only the issuer's own server can verify it and only LiveKit needs to. What
    is worth seeing locally is the grant and the two timestamps.
    """
    try:
        payload = token.split(".")[1]
        payload += "=" * (-len(payload) % 4)  # base64url, unpadded
        return json.loads(base64.urlsafe_b64decode(payload))
    except (IndexError, ValueError):
        return {}


def read_credential(path):
    try:
        with open(path) as handle:
            credential = handle.read().strip()
    except OSError as error:
        raise SystemExit(
            f"cannot read credential {path}: {error}\n"
            "It is written onto the card when the device is built. There is no\n"
            "enrolment call - if it is missing, the device must be re-minted."
        )
    if not credential:
        raise SystemExit(f"{path} is empty; the device must be re-minted.")
    return credential


def require(name, hint):
    if not Gst.ElementFactory.find(name):
        sys.exit(f"missing GStreamer element '{name}' - {hint}")


def parse_size(text):
    """WIDTHxHEIGHT, checked here so a typo fails before the camera is opened."""
    try:
        width, height = (int(n) for n in text.lower().split("x", 1))
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected WIDTHxHEIGHT, got {text!r}")
    if width < 64 or height < 64:
        raise argparse.ArgumentTypeError(f"{text} is too small to be a picture")
    # LiveKit reads an I420 buffer as tightly packed planes, and GStreamer pads
    # each row up to a multiple of four. They agree for any width that is a
    # multiple of eight and can disagree otherwise, which shows up as a picture
    # sheared diagonally rather than as an error.
    if width % 8 or height % 2:
        raise argparse.ArgumentTypeError(
            f"{text}: width must be a multiple of 8 and height even, or the "
            "I420 rows will not line up"
        )
    return width, height


def parse_focus(text):
    """'continuous', 'default', or a distance in metres."""
    if text in ("continuous", "default"):
        return text
    try:
        metres = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"expected 'continuous', 'default' or a distance in metres, got {text!r}"
        )
    if metres <= 0:
        raise argparse.ArgumentTypeError("a focus distance has to be greater than zero")
    return metres


def is_viewer(identity):
    """A participant that is a person, not one of our own two connections.

    The Pi joins twice - once to publish and once to listen - so both of its
    halves appear to each other as ordinary remote participants. Counting them
    as an audience means the call never decides it is over.
    """
    return not identity.startswith("device:")


def set_sensor_mode(camera, spec):
    """Force the full-frame sensor mode, so the wide lens stays wide.

    sensor-config is a recent addition to libcamerasrc and setting a property
    that is not there is fatal, so it is applied here rather than in the
    parse_launch string.
    """
    if "sensor-config" not in {p.name for p in camera.list_properties()}:
        print("camera: this libcamerasrc cannot choose the sensor mode, so the field")
        print("        of view is whatever libcamera picks for the requested size")
        return

    if hasattr(Gst.Structure, "new_from_string"):
        structure = Gst.Structure.new_from_string(spec)
    else:
        parsed = Gst.Structure.from_string(spec)
        structure = parsed[0] if isinstance(parsed, tuple) else parsed
    if structure is None:
        print(f"camera: could not parse sensor config {spec!r}, leaving it automatic")
        return

    camera.set_property("sensor-config", structure)
    print(
        f"camera: sensor mode {structure.get_value('width')}x{structure.get_value('height')}"
        " - the full array, so the wide lens keeps its field of view"
    )


def set_focus(camera, focus):
    """Point the Camera Module 3's autofocus, if this libcamera exposes it.

    libcamerasrc turns each sensor control into a GObject property, and which
    ones exist depends on the sensor and the libcamera version. Leaving focus
    alone is not a neutral choice: af-mode defaults to *manual* with
    lens-position 0, and 0 dioptres is infinity, so an unconfigured Camera
    Module 3 films past the horizon while the caller stands a metre away.
    """
    names = {spec.name for spec in camera.list_properties()}
    if "af-mode" not in names:
        print("camera: this libcamerasrc has no af-mode, leaving focus alone")
        return

    if focus == "continuous":
        Gst.util_set_object_arg(camera, "af-mode", "continuous")
        print("camera: continuous autofocus")
        return

    # A doorbell does not move, and continuous autofocus hunts on a static
    # scene. libcamera measures lens position in dioptres, which is 1/metres.
    if "lens-position" not in names:
        print("camera: no lens-position property, falling back to continuous autofocus")
        Gst.util_set_object_arg(camera, "af-mode", "continuous")
        return
    Gst.util_set_object_arg(camera, "af-mode", "manual")
    camera.set_property("lens-position", 1.0 / focus)
    print(f"camera: focus fixed at {focus:g} m (lens-position {1.0 / focus:.2f})")


def tune_dsp(dsp):
    """Set what this webrtcdsp actually has, and say what it does not.

    Same reasoning as set_focus: these differ between versions, and a property
    that is not there is fatal in a launch string.
    """
    names = {spec.name for spec in dsp.list_properties()}
    applied, missing = [], []
    for name, value in DSP_TUNING.items():
        if name in names:
            dsp.set_property(name, value)
            applied.append(name)
        else:
            missing.append(name)
    print(f"audio: echo cancellation on ({', '.join(applied)})")
    if missing:
        print(f"       this webrtcdsp has no {', '.join(missing)}; left at its defaults")


class Media:
    """The GStreamer half: **two** pipelines, and they must stay two.

    Capture (camera and microphone) and playback (a viewer, out of the speaker)
    are separate pipelines in one process. webrtcdsp finds its echo probe by
    name across the whole process, so one pipeline was never required - and one
    pipeline is actively wrong, because a GStreamer pipeline reaches PLAYING
    only when every sink in it is ready. The speaker chain starts with an appsrc
    that has nothing in it until somebody holds the talk button, so putting it
    in with the camera means **the camera waits for a viewer to speak**: both
    sources sit in PAUSED, the counters read NOTHING, and no error is ever
    posted because nothing has gone wrong as far as GStreamer is concerned.

    That is exactly what happened on the first run on real hardware. Two
    pipelines, one clock, and the speaker can stall on its own.
    """

    def __init__(self, opts, on_error):
        self.opts = opts
        self.on_error = on_error
        self.width, self.height = opts.size
        self.expected_frame = self.width * self.height * 3 // 2
        self.warned_size = False

        # Counted, never inferred. Negotiation succeeding, a track publishing
        # and a bitrate in the viewer's browser can all look healthy while
        # nothing moves, and these are the numbers that tell the difference.
        # Both ends of each direction, never one. "Nothing is arriving" and
        # "nothing is being forwarded" look identical from a single number, and
        # they point at opposite halves of the program.
        self.cam_frames = 0
        self.published_frames = 0
        self.mic_frames = 0
        self.mic_published = 0
        self.play_frames = 0
        self.talk_level = None
        self.seen = (0, 0, 0, 0, 0)

        self.video_sink = None
        self.audio_sink = None
        self.play_src = None
        self.on_video = None  # set by the Call before PLAYING
        self.on_audio = None

        self.capture = Gst.parse_launch(self._describe_capture())
        self.playback = None
        if opts.talkback:
            self.playback = Gst.parse_launch(self._describe_playback())

        # Pin the clock on both, and to the *same* clock.
        #
        # alsasrc provides a clock and libcamerasrc does not, so left alone
        # GStreamer picks the sound card's for the capture pipeline. libcamerasrc
        # goes on timestamping from the system monotonic clock regardless, so
        # every video buffer arrives with a running time worked out by
        # subtracting a base time in one clock's units from a timestamp in
        # another's, and the video branch stalls within a second while audio runs
        # perfectly and hides the cause. This cost a whole session once; see
        # CLAUDE.md.
        #
        # The two pipelines share it for a second reason: webrtcdsp has to line
        # up what the probe heard being played against what the microphone
        # heard a moment later, and it cannot do that across two time bases.
        for pipeline in self._pipelines():
            pipeline.use_clock(Gst.SystemClock.obtain())

        camera = self.capture.get_by_name("camera")
        if camera is not None and opts.video == "camera":
            if opts.sensor_mode == "full":
                set_sensor_mode(camera, FULL_FRAME_MODE)
            if opts.focus != "default":
                set_focus(camera, opts.focus)

        dsp = self.capture.get_by_name("aec")
        if dsp is not None:
            tune_dsp(dsp)

        self.video_sink = self.capture.get_by_name("vsink")
        self.video_sink.connect("new-sample", self._pull_video)
        self.audio_sink = self.capture.get_by_name("asink")
        if self.audio_sink is not None:
            self.audio_sink.connect("new-sample", self._pull_audio)
        if self.playback is not None:
            self.play_src = self.playback.get_by_name("aplay")

        self.clock = self.capture.get_by_name("clock")
        self.stopping = threading.Event()
        self.bus_threads = [
            threading.Thread(target=self._watch_bus, args=(p, name), daemon=True)
            for p, name in self._named_pipelines()
        ]

    def _pipelines(self):
        return [p for p in (self.capture, self.playback) if p is not None]

    def _named_pipelines(self):
        pairs = [(self.capture, "capture")]
        if self.playback is not None:
            pairs.append((self.playback, "playback"))
        return pairs

    def _describe_capture(self):
        opts = self.opts
        source = CAMERA if opts.video == "camera" else TEST_SOURCE
        overlay = ""
        if opts.overlay:
            if Gst.ElementFactory.find("textoverlay"):
                overlay = OVERLAY
            else:
                print("note: textoverlay is missing, so there is no clock to measure delay with")

        video = (
            source.format(
                width=self.width, height=self.height, fps=opts.fps, pattern=opts.pattern
            )
            + overlay
            + VIDEO_SINK
        )

        audio = ""
        if opts.audio != "none":
            audio = " " + AUDIO_SOURCES[opts.audio].format(device=opts.mic_device)
            audio += CAPTURE_AUDIO.format(dsp=DSP if opts.aec else "")

        description = f"{video}{audio}"
        print(f"\ncapture pipeline:\n  {description}\n")
        return description

    def _describe_playback(self):
        description = PLAYBACK.format(
            device=self.opts.speaker_device, probe=PROBE if self.opts.aec else ""
        )
        print(f"playback pipeline:\n  {description}\n")
        return description

    # --- out of the pipeline, into LiveKit --------------------------------

    def _pull_video(self, sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buffer = sample.get_buffer()
        self.cam_frames += 1

        ok, info = buffer.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        try:
            if info.size != self.expected_frame:
                # GStreamer pads I420 rows to a multiple of four and LiveKit
                # reads them tightly packed. parse_size refuses the sizes where
                # those disagree, so reaching this means something downstream
                # renegotiated - which would show as a sheared picture, not an
                # error, if it were let through.
                if not self.warned_size:
                    self.warned_size = True
                    print(
                        f"video: a frame is {info.size} bytes and I420 at "
                        f"{self.width}x{self.height} is {self.expected_frame}; "
                        "not publishing it rather than publishing it sheared"
                    )
                return Gst.FlowReturn.OK
            # bytes(), not the memoryview: the SDK hands the buffer's *address*
            # to Rust rather than copying it, and this mapping is gone the
            # moment we return. One memcpy a frame is the price of not having a
            # use-after-free in something that runs for months.
            if self.on_video is not None:
                self.on_video(bytes(info.data), buffer.pts)
                self.published_frames += 1
        finally:
            buffer.unmap(info)
        return Gst.FlowReturn.OK

    def _pull_audio(self, sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buffer = sample.get_buffer()
        # Counted here, before anything is done with it: this is the card's
        # output, and it keeps arriving whether or not there is yet a publisher
        # to hand it to. Counting it only when forwarded made the first two
        # reports of every run say "NOTHING from the card" about a microphone
        # that was working perfectly.
        self.mic_frames += 1

        ok, info = buffer.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        try:
            if self.on_audio is not None and info.size:
                self.on_audio(bytes(info.data))
                self.mic_published += 1
        finally:
            buffer.unmap(info)
        return Gst.FlowReturn.OK

    # --- into the pipeline, out of the speaker ----------------------------

    def play(self, pcm):
        """One viewer's decoded audio, straight to the card.

        Called from the asyncio thread; appsrc's push-buffer is thread-safe and
        Gst.Buffer.new_wrapped takes its own reference to the bytes.
        """
        if self.play_src is None or not pcm:
            return
        self.play_src.emit("push-buffer", Gst.Buffer.new_wrapped(pcm))
        self.play_frames += 1

    # --- running it -------------------------------------------------------

    def start(self):
        for thread in self.bus_threads:
            thread.start()

        # Playback first, and the order is not cosmetic: webrtcdsp looks its
        # echo probe up **by name, once, as it starts**, and the probe lives in
        # the other pipeline now. Start the capture side first and the canceller
        # goes looking for something that does not exist yet.
        if self.playback is not None and not self._bring_up(self.playback, "playback"):
            print("        Carrying on without it. Seeing who is at the door matters more")
            print("        than talking to them, so this costs talkback and nothing else.")
            self.playback.set_state(Gst.State.NULL)
            self.playback = None
            self.play_src = None

        # And only the capture side can fail the call: with no camera and no
        # microphone there is nothing to publish and no reason to join a room.
        return self._bring_up(self.capture, "capture")

    def _bring_up(self, pipeline, name):
        """Set it PLAYING and *check*, which is the whole point.

        `set_state` returning ASYNC is normal and says nothing; a pipeline that
        never gets past it looks exactly like one that is running and producing
        no data, and posts no error because nothing has failed. Waiting for the
        state and naming what it is stuck on turns the single most confusing
        failure this program has into one line.
        """
        change = pipeline.set_state(Gst.State.PLAYING)
        if change == Gst.StateChangeReturn.FAILURE:
            print(f"{name}: refused to start - the pipeline error above says why")
            return False
        if change == Gst.StateChangeReturn.NO_PREROLL:
            # A live pipeline says so this way. It will not preroll and does not
            # need to, so asking it to reach PLAYING below would be waiting for
            # something that is already as true as it gets.
            return True

        change, state, pending = pipeline.get_state(5 * Gst.SECOND)
        if state == Gst.State.PLAYING:
            return True

        print(f"{name}: stuck in {state.value_nick}, waiting to reach {pending.value_nick}")
        print("        Something in it cannot preroll. A sink with no data to start on")
        print("        will hold a whole pipeline there, silently and with no error.")
        if name == "playback":
            print(f"        Here that is {self.opts.speaker_device}: if another process has")
            print("        the card, nothing plays and nothing says so. --no-talkback runs")
            print("        the call one-way, which is the test that separates the two.")
        return False

    def stop(self):
        self.stopping.set()
        # Only if something was ever pushed. An appsrc that never produced a
        # buffer has no TIME segment, and ending a stream that never started
        # makes GStreamer assert its way through two criticals on the way out -
        # alarming in a log, and about nothing.
        if self.play_src is not None and self.play_frames:
            self.play_src.emit("end-of-stream")
        for pipeline in self._pipelines():
            pipeline.set_state(Gst.State.NULL)

    def tick_clock(self):
        if self.clock is not None:
            self.clock.set_property("text", datetime.now().strftime("%H:%M:%S.%f")[:-3])

    def _watch_bus(self, pipeline, _name):
        """A thread rather than a GLib main loop: asyncio owns the main thread."""
        bus = pipeline.get_bus()
        wanted = (
            Gst.MessageType.ERROR
            | Gst.MessageType.WARNING
            | Gst.MessageType.ELEMENT
            | Gst.MessageType.EOS
        )
        while not self.stopping.is_set():
            message = bus.timed_pop_filtered(200 * Gst.MSECOND, wanted)
            if message is None:
                continue
            self._on_message(message)

    def _on_message(self, message):
        if message.type == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            where = message.src.get_name() if message.src else "?"
            print(f"pipeline error from {where}: {err.message}")
            if debug:
                print(f"  {debug}")
            if where == "camera":
                print("        The camera would not open. Like an ALSA hw: device it goes to")
                print("        one process at a time, so rpicam-hello, porchlightd's recorder")
                print("        or a second copy of this will hold it. If it was never detected")
                print("        at all, the ribbon or config.txt is the cause: ./check-camera.sh")
                print("        To carry on without it meanwhile:  --video test")
            if where == "mic":
                print("        The microphone would not open. If this is 'Invalid argument' it")
                print("        is the known WM8960 fault:  ./fix-wm8960.sh")
                print("        To carry on without the HAT meanwhile:  --audio test")
            if where == "speaker":
                print("        The speaker would not open - something else holds the card.")
                print("        porchlightd's chime uses it too, and cannot while a call is up.")
                print("        Or send the voice elsewhere: --speaker-device hw:CARD=Headphones")
            self.on_error(f"pipeline failed: {err.message}")
        elif message.type == Gst.MessageType.WARNING:
            err, _debug = message.parse_warning()
            print(f"pipeline warning: {err.message}")
        elif message.type == Gst.MessageType.ELEMENT:
            structure = message.get_structure()
            if structure is not None and structure.get_name() == "level":
                try:
                    channels = structure.get_value("rms")
                    if channels:
                        self.talk_level = max(channels)
                except (TypeError, ValueError):
                    pass

    def report(self, viewers):
        cam, pub, mic, mic_pub, play = self.seen
        frames = self.cam_frames - cam
        published = self.published_frames - pub
        mic_frames = self.mic_frames - mic
        mic_published = self.mic_published - mic_pub
        played = self.play_frames - play
        self.seen = (
            self.cam_frames,
            self.published_frames,
            self.mic_frames,
            self.mic_published,
            self.play_frames,
        )

        if frames == 0:
            print("video: NOTHING - the source stopped producing frames")
        elif published == 0:
            print(f"video: {frames / 2:.0f} fps from the source, but none reaching LiveKit")
        else:
            print(f"video: {frames / 2:.0f} fps from the source, {published / 2:.0f} published/s")

        if self.opts.audio != "none":
            if mic_frames == 0:
                print("mic:   NOTHING from the card")
            elif mic_published == 0:
                print(f"mic:   {mic_frames / 2:.0f} frames/s from the card, none published yet")
            else:
                print(
                    f"mic:   {mic_frames / 2:.0f} frames/s from the card,"
                    f" {mic_published / 2:.0f} published/s"
                )

        # play_src rather than opts.talkback: the speaker chain may have been
        # dropped at startup, and reporting on it then would be reporting on
        # something that is not there.
        if self.play_src is not None:
            if played == 0:
                print("talk:  nothing arriving - nobody is holding the button")
            elif self.talk_level is not None and self.talk_level < -70:
                print(
                    f"talk:  {played / 2:.0f} frames/s, {self.talk_level:.0f} dBFS"
                    " - that is silence, not speech"
                )
            else:
                loud = f", {self.talk_level:.0f} dBFS" if self.talk_level is not None else ""
                print(f"talk:  {played / 2:.0f} frames/s{loud}")

        who = ", ".join(sorted(viewers)) if viewers else "nobody"
        print(f"room:  {len(viewers)} viewer(s): {who}")


class Call:
    """The LiveKit half: two rooms, and the rule for when the call is over."""

    def __init__(self, opts, credential, media):
        self.opts = opts
        self.credential = credential
        self.media = media
        # Held because the pipeline's threads reach in from outside asyncio, and
        # every one of those crossings has to go through call_soon_threadsafe.
        self.loop = asyncio.get_running_loop()

        self.publisher = None
        self.listener = None
        self.video_source = None
        self.audio_source = None
        self.audio_queue = asyncio.Queue(maxsize=50)

        self.viewers = set()
        self.saw_viewer = False
        self.sent_bytes = 0
        self.finished = asyncio.Event()
        self.exit_code = EXIT_OK
        self.stopping = False
        self.talk_tasks = {}

    # --- joining ----------------------------------------------------------

    async def connect(self, kind, on_ready):
        """Join with a freshly minted token, and keep re-joining if dropped.

        Every attempt mints its own token, which is the whole handling of
        expiry: a token is needed to join and for nothing afterwards, so one
        that went stale while we were disconnected is simply never used again.
        There is no cache to invalidate and no refresh to get wrong.

        deadline is set the moment things start going wrong and cleared by a
        success, so a call survives a minute of bad network and gives up on an
        app server that is simply not there.
        """
        backoff = 1.0
        deadline = None
        while not self.stopping:
            room = None
            try:
                grant = await asyncio.to_thread(
                    fetch_token, self.opts.url, self.opts.device_id, kind, self.credential
                )
                room = rtc.Room()
                dropped = asyncio.Event()
                self._wire(room, kind, dropped)
                # auto_subscribe off on the publisher: its token says
                # canSubscribe false, and asking anyway is a request LiveKit is
                # entitled to hang up on.
                await room.connect(
                    grant["url"],
                    grant["token"],
                    options=rtc.RoomOptions(auto_subscribe=(kind == "listener")),
                )
            except TokenError as error:
                print(f"{kind}: {error}")
                if error.permanent:
                    print(f"{kind}: this will not get better; the device must be re-minted")
                    self.fail(EXIT_CONFIG)
                    return
                deadline = deadline or time.monotonic() + self.opts.reconnect_timeout
            except Exception as error:  # the SDK raises its own connect errors
                print(f"{kind}: could not join: {error}")
                await self._let_go(room)
                deadline = deadline or time.monotonic() + self.opts.reconnect_timeout
            else:
                print(f"{kind}: joined {grant['room']} as {grant['identity']}")
                backoff = 1.0
                deadline = None
                if kind == "listener":
                    self.listener = room
                    # The viewer was almost certainly in the room before we
                    # were - they are the reason this process started - so the
                    # count is taken now rather than waiting for an event that
                    # already happened.
                    self._resync_viewers()
                else:
                    self.publisher = room
                await on_ready(room)

                await dropped.wait()
                await self._let_go(room)
                if self.stopping:
                    return
                print(f"{kind}: dropped; joining again with a fresh token")
                deadline = time.monotonic() + self.opts.reconnect_timeout
                continue

            if time.monotonic() > deadline:
                print(f"{kind}: giving up after {self.opts.reconnect_timeout:.0f}s of this")
                self.fail(EXIT_MEDIA)
                return
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 10.0)

    async def _let_go(self, room):
        if room is None:
            return
        if room is self.listener:
            self.listener = None
        if room is self.publisher:
            self.publisher = None
        try:
            await room.disconnect()
        except Exception:
            pass  # it is already gone; this is only tidying up after it

    def _wire(self, room, kind, dropped):
        @room.on("disconnected")
        def _(*_args):
            # The SDK reconnects by itself first and only emits this when it
            # has given up, so there is nothing subtle left to do here.
            self.loop.call_soon_threadsafe(dropped.set)

        @room.on("reconnecting")
        def _(*_args):
            print(f"{kind}: connection lost, the SDK is retrying")

        @room.on("reconnected")
        def _(*_args):
            print(f"{kind}: reconnected")

        if kind != "listener":
            return

        @room.on("participant_connected")
        def _(participant):
            self.loop.call_soon_threadsafe(self._resync_viewers)

        @room.on("participant_disconnected")
        def _(participant):
            self.loop.call_soon_threadsafe(self._resync_viewers)

        # The downlink has four steps and only the last one used to say
        # anything, so "nobody is holding the button" covered all four: the
        # viewer never got permission to publish, the viewer published and we
        # did not subscribe, we subscribed and it failed, or it worked and the
        # audio was silent. They need completely different fixes and three of
        # them are not on this device at all.
        @room.on("track_published")
        def _(publication, participant):
            if is_viewer(participant.identity):
                print(
                    f"listener: {participant.identity} published"
                    f" {publication.kind} '{publication.name}'"
                )

        @room.on("track_subscription_failed")
        def _(participant, track_sid, error):
            print(f"listener: could not subscribe to {participant.identity}: {error}")

        @room.on("track_subscribed")
        def _(track, publication, participant):
            if not is_viewer(participant.identity):
                return
            print(f"listener: subscribed to {participant.identity} {track.kind}")
            if track.kind == rtc.TrackKind.KIND_AUDIO:
                self.loop.call_soon_threadsafe(self._start_talkback, track, participant.identity)

        @room.on("track_unsubscribed")
        def _(track, publication, participant):
            self.loop.call_soon_threadsafe(self._stop_talkback, participant.identity)

    # --- publishing -------------------------------------------------------

    async def publish(self, room):
        options = rtc.TrackPublishOptions()
        options.source = rtc.TrackSource.SOURCE_CAMERA
        options.video_codec = (
            rtc.VideoCodec.H264 if self.opts.codec == "h264" else rtc.VideoCodec.VP8
        )
        options.video_encoding.max_framerate = self.opts.fps
        options.video_encoding.max_bitrate = self.opts.bitrate_kbps * 1000
        # Off by default, and the OpenH264 log is what gives it away: with it on
        # there are two or three encoder instances, each doing a software H.264
        # encode of every frame, on a Cortex-A72 that is also running the
        # camera, the microphone and an echo canceller. Simulcast buys a viewer
        # on a bad link a lower layer to fall back to - worth it for a broadcast
        # to strangers, not for 1-5 people looking at a doorstep at 640x360.
        options.simulcast = self.opts.simulcast
        # What to give up first when the link cannot carry the stream. The
        # default is "balanced", which shrinks the picture - and the first run
        # on a weak link went 640x360 -> 480x264 -> 320x180 while the viewer
        # was trying to see who was at the door. A face at 10 fps is worth more
        # than a smooth thumbnail, so resolution is what this holds on to.
        options.degradation_preference = DEGRADATION[self.opts.degradation]

        width, height = self.opts.size
        if self.video_source is None:
            self.video_source = rtc.VideoSource(width, height)
            # capture_frame is synchronous and thread-safe, so the pipeline's
            # streaming thread calls it directly. Nothing is queued, nothing is
            # scheduled, and a frame is published in the call that produced it.
            self.media.on_video = self._capture_video
        track = rtc.LocalVideoTrack.create_video_track("camera", self.video_source)
        await room.local_participant.publish_track(track, options)
        print(f"publisher: camera published, {width}x{height} {self.opts.codec}")

        if self.opts.audio == "none":
            return
        if self.audio_source is None:
            self.audio_source = rtc.AudioSource(AUDIO_RATE, AUDIO_CHANNELS, queue_size_ms=200)
            self.media.on_audio = self._queue_audio
            asyncio.ensure_future(self._pump_audio())
        mic_options = rtc.TrackPublishOptions()
        mic_options.source = rtc.TrackSource.SOURCE_MICROPHONE
        mic = rtc.LocalAudioTrack.create_audio_track("mic", self.audio_source)
        await room.local_participant.publish_track(mic, mic_options)
        print("publisher: microphone published")

    def _capture_video(self, data, pts_ns):
        width, height = self.opts.size
        frame = rtc.VideoFrame(width, height, rtc.VideoBufferType.I420, data)
        # A GStreamer PTS is nanoseconds and LiveKit wants microseconds. An
        # invalid one (CLOCK_TIME_NONE) means "now", which is what 0 asks for.
        stamp = pts_ns // 1000 if pts_ns is not None and pts_ns != Gst.CLOCK_TIME_NONE else 0
        self.video_source.capture_frame(frame, timestamp_us=stamp)

    def _queue_audio(self, pcm):
        """From the pipeline's thread. capture_frame is async, so it cannot be
        called here - the queue is the crossing, and it drops rather than grows.
        """
        self.loop.call_soon_threadsafe(self._offer_audio, pcm)

    def _offer_audio(self, pcm):
        if self.audio_queue.full():
            # Old microphone audio is worth nothing in a conversation, so the
            # backlog is dropped rather than delivered late.
            self.audio_queue.get_nowait()
        self.audio_queue.put_nowait(pcm)

    async def _pump_audio(self):
        while not self.stopping:
            pcm = await self.audio_queue.get()
            if self.audio_source is None:
                continue
            samples = len(pcm) // (2 * AUDIO_CHANNELS)
            frame = rtc.AudioFrame(pcm, AUDIO_RATE, AUDIO_CHANNELS, samples)
            try:
                await self.audio_source.capture_frame(frame)
            except Exception as error:
                print(f"publisher: microphone frame refused: {error}")
                return

    # --- listening --------------------------------------------------------

    def _start_talkback(self, track, identity):
        if not self.opts.talkback or identity in self.talk_tasks:
            return
        print(f"listener: {identity} started talking")
        self.talk_tasks[identity] = asyncio.ensure_future(self._drain(track, identity))

    def _stop_talkback(self, identity):
        task = self.talk_tasks.pop(identity, None)
        if task is not None:
            print(f"listener: {identity} stopped talking")
            task.cancel()

    async def _drain(self, track, identity):
        """One viewer's audio into the speaker chain, frame by frame.

        Several viewers at once would each get one of these and the card would
        mix them, which is what DESIGN.md asks for. In practice the server hands
        the talk floor to one viewer at a time, so there is normally one.
        """
        stream = rtc.AudioStream(
            track, sample_rate=AUDIO_RATE, num_channels=AUDIO_CHANNELS
        )
        try:
            async for event in stream:
                self.media.play(bytes(event.frame.data))
        except asyncio.CancelledError:
            raise
        except Exception as error:
            print(f"listener: {identity}'s audio ended: {error}")
        finally:
            await stream.aclose()

    # --- what the link is actually doing ----------------------------------

    async def link_report(self):
        """What LiveKit sent, as opposed to what we handed it.

        Every counter in Media stops at the appsink, which is one step too
        early: "30 published/s" stays perfectly steady while the encoder skips
        frames and the viewer watches a frozen picture, because handing a frame
        to the SDK and the SDK getting it out of the house are different
        things. quality_limitation_reason is the whole reason this exists - it
        is WebRTC's own answer to "is it the CPU or the network", and it
        settles in one word an argument that otherwise takes an evening.
        """
        if self.publisher is None or not self.publisher.isconnected():
            return None
        try:
            stats = await self.publisher.get_rtc_stats()
        except Exception:
            return None  # a reconnect in progress; the next one will do

        video, estimate = None, None
        for stat in stats.publisher_stats:
            which = stat.WhichOneof("stats")
            if which == "outbound_rtp" and stat.outbound_rtp.stream.kind == "video":
                video = stat.outbound_rtp
            elif which == "candidate_pair" and stat.candidate_pair.candidate_pair.nominated:
                estimate = stat.candidate_pair.candidate_pair.available_outgoing_bitrate

        if video is None:
            return "link:  nothing published yet"

        out = video.outbound
        sent_kbits = (video.sent.bytes_sent - self.sent_bytes) * 8 / 2000.0
        self.sent_bytes = video.sent.bytes_sent

        limit = LIMITATION.get(out.quality_limitation_reason, "?")
        line = (
            f"link:  {out.frames_per_second:.0f} fps out at {out.frame_width}x{out.frame_height},"
            f" {sent_kbits:.0f} kbit/s"
        )
        if estimate:
            line += f", link estimate {estimate / 1000:.0f} kbit/s"
        if limit != "none":
            line += f"  LIMITED BY {limit.upper()}"
        return line

    # --- when it is over --------------------------------------------------

    def _resync_viewers(self):
        if self.listener is None:
            return
        now = {i for i in self.listener.remote_participants if is_viewer(i)}
        if now == self.viewers:
            return
        for gone in self.viewers - now:
            self._stop_talkback(gone)
        self.viewers = now
        if now:
            self.saw_viewer = True
            print(f"listener: {len(now)} viewer(s): {', '.join(sorted(now))}")
        else:
            print("listener: the last viewer left")

    def should_finish(self):
        """Nobody is watching, so there is nothing to publish for.

        Two separate cases. Nobody ever arrived, which means the request that
        started this process was stale or the viewer gave up - that gets the
        longer timeout. Or everybody left, which is the ordinary end of a call,
        and lingers only long enough that a page reload is not a restart.
        """
        if self.viewers:
            return None
        return self.opts.linger if self.saw_viewer else self.opts.idle_timeout

    def fail(self, code):
        self.exit_code = code
        self.stop()

    def fail_from_thread(self, code):
        """The same, for the pipeline's bus thread. asyncio.Event is not
        thread-safe, and setting one from outside the loop is the kind of bug
        that works on a desk and hangs on a doorbell."""
        self.loop.call_soon_threadsafe(self.fail, code)

    def stop(self):
        self.stopping = True
        self.finished.set()


async def run(opts, credential):
    media = None
    call = None
    loop = asyncio.get_running_loop()
    try:
        # The bus thread is created here but not started until media.start(),
        # which is after the Call exists - so this closure is never called with
        # call still None, however early the pipeline goes wrong.
        media = Media(opts, on_error=lambda _why: call.fail_from_thread(EXIT_MEDIA))
        call = Call(opts, credential, media)
        if not media.start():
            # Joining a room to publish nothing wastes a token and, worse,
            # tells the viewer the camera is there.
            return EXIT_CONFIG

        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, lambda: (print("\nstopping"), call.stop()))

        tasks = [
            asyncio.ensure_future(call.connect("publisher", call.publish)),
            asyncio.ensure_future(call.connect("listener", lambda _room: asyncio.sleep(0))),
            asyncio.ensure_future(supervise(opts, call, media)),
        ]
        await call.finished.wait()

        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await hang_up(call)
        return call.exit_code
    finally:
        if media is not None:
            media.stop()


async def hang_up(call):
    for task in list(call.talk_tasks.values()):
        task.cancel()
    for room in (call.publisher, call.listener):
        if room is not None:
            try:
                await room.disconnect()
            except Exception:
                pass  # going away regardless


async def supervise(opts, call, media):
    """The clock overlay, the two-second report, and the decision to hang up."""
    empty_since = None
    # Now, not zero: otherwise the first report lands before the pipeline has
    # produced a frame and opens with "the source stopped producing frames".
    last_report = time.monotonic()
    while not call.stopping:
        await asyncio.sleep(0.05)
        media.tick_clock()

        now = time.monotonic()
        if now - last_report >= 2.0:
            last_report = now
            media.report(call.viewers)
            link = await call.link_report()
            if link:
                print(link)

        grace = call.should_finish()
        if grace is None:
            empty_since = None
            continue
        if empty_since is None:
            empty_since = now
            continue
        if now - empty_since >= grace:
            why = "the last viewer left" if call.saw_viewer else "nobody ever arrived"
            print(f"\n{why} - stopping")
            call.stop()


def dry_run(opts, seconds):
    """Everything except LiveKit: build the pipeline and run it.

    This is the half that cannot be tested anywhere but the Pi - a parse error,
    a missing element, a camera another process is holding, an ALSA device that
    will not open - and none of it needs a token, a network or a viewer. Run
    this first when something is wrong: if it is clean, the fault is LiveKit or
    the app server, and if it is not, nothing else was ever going to work.
    """
    failed = threading.Event()
    media = Media(opts, on_error=lambda _why: failed.set())
    # Counted to the same point a real call counts them, so the I420 size check
    # runs and a frame that would have been published sheared says so here.
    media.on_video = lambda _data, _pts: None
    media.on_audio = lambda _pcm: None
    if not media.start():
        media.stop()
        return EXIT_CONFIG

    print(f"\ndry run: no LiveKit, no token. {seconds:.0f}s, then stop.\n")
    started = time.monotonic()
    last = started  # see supervise: a report at t=0 would describe nothing
    try:
        while not failed.is_set() and time.monotonic() - started < seconds:
            time.sleep(0.05)
            media.tick_clock()
            if time.monotonic() - last >= 2.0:
                last = time.monotonic()
                media.report(set())
    except KeyboardInterrupt:
        pass
    finally:
        media.stop()

    if failed.is_set():
        return EXIT_CONFIG
    if media.published_frames == 0:
        print("\nno frames reached the hand-off point - the source or the caps are wrong")
        return EXIT_CONFIG
    print("\npipeline is sound. What is left to fail is LiveKit or the app server.")
    return EXIT_OK


def check(opts, credential):
    """Prove the credential and print what the two tokens actually grant."""
    skew_note = False
    for kind in ("publisher", "listener"):
        try:
            grant = fetch_token(opts.url, opts.device_id, kind, credential)
        except TokenError as error:
            print(f"{kind}: {error}")
            return EXIT_CONFIG
        claims = token_claims(grant["token"])
        video = claims.get("video", {})
        print(f"{kind}:")
        print(f"  url      {grant['url']}")
        print(f"  room     {grant['room']}")
        print(f"  identity {grant['identity']}")
        print(
            f"  grants   canPublish={video.get('canPublish')}"
            f" canSubscribe={video.get('canSubscribe')}"
            f" canPublishData={video.get('canPublishData')}"
        )
        nbf, exp = claims.get("nbf"), claims.get("exp")
        if nbf and exp:
            now = time.time()
            print(f"  valid    {exp - nbf:.0f}s, from {nbf - now:+.1f}s to {exp - now:+.1f}s")
            # nbf is the moment the app server minted it. If our clock is behind
            # the server's, that is in our future - harmless, since only LiveKit
            # checks it, but a large number here means one of the two machines
            # is not on NTP and that is worth knowing before it is a mystery.
            if abs(nbf - now) > 5:
                skew_note = True
    if skew_note:
        print("\nnote: our clock and the app server's differ by more than 5 seconds.")
        print("      Only LiveKit enforces nbf, so this is not fatal here, but fix it:")
        print("      timedatectl, and w32tm /resync on Windows.")
    return EXIT_OK


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", required=True, help="the app server, e.g. http://192.168.0.219:4000")
    parser.add_argument("--device-id", required=True)
    # Not required, because --dry-run deliberately needs nothing but the Pi.
    parser.add_argument("--credential-file")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fetch both tokens, print what they grant, and exit",
    )
    parser.add_argument(
        "--dry-run",
        type=float,
        nargs="?",
        const=10.0,
        default=None,
        metavar="SECONDS",
        help="build and run the pipeline with no LiveKit and no token, then stop."
        " The one test that isolates the camera and the sound card from everything else",
    )
    parser.add_argument(
        "--video",
        choices=("camera", "test"),
        default="camera",
        help="camera is the real one through libcamera; test is videotestsrc",
    )
    parser.add_argument(
        "--size",
        type=parse_size,
        default=None,
        metavar="WIDTHxHEIGHT",
        help=f"picture size (camera {CAMERA_SIZE}, test {TEST_SIZE}; the imx708 is"
        " 16:9, so asking it for 4:3 crops the sides off)",
    )
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS, help=f"default {DEFAULT_FPS}")
    parser.add_argument(
        "--codec",
        choices=("h264", "vp8"),
        default="h264",
        help="what LiveKit encodes with. h264 matches what the recorder produces",
    )
    parser.add_argument(
        "--degradation",
        choices=sorted(DEGRADATION),
        default="resolution",
        help="what to give up on a tight link. resolution keeps the picture size and"
        " drops frames; balanced is WebRTC's default and shrinks the picture",
    )
    parser.add_argument(
        "--overlay",
        action="store_true",
        help="burn the Pi's clock into the picture, to measure glass-to-glass delay."
        " It costs a Pango re-render 20x a second and a blend on every frame",
    )
    parser.add_argument(
        "--simulcast",
        choices=("on", "off"),
        default="off",
        help="publish several quality layers. Each one is another software encode"
        " on the Pi; on is worth it only if viewers are on poor connections",
    )
    parser.add_argument(
        "--bitrate-kbps",
        type=int,
        default=1500,
        help="video bitrate ceiling (default 1500; LiveKit lowers it under congestion)",
    )
    parser.add_argument(
        "--focus",
        type=parse_focus,
        default="continuous",
        metavar="MODE",
        help="continuous, default (leave libcamera alone), or a distance in metres."
        " Not optional in practice: af-mode defaults to manual at infinity",
    )
    parser.add_argument(
        "--sensor-mode",
        choices=("full", "auto"),
        default="full",
        help="full reads the whole sensor and keeps the wide lens wide",
    )
    parser.add_argument(
        "--audio",
        choices=("alsa", "test", "none"),
        default="alsa",
        help="alsa is the real microphone; test is a once-a-second tick; none publishes no audio",
    )
    parser.add_argument("--mic-device", default=DEFAULT_MIC, help=f"default {DEFAULT_MIC}")
    parser.add_argument("--speaker-device", default=DEFAULT_SPEAKER, help=f"default {DEFAULT_SPEAKER}")
    parser.add_argument(
        "--no-talkback",
        action="store_true",
        help="do not play viewers out of the speaker (the room is still watched)",
    )
    parser.add_argument(
        "--aec",
        choices=("on", "off"),
        default="on",
        help="echo cancellation. Off means the microphone hears the speaker beside it",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=30.0,
        help="seconds to wait for the first viewer before giving up (default 30)",
    )
    parser.add_argument(
        "--linger",
        type=float,
        default=3.0,
        help="seconds to stay up after the last viewer leaves (default 3)",
    )
    parser.add_argument(
        "--reconnect-timeout",
        type=float,
        default=60.0,
        help="seconds to keep trying to rejoin before giving up (default 60)",
    )
    parser.add_argument(
        "--pattern",
        default="ball",
        help="--video test only: ball is a dot on black, smpte is colour bars",
    )
    args = parser.parse_args()

    args.talkback = not args.no_talkback
    args.aec = args.aec == "on"
    args.simulcast = args.simulcast == "on"
    # webrtcdsp cancels what the probe heard being played. With nothing playing
    # there is no probe, no echo, and a canceller looking for an element that
    # was never built - so this is a consequence rather than a choice.
    if args.aec and (not args.talkback or args.audio == "none"):
        args.aec = False
    if args.size is None:
        args.size = parse_size(CAMERA_SIZE if args.video == "camera" else TEST_SIZE)

    def credential():
        if not args.credential_file:
            sys.exit("--credential-file is needed for anything that talks to the app server")
        return read_credential(args.credential_file)

    # Before Gst.init, because proving the credential needs nothing else.
    if args.check:
        return check(args, credential())

    Gst.init(None)
    require("appsink", "install gstreamer1.0-plugins-base")
    if args.video == "camera":
        require("libcamerasrc", "install gstreamer1.0-libcamera; or use --video test")
    else:
        require("videotestsrc", "install gstreamer1.0-plugins-base")
    if args.audio == "alsa":
        require("alsasrc", "install gstreamer1.0-alsa")
    elif args.audio == "test":
        require("audiotestsrc", "install gstreamer1.0-plugins-base")
    if args.talkback:
        require("appsrc", "install gstreamer1.0-plugins-base")
        require("alsasink", "install gstreamer1.0-alsa")
    if args.aec and args.audio != "none":
        require("webrtcdsp", "install gstreamer1.0-plugins-bad, or run with --aec off")

    width, height = args.size
    picture = "the camera" if args.video == "camera" else f"videotestsrc {args.pattern}"
    sound = {
        "none": "no microphone",
        "test": "a test tick, NOT the microphone",
        "alsa": f"microphone {args.mic_device}",
    }[args.audio]
    print(f"live call: {picture} at {width}x{height}@{args.fps} {args.codec}, {sound}")
    if args.talkback:
        print(f"           viewers come out of {args.speaker_device}")
    else:
        print("           no talkback - viewers are watched but not played")

    if (
        args.talkback
        and args.audio == "alsa"
        and args.speaker_device == args.mic_device
        and not args.aec
    ):
        print("\n  NOTE: one card, no echo cancellation. The Pi will hear itself and can")
        print("        howl. Drop --aec off, or send the voice somewhere the microphone")
        print("        cannot reach it:  --speaker-device hw:CARD=Headphones\n")

    if args.dry_run is not None:
        return dry_run(args, args.dry_run)

    print(f"           tokens from {args.url} as {args.device_id}")
    if rtc is None:
        sys.exit(
            "the LiveKit SDK is not installed. It is a pip package, and it needs a\n"
            "venv that can still see python3-gi:\n"
            "  python3 -m venv --system-site-packages /opt/porchlight/venv\n"
            "  /opt/porchlight/venv/bin/pip install livekit\n"
            "then run this with /opt/porchlight/venv/bin/python3."
        )

    try:
        return asyncio.run(run(args, credential()))
    except KeyboardInterrupt:
        return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
