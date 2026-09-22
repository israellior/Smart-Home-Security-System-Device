#!/usr/bin/env python3
"""Steps 1-3 and 5: a call between the Pi and a browser, over one WebRTC connection.

    libcamerasrc -> clock overlay -> H.264 -> rtph264pay --\\
                                                            >-- webrtcbin <-> browser
    alsasrc (WM8960) -> 48 kHz mono -> Opus -> rtpopuspay -/       |
                                                                  v
              alsasink (WM8960) <- opusdec <- rtpopusdepay <- (the browser's mic)

Step 1 was the handshake with one video track. Step 2 put a second track on the
*same* webrtcbin - one transport, one pipeline clock, one set of RTCP sender
reports, which is what holds sound and picture together. Step 3 adds a third
m-line pointing the other way, and that makes it a call. Step 5 changes only
where the picture comes from: a real camera instead of videotestsrc, which
touches nothing downstream of the caps.

The return path is built differently from the other two on purpose. There is no
branch for it in the pipeline below, because nothing here produces it: it is
asked for with add-transceiver, and webrtcbin grows a src pad for it only once
the browser answers and starts sending. The speaker chain is assembled in
on_incoming_stream, against a pipeline that is already PLAYING.

    ./webrtc-video.py 192.168.0.219                     camera, mic in, speaker out
    ./webrtc-video.py 192.168.0.219 --video test        videotestsrc, Steps 1-3 again
    ./webrtc-video.py 192.168.0.219 --size 1280x720     bigger picture
    ./webrtc-video.py 192.168.0.219 --focus 1.5         fix the lens at 1.5 m
    ./webrtc-video.py 192.168.0.219 --no-talkback       Step 2 again, one way
    ./webrtc-video.py 192.168.0.219 --audio test        a tick, when the HAT is down
    ./webrtc-video.py 192.168.0.219 --audio none        Step 1 again, video only
    ./webrtc-video.py 192.168.0.219 --encoder v4l2      the Pi 4's hardware encoder
    ./webrtc-video.py 192.168.0.219 --print-sdp         dump the offer and the answer
    ./webrtc-video.py 192.168.0.219 --speaker-device hw:CARD=Headphones

Two things bite at this step. The browser needs a **secure context** to hand over
its microphone at all, so the page has to be opened on http://localhost:3000 (or
over HTTPS), not on the LAN address. And the Pi's speaker sits next to the Pi's
microphone, so it will hear itself until Step 4 adds webrtcdsp - headphones, or
--speaker-device hw:CARD=Headphones, until then.

Needs:  sudo apt install -y python3-gi python3-gst-1.0 python3-websocket \
          gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
          gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-alsa \
          gstreamer1.0-libcamera
"""

import argparse
import json
import sys
import threading
import time
from datetime import datetime

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import GLib, Gst, GstSdp, GstWebRTC  # noqa: E402

import websocket  # python3-websocket  # noqa: E402

RECONNECT_S = 2  # keep trying, so a server restart does not need a Pi restart

# The WM8960 HAT. Always by name: card numbers move between boots, so hw:2,0 is
# right until the day it silently points at the headphone jack instead.
DEFAULT_MIC = "hw:CARD=wm8960soundcard"

# The same card plays as well as records, which is the whole point of the HAT and
# also the reason Step 4 exists: what comes out of it goes straight back into the
# microphone beside it. hw:CARD=Headphones is the Pi's own 3.5 mm jack, useful for
# proving the two directions work before any echo cancellation does.
DEFAULT_SPEAKER = "hw:CARD=wm8960soundcard"

# bundle-policy=max-bundle puts every track on one ICE transport and one UDP port
# pair. Without it each m-line would negotiate its own, which is more ports, more
# candidates, and two clocks to reconcile instead of one.
WEBRTC = "webrtcbin name=webrtc bundle-policy=max-bundle"

# Step 5. The Camera Module 3 has no usable V4L2 capture path of its own:
# /dev/video0 hands back raw Bayer, and it is the Pi's ISP - a separate device -
# that turns that into a picture. libcamera drives both halves as one unit, so
# libcamerasrc is the source, not v4l2src.
#
# format=I420 is pinned deliberately, and this was measured rather than assumed.
# Left to itself libcamerasrc negotiates NV21 here, which makes the videoconvert
# in the encoder branch do a real de-interleave on every frame instead of being
# the passthrough it is with videotestsrc. The Pi's ISP produces I420 directly,
# so asking for it moves that work to hardware that was going to run anyway.
#
# The queue is what stops latency growing when the encoder falls behind.
# leaky=downstream discards the *oldest* buffer rather than the newest, so a
# slow encoder costs dropped frames instead of a picture that drifts further
# behind the sound with every second.
CAMERA = (
    "libcamerasrc name=camera"
    " ! video/x-raw,format=I420,width={width},height={height},framerate={fps}/1"
    " ! queue max-size-buffers=2 leaky=downstream"
)

# libcamera chooses a sensor mode to satisfy the requested output size, and for
# anything at or below 1536x864 it picks the imx708's binned 1536x864 mode. That
# mode reads only the centre 3072x1728 of the 4608x2592 array - rpicam-hello
# reports it as "(768, 432)/3072x1728 crop" - which throws away about a third of
# the frame width. On the WIDE lens that is roughly 120 degrees of diagonal
# field of view reduced to about 98, and no amount of scaling gets it back.
#
# 2304x1296 reads the full array and still runs at 56 fps, so it carries 30 fps
# with room to spare; the ISP scales it down to whatever --size asked for. The
# cost is sensor readout and ISP bandwidth, not CPU.
FULL_FRAME_MODE = "sensor/config,width=2304,height=1296,depth=10"

# A moving pattern by default, so a frozen picture is obvious at a glance rather
# than looking like a still image that arrived correctly. "ball" is one small ball
# on a black field, which reads as a blank screen in a small window; "smpte" gives
# the familiar colour bars instead, with the clock overlay to prove it is live.
TEST_SOURCE = (
    "videotestsrc is-live=true pattern={pattern} animation-mode=running-time"
    " ! video/x-raw,width={width},height={height},framerate={fps}/1"
)

# The imx708 is 4608x2592 - 16:9. Ask it for 4:3 and the ISP crops the sides off
# to match, which on the *wide* lens throws away exactly the field of view the
# wide lens was bought for. So the camera's default is 16:9, at about the same
# pixel count and the same bitrate as the 4:3 one it replaces.
#
# videotestsrc keeps 640x480 rather than following it, because its whole job now
# is to reproduce Steps 1-3 unchanged when the camera is the suspect.
CAMERA_SIZE = "640x360"
TEST_SIZE = "640x480"
DEFAULT_FPS = 30

# The Pi's wall clock, burnt into the picture with millisecond resolution. Put the
# browser's own clock next to the video and the difference is the delay.
OVERLAY = (
    ' ! textoverlay name=clock font-desc="Mono 18" halignment=left valignment=top'
    " xpad=12 ypad=12 shaded-background=true"
)

ENCODERS = {
    # tune=zerolatency turns off B-frames and lookahead, which is what would
    # otherwise hold frames back for tens of milliseconds.
    "x264": (
        " ! videoconvert ! video/x-raw,format=I420"
        " ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 key-int-max=30"
        " ! video/x-h264,profile=constrained-baseline"
    ),
    # repeat_sequence_header makes the hardware encoder emit SPS/PPS with every
    # keyframe. Without it a browser that joins late never learns how to decode.
    "v4l2": (
        " ! videoconvert ! video/x-raw,format=I420"
        ' ! v4l2h264enc extra-controls="controls,repeat_sequence_header=1'
        ',h264_i_frame_period=30,video_bitrate=2000000"'
        " ! video/x-h264,level=(string)4"
    ),
}

# config-interval=-1 repeats the parameter sets on every keyframe, for the same
# reason. aggregate-mode=zero-latency pushes each packet out as it is made rather
# than collecting a whole frame first.
PAYLOAD = (
    " ! h264parse config-interval=-1"
    " ! rtph264pay name=vpay pt=96 config-interval=-1 aggregate-mode=zero-latency"
    " ! application/x-rtp,media=video,encoding-name=H264,payload=96"
    " ! webrtc."
)

# Both microphones on the HAT feed one stereo capture stream, downmixed here to
# mono: a room intercom gains nothing from two channels, mono halves the bitrate,
# and Step 4's webrtcdsp has one signal to cancel instead of two.
#
# alsasrc's default buffer-time is 200 ms, which is 200 ms of delay bought for
# nothing on a LAN. 40 ms of ring buffer read in 10 ms slices is still far more
# headroom than a 20 ms Opus frame needs.
#
# The source caps are deliberately left open: whatever rate the card actually
# offers, audioresample brings it to the 48 kHz Opus works in natively. Pinning
# the card to 48 kHz here would turn a resample into a hard failure.
AUDIO_SOURCES = {
    # The device name is quoted because it contains an '=' of its own
    # (hw:CARD=...), which the pipeline parser would otherwise have to guess at.
    "alsa": (
        'alsasrc name=mic device="{device}" buffer-time=40000 latency-time=10000'
    ),
    # wave=ticks is one short beep a second. A continuous tone proves just as much
    # and is unbearable within a minute; a tick also makes a dropout or a stutter
    # audible as a missed beat, which a steady tone hides completely.
    "test": (
        "audiotestsrc name=mic is-live=true wave=ticks freq=880"
    ),
}

# audio-type=voice tells Opus what it is compressing, which at these bitrates is
# the difference between speech and gargling. 32 kbit/s mono is generous for a
# voice; the 20 ms frame is Opus's default and WebRTC's usual, low enough to not
# dominate the delay and long enough that per-packet overhead stays sane.
#
# pt=97, not 96: the video track already claimed 96, and two media sharing one
# payload type on one bundled transport is a silent, baffling failure.
#
# If the browser ever answers with this m-line's port set to 0 (--print-sdp shows
# it), look at the rtpmap: mono Opus is legal but unusual, and the fix is to ask
# for channels=2 in the caps below rather than 1.
ENCODE_AUDIO = (
    " ! audioconvert ! audioresample"
    " ! audio/x-raw,rate=48000,channels=1"
    " ! opusenc audio-type=voice bitrate=32000 frame-size=20"
    " ! rtpopuspay pt=97"
    " ! application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000"
    " ! webrtc."
)

# Step 3, the way back. This m-line has no branch in the pipeline above, because
# nothing here produces it: it is asked for with add-transceiver, and webrtcbin
# grows a src pad for it only once the browser answers and starts sending.
#
# pt=98 because 96 and 97 are taken. Under BUNDLE every payload type on the one
# shared transport has to be distinct, even across different m-lines.
#
# encoding-params=2 is not cosmetic. It becomes the channel count in the rtpmap,
# and leaving it out emits "OPUS/48000", which SDP reads as one channel. Chrome
# will happily *receive* mono Opus but will not *send* it - its encoder only
# offers opus/48000/2 - so an m-line it has to send on with no stereo rtpmap
# matches none of its codecs, and it answers "m=audio 0 UDP/TLS/RTP/SAVPF 0",
# which is its way of saying the m-line was unusable rather than unwanted.
# This is the mono-Opus trap the send side got away with and this one does not.
RETURN_CAPS = (
    "application/x-rtp,media=audio,encoding-name=OPUS,clock-rate=48000"
    ",payload=98,encoding-params=(string)2"
)

# What arrives on that pad has already been through webrtcbin's own jitter
# buffer, so this chain only has to turn it back into sound. Same ALSA tuning as
# the capture side, and for the same reason: the default 200 ms buffer would add
# a fifth of a second to a conversation for nothing.
SPEAKER = (
    "rtpopusdepay ! opusdec"
    " ! audioconvert ! audioresample{level}"
    ' ! alsasink name=speaker device="{device}" buffer-time=40000 latency-time=10000'
)

# Measured after the decoder, so it reports what would come out of the speaker
# rather than what arrived on the wire. Silence that decodes perfectly and
# silence that never arrived look identical at the speaker and nothing else
# tells them apart.
LEVEL = " ! level name=talklevel interval=1000000000 post-messages=true"


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


def set_sensor_mode(camera, spec):
    """Force the full-frame sensor mode, so the wide lens stays wide.

    Same reasoning as set_focus: sensor-config is a recent addition to
    libcamerasrc and setting a property that is not there is fatal, so it is
    applied here rather than in the parse_launch string.
    """
    if "sensor-config" not in {p.name for p in camera.list_properties()}:
        print("camera: this libcamerasrc cannot choose the sensor mode, so the field")
        print("        of view is whatever libcamera picks for the requested size")
        return

    # new_from_string is the binding-friendly one and returns None on a bad
    # string; from_string is older and hands back a tuple in some versions.
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

    libcamera's GStreamer element turns each of the sensor's controls into a
    GObject property, but which ones exist depends on the sensor and on the
    libcamera version - a fixed-focus Camera Module 2 has none of these at all.
    Setting a property that is not there is fatal, and putting them in the
    parse_launch string would take the whole pipeline down with it, so they are
    applied here where a missing one is a printed note instead.

    Leaving focus alone is not a neutral choice here. libcamerasrc's af-mode
    defaults to *manual* with lens-position 0, and 0 dioptres is infinity - so
    an unconfigured Camera Module 3 sits focused past the horizon while the
    caller stands a metre away. Something has to be set.
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
    # scene - it refocuses on nothing, visibly, every few seconds. Fixing the
    # lens at the distance of the doorstep costs nothing and never hunts.
    # libcamera measures lens position in dioptres, which is 1/metres.
    if "lens-position" not in names:
        print("camera: no lens-position property, falling back to continuous autofocus")
        Gst.util_set_object_arg(camera, "af-mode", "continuous")
        return
    Gst.util_set_object_arg(camera, "af-mode", "manual")
    camera.set_property("lens-position", 1.0 / focus)
    print(f"camera: focus fixed at {focus:g} m (lens-position {1.0 / focus:.2f})")


DIRECTIONS = ("a=sendrecv", "a=sendonly", "a=recvonly", "a=inactive")


def show_media_lines(label, sdp_text):
    """Print each m= line with the detail that explains it.

    Port 0 means two entirely different things depending on where it appears. In
    an offer it almost always goes with a=bundle-only: the m-line is real, it
    just may not be used on a transport of its own. In an answer it means the
    other side turned the track down flat. Reading the first as the second sends
    you hunting a fault that is not there, which is exactly what an earlier
    version of this function did.

    The rtpmap is printed too, because when a track *is* refused the reason is
    usually written right there - a codec line the other side cannot match.
    """
    sections = []
    for raw in sdp_text.splitlines():
        line = raw.strip()
        if line.startswith("m="):
            sections.append({"m": line, "rtpmap": [], "bundle_only": False, "direction": None})
        elif sections:
            section = sections[-1]
            if line.startswith("a=rtpmap:"):
                section["rtpmap"].append(line[len("a=rtpmap:") :])
            elif line == "a=bundle-only":
                section["bundle_only"] = True
            elif line in DIRECTIONS:
                section["direction"] = line[2:]

    pad = " " * len(label)
    for section in sections:
        detail = [d for d in (section["direction"], ", ".join(section["rtpmap"])) if d]
        suffix = f"   [{'; '.join(detail)}]" if detail else ""
        print(f"{label}: {section['m']}{suffix}")
        if section["m"].split()[1:2] != ["0"]:
            continue
        if section["bundle_only"]:
            print(f"{pad}  ^ port 0 + a=bundle-only: normal, it rides the first m-line")
        else:
            print(f"{pad}  ^ REFUSED - the other side will not carry this track")


class Session:
    """One browser: its own pipeline, built when it asks and torn down when it goes."""

    def __init__(self, client, peer_id, opts):
        self.client = client
        self.peer_id = peer_id
        self.print_sdp = opts.print_sdp
        self.speaker_device = opts.speaker_device
        self.speaker_bin = None
        self.offered = False

        # What the way back is actually carrying. Counted rather than assumed:
        # every other signal at this point is about what was *negotiated*.
        self.talk_packets = 0
        self.talk_bytes = 0
        self.talk_seen = (0, 0)  # packets, bytes at the last report
        self.talk_level = None

        # And the same for the way out, at BOTH ends of the video branch. A
        # frozen picture in the browser has three possible causes that look
        # identical from there, and these two counters separate all three:
        # frames leaving the camera but no RTP means the encoder or payloader
        # stalled; neither moving means the source died; both still counting
        # means the fault is the network or the browser, not this pipeline.
        self.cam_frames = 0
        self.rtp_packets = 0
        self.rtp_bytes = 0
        self.out_seen = (0, 0, 0)
        self.stats_timer = 0

        overlay = OVERLAY if Gst.ElementFactory.find("textoverlay") else ""
        if not overlay:
            print("note: textoverlay is missing, so there is no clock to measure delay with")

        width, height = opts.size
        if opts.video == "camera":
            source = CAMERA.format(width=width, height=height, fps=opts.fps)
        else:
            source = TEST_SOURCE.format(
                pattern=opts.pattern, width=width, height=height, fps=opts.fps
            )

        video = source + overlay + ENCODERS[opts.encoder] + PAYLOAD
        audio = ""
        if opts.audio != "none":
            audio = " " + AUDIO_SOURCES[opts.audio].format(device=opts.mic_device) + ENCODE_AUDIO

        # webrtcbin first, then each branch ending in "webrtc.". Each of those
        # requests the next sink pad, and a pad is what makes a transceiver, so
        # the order here is the order of the m-lines in the offer.
        description = f"{WEBRTC} {video}{audio}"
        print(f"\npipeline:\n  {description}\n")

        self.pipeline = Gst.parse_launch(description)

        # Pin the clock, or the camera and the microphone end up on two of them.
        #
        # alsasrc provides a clock and libcamerasrc does not, so with both in one
        # pipeline GStreamer picks the sound card's clock. libcamerasrc goes on
        # timestamping from the system monotonic clock regardless, so every video
        # buffer reaches webrtcbin with a running time worked out by subtracting a
        # base time in one clock's units from a timestamp in another's. The video
        # branch then stalls within a second - the camera keeps producing 30 fps
        # and nothing leaves the payloader - while audio, stamped against its own
        # clock, runs perfectly and hides what is happening.
        #
        # videotestsrc never showed this because it paces itself off whichever
        # clock the pipeline chose, so Steps 1-3 were immune by accident.
        #
        # The system clock costs the audio side nothing that matters: alsasrc
        # timestamps against it instead, and the drift between the card's crystal
        # and the system clock is what audioresample and the browser's jitter
        # buffer already absorb.
        self.pipeline.use_clock(Gst.SystemClock.obtain())

        # Camera controls are properties, not pipeline syntax, and they have to
        # be set before the element goes to PLAYING for the first frame to come
        # out already in focus.
        camera = self.pipeline.get_by_name("camera")
        if camera is not None:
            if opts.sensor_mode == "full":
                set_sensor_mode(camera, FULL_FRAME_MODE)
            if opts.focus != "default":
                set_focus(camera, opts.focus)

        self.webrtc = self.pipeline.get_by_name("webrtc")
        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)
        self.webrtc.connect("notify::ice-connection-state", self.on_ice_state)
        self.webrtc.connect("notify::connection-state", self.on_connection_state)

        # Each branch above requested a sink pad, and each pad made a transceiver.
        # These are the ones we produce, so they are send-only: otherwise the
        # browser offers to send media back on those same m-lines and the answer
        # describes tracks nobody here reads.
        sending = 0
        while True:
            transceiver = self.webrtc.emit("get-transceiver", sending)
            if transceiver is None:
                break
            transceiver.direction = GstWebRTC.WebRTCRTPTransceiverDirection.SENDONLY
            sending += 1
        if not sending:
            print("note: could not reach any transceiver; leaving directions at their default")

        # Step 3: ask for one more m-line, pointing the other way. Adding it here,
        # before the pipeline ever leaves NULL, is what puts it in the offer - a
        # transceiver added after negotiation would need a second round of it.
        self.talkback = not opts.no_talkback
        if self.talkback:
            self.webrtc.connect("pad-added", self.on_incoming_stream)
            self.webrtc.emit(
                "add-transceiver",
                GstWebRTC.WebRTCRTPTransceiverDirection.RECVONLY,
                Gst.caps_from_string(RETURN_CAPS),
            )
        print(
            f"webrtc: {sending} transceiver(s) sending"
            + (", 1 receiving" if self.talkback else ", none receiving")
        )

        self.watch_outgoing()

        self.clock = self.pipeline.get_by_name("clock")
        self.clock_timer = GLib.timeout_add(50, self.tick_clock) if self.clock else 0

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)

        self.pipeline.set_state(Gst.State.PLAYING)

    # --- the handshake ----------------------------------------------------

    def on_negotiation_needed(self, element):
        # Both branches are built before the pipeline ever leaves NULL, so this
        # fires once with both m-lines ready. Guarding anyway: a second offer
        # over the same session would leave the browser answering the wrong one.
        if self.offered:
            print("webrtc: negotiation needed again, ignoring it")
            return
        self.offered = True
        print("webrtc: negotiation needed, creating an offer")
        promise = Gst.Promise.new_with_change_func(self.on_offer_created, element, None)
        element.emit("create-offer", None, promise)

    def on_offer_created(self, promise, element, _user_data):
        promise.wait()
        reply = promise.get_reply()
        if reply is None:
            print("webrtc: create-offer returned nothing")
            return
        offer = reply.get_value("offer")
        if offer is None:
            print(f"webrtc: create-offer failed: {reply.to_string()}")
            return

        # Read the SDP out first. Handing the description to set-local-description
        # below leaves this Python wrapper's .sdp empty, and by then the only copy
        # of the text we have to send is gone.
        text = offer.sdp.as_text()

        # Setting our own description is what starts ICE gathering, so candidates
        # only begin to arrive after this line.
        local = Gst.Promise.new()
        element.emit("set-local-description", offer, local)
        local.interrupt()

        if self.print_sdp:
            print(f"\n----- offer we are sending -----\n{text}-----\n")
        show_media_lines("offer", text)
        self.client.send({"type": "offer", "to": self.peer_id, "sdp": text})

    def set_answer(self, sdp_text):
        if self.print_sdp:
            print(f"\n----- answer the browser sent -----\n{sdp_text}-----\n")
        show_media_lines("answer", sdp_text)
        _ok, sdpmsg = GstSdp.SDPMessage.new()
        GstSdp.sdp_message_parse_buffer(sdp_text.encode(), sdpmsg)
        answer = GstWebRTC.WebRTCSessionDescription.new(GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg)
        promise = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", answer, promise)
        promise.interrupt()
        print("webrtc: answer applied")

    def on_ice_candidate(self, _element, mlineindex, candidate):
        self.client.send(
            {
                "type": "ice",
                "to": self.peer_id,
                "candidate": {"candidate": candidate, "sdpMLineIndex": mlineindex},
            }
        )

    def add_ice(self, candidate):
        self.webrtc.emit("add-ice-candidate", candidate["sdpMLineIndex"], candidate["candidate"])

    # --- the way back ------------------------------------------------------

    def on_incoming_stream(self, _element, pad):
        """The browser started sending. Build somewhere for it to come out."""
        if pad.direction != Gst.PadDirection.SRC:
            return  # webrtcbin's own sink pads come through here too
        if self.speaker_bin is not None:
            print("webrtc: a second incoming stream appeared; ignoring it")
            return

        caps = pad.get_current_caps()
        print(f"webrtc: the browser's microphone arrived ({caps.to_string() if caps else '?'})")

        level = LEVEL if Gst.ElementFactory.find("level") else ""
        if not level:
            print("note: the 'level' element is missing, so loudness cannot be reported")
        try:
            self.speaker_bin = Gst.parse_bin_from_description(
                SPEAKER.format(device=self.speaker_device, level=level), True
            )
        except GLib.Error as err:
            print(f"webrtc: could not build the speaker chain: {err.message}")
            self.speaker_bin = None
            return

        self.pipeline.add(self.speaker_bin)
        # The pipeline is already PLAYING, so a bin added now starts in NULL and
        # would sit there silently. This is what catches it up.
        self.speaker_bin.sync_state_with_parent()

        result = pad.link(self.speaker_bin.get_static_pad("sink"))
        if result != Gst.PadLinkReturn.OK:
            print(f"webrtc: could not reach the speaker: {result.value_nick}")
            return

        print(f"webrtc: talking back through {self.speaker_device}")
        # This pad appearing proves nothing about traffic: webrtcbin takes the
        # SSRC from the SDP, so the pad can exist before a single RTP packet
        # does. The probe counts what genuinely comes down it.
        pad.add_probe(Gst.PadProbeType.BUFFER, self.count_talkback)

    def count_talkback(self, _pad, info):
        buffer = info.get_buffer()
        if buffer is not None:
            self.talk_packets += 1
            self.talk_bytes += buffer.get_size()
        return Gst.PadProbeReturn.OK

    # --- the way out -------------------------------------------------------

    def watch_outgoing(self):
        """Count raw frames out of the source and RTP packets into webrtcbin.

        Neither number is available from webrtcbin without asking it for stats,
        and by then a stall has already been invisible for however long it took
        to notice. Probes cost a function call per buffer and answer the only
        question a frozen picture raises: which end stopped.
        """
        source = self.pipeline.get_by_name("camera")
        if source is not None:
            pad = source.get_static_pad("src")
            if pad is not None:
                pad.add_probe(Gst.PadProbeType.BUFFER, self.count_frame)

        payloader = self.pipeline.get_by_name("vpay")
        if payloader is not None:
            pad = payloader.get_static_pad("src")
            if pad is not None:
                pad.add_probe(Gst.PadProbeType.BUFFER, self.count_rtp)

        self.stats_timer = GLib.timeout_add_seconds(2, self.report)

    def count_frame(self, _pad, info):
        if info.get_buffer() is not None:
            self.cam_frames += 1
        return Gst.PadProbeReturn.OK

    def count_rtp(self, _pad, info):
        buffer = info.get_buffer()
        if buffer is not None:
            self.rtp_packets += 1
            self.rtp_bytes += buffer.get_size()
        return Gst.PadProbeReturn.OK

    def report(self):
        self.report_video()
        if self.speaker_bin is not None:
            self.report_talkback()
        return True

    def report_video(self):
        frames = self.cam_frames - self.out_seen[0]
        packets = self.rtp_packets - self.out_seen[1]
        kbits = (self.rtp_bytes - self.out_seen[2]) * 8 / 2000.0
        self.out_seen = (self.cam_frames, self.rtp_packets, self.rtp_bytes)

        # Reported per second, because a frame rate is the thing being judged
        # and nobody thinks in frames per two seconds.
        source = f"{frames / 2:.0f} fps from the source, " if self.cam_frames or frames else ""

        if packets == 0 and frames == 0:
            print("video: NOTHING - the source stopped producing frames")
        elif packets == 0:
            print(f"video: {source}but no RTP leaving - the encoder or payloader stalled")
        else:
            print(f"video: {source}{packets} RTP packets/2s ({kbits:.0f} kbit/s)")

    def report_talkback(self):
        packets = self.talk_packets - self.talk_seen[0]
        kbits = (self.talk_bytes - self.talk_seen[1]) * 8 / 2000.0
        self.talk_seen = (self.talk_packets, self.talk_bytes)

        if packets == 0:
            print("talkback: nothing arriving - the browser is not sending")
            return

        if self.talk_level is None:
            loudness = ""
        elif self.talk_level < -70:
            loudness = f", {self.talk_level:.0f} dBFS - that is silence, not speech"
        else:
            loudness = f", {self.talk_level:.0f} dBFS"
        print(f"talkback: {packets} packets/2s ({kbits:.0f} kbit/s){loudness}")

    # --- what it is doing -------------------------------------------------

    def on_ice_state(self, element, _param):
        print(f"webrtc: ICE {element.get_property('ice-connection-state').value_nick}")

    def on_connection_state(self, element, _param):
        state = element.get_property("connection-state").value_nick
        print(f"webrtc: connection {state}")
        if state == "failed":
            print("        no route found between the two machines")

    def on_bus_message(self, _bus, message):
        if message.type == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            where = message.src.get_name() if message.src else "?"
            print(f"pipeline error from {where}: {err.message}")
            if debug:
                print(f"  {debug}")
            if where == "speaker":
                print("        The speaker would not open. An ALSA hw: device goes to one")
                print("        process at a time, so something else may hold it. Or send")
                print("        the voice elsewhere:  --speaker-device hw:CARD=Headphones")
            if where == "camera":
                print("        The camera would not open. Like an ALSA hw: device it goes to")
                print("        one process at a time, so rpicam-hello or a second copy of")
                print("        this script will hold it. If it was never detected at all the")
                print("        ribbon or config.txt is the cause:  ./check-camera.sh")
                print("        To carry on without the camera meanwhile:  --video test")
            if where == "mic":
                print("        The microphone would not open. If this is 'Invalid argument'")
                print("        it is the known WM8960 fault: Waveshare's out-of-tree modules")
                print("        collide with the in-tree ones, so the card enumerates but its")
                print("        PCM was never built. Fix it:  ./fix-wm8960.sh")
                print("        To carry on without the HAT meanwhile:  --audio test")
            self.client.stop_session("the pipeline failed")
        elif message.type == Gst.MessageType.WARNING:
            err, _debug = message.parse_warning()
            print(f"pipeline warning: {err.message}")
        elif message.type == Gst.MessageType.ELEMENT:
            structure = message.get_structure()
            if structure is not None and structure.get_name() == "level":
                # rms is one value per channel, in dB below full scale.
                try:
                    channels = structure.get_value("rms")
                    if channels:
                        self.talk_level = max(channels)
                except (TypeError, ValueError):
                    pass

    def tick_clock(self):
        self.clock.set_property("text", datetime.now().strftime("%H:%M:%S.%f")[:-3])
        return True

    def close(self):
        if self.clock_timer:
            GLib.source_remove(self.clock_timer)
        if self.stats_timer:
            GLib.source_remove(self.stats_timer)
        self.pipeline.set_state(Gst.State.NULL)


class Client:
    """The signaling side: one WebSocket to server.js, reconnecting on its own."""

    def __init__(self, url, opts):
        self.url = url
        self.opts = opts
        self.ws = None
        self.session = None
        self.my_id = None
        self.stopping = False

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stopping:
            self.ws = websocket.WebSocketApp(
                self.url,
                on_open=self._on_open,
                on_message=self._on_message,
                on_error=lambda _ws, err: print(f"signaling: {err}"),
                on_close=lambda _ws, *_a: print("signaling: connection closed"),
            )
            self.ws.run_forever()
            if self.stopping:
                break
            GLib.idle_add(self.stop_session, "the server went away")
            time.sleep(RECONNECT_S)

    def _on_open(self, ws):
        print(f"signaling: connected to {self.url}")
        ws.send(json.dumps({"type": "hello", "role": "pi"}))

    def _on_message(self, _ws, message):
        # Hand it to the GLib loop: everything touching the pipeline has to run
        # on one thread, and that thread is the main one.
        GLib.idle_add(self._handle, message)

    def _handle(self, raw):
        try:
            msg = json.loads(raw)
        except ValueError:
            return False
        kind = msg.get("type")

        if kind == "welcome":
            self.my_id = msg["id"]
            print(f"signaling: signed in as pi #{self.my_id}, waiting for a browser")
        elif kind == "request-offer":
            self.start_session(msg["from"])
        elif kind == "answer":
            if self.session and self.session.peer_id == msg["from"]:
                self.session.set_answer(msg["sdp"])
        elif kind == "ice":
            if self.session and self.session.peer_id == msg["from"]:
                self.session.add_ice(msg["candidate"])
        elif kind in ("bye", "peer-left"):
            gone = msg.get("id", msg.get("from"))
            if self.session and self.session.peer_id == gone:
                self.stop_session("the browser left")
        elif kind == "signal-error":
            print(f"signaling: peer #{msg.get('to')} is not connected")
        return False  # idle_add runs this once

    def send(self, obj):
        try:
            self.ws.send(json.dumps(obj))
        except Exception as err:  # the socket can die between two signals
            print(f"signaling: could not send {obj.get('type')}: {err}")

    def start_session(self, peer_id):
        # One browser at a time, and a new request replaces the old session -
        # the same rule server.js uses for uploads, so a browser that vanished
        # without closing cleanly never locks the next one out.
        if self.session:
            self.stop_session(f"replaced by browser #{peer_id}")
        print(f"\nbrowser #{peer_id} asked for media")
        self.session = Session(self, peer_id, self.opts)

    def stop_session(self, why):
        if self.session:
            print(f"session with browser #{self.session.peer_id} ended: {why}")
            self.session.close()
            self.session = None
        return False

    def stop(self):
        self.stopping = True
        self.stop_session("shutting down")
        if self.ws:
            self.ws.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("server", help="the machine running server.js, as IP or IP:PORT")
    parser.add_argument(
        "--video",
        choices=("camera", "test"),
        default="camera",
        help="camera is the real one through libcamera; test is videotestsrc, Steps 1-3",
    )
    parser.add_argument(
        "--size",
        type=parse_size,
        default=None,
        metavar="WIDTHxHEIGHT",
        help=f"picture size (camera {CAMERA_SIZE}, test {TEST_SIZE}; the imx708 is"
        " 16:9, so asking it for 4:3 crops the sides off)",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=DEFAULT_FPS,
        help=f"frames per second (default {DEFAULT_FPS})",
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
        help="full reads the whole sensor and keeps the wide lens wide; auto lets"
        " libcamera choose, which crops to the centre 2/3 at small sizes",
    )
    parser.add_argument(
        "--encoder",
        choices=sorted(ENCODERS),
        default="x264",
        help="x264 is software and always works; v4l2 is the Pi 4's hardware encoder",
    )
    parser.add_argument(
        "--audio",
        choices=("alsa", "test", "none"),
        default="alsa",
        help="alsa is the real microphone; test is a once-a-second tick; none is Step 1",
    )
    parser.add_argument(
        "--mic-device",
        default=DEFAULT_MIC,
        help=f"ALSA capture device for --audio alsa (default {DEFAULT_MIC})",
    )
    parser.add_argument(
        "--speaker-device",
        default=DEFAULT_SPEAKER,
        help=f"where the browser's voice comes out (default {DEFAULT_SPEAKER})",
    )
    parser.add_argument(
        "--no-talkback",
        action="store_true",
        help="do not ask the browser for its microphone; Step 2 again",
    )
    parser.add_argument("--print-sdp", action="store_true", help="dump the offer and the answer")
    parser.add_argument(
        "--pattern",
        default="ball",
        help="--video test only: ball is a dot on black, smpte is colour bars",
    )
    args = parser.parse_args()

    # Resolved here rather than as an argparse default, because the default
    # depends on which source was chosen.
    if args.size is None:
        args.size = parse_size(CAMERA_SIZE if args.video == "camera" else TEST_SIZE)

    server = args.server if ":" in args.server else f"{args.server}:3000"

    Gst.init(None)
    require("webrtcbin", "install gstreamer1.0-plugins-bad")
    require("rtph264pay", "install gstreamer1.0-plugins-good")
    if args.video == "camera":
        require(
            "libcamerasrc",
            "install gstreamer1.0-libcamera, then ./check-camera.sh; or use --video test",
        )
    else:
        require("videotestsrc", "install gstreamer1.0-plugins-base")
    if args.encoder == "x264":
        require("x264enc", "install gstreamer1.0-plugins-ugly, or use --encoder v4l2")
    else:
        require("v4l2h264enc", "no hardware encoder here, use --encoder x264")
    if args.audio != "none":
        require("opusenc", "install gstreamer1.0-plugins-base")
        require("rtpopuspay", "install gstreamer1.0-plugins-good")
        require("audioresample", "install gstreamer1.0-plugins-base")
        if args.audio == "alsa":
            require("alsasrc", "install gstreamer1.0-alsa")
        else:
            require("audiotestsrc", "install gstreamer1.0-plugins-base")
    if not args.no_talkback:
        require("rtpopusdepay", "install gstreamer1.0-plugins-good")
        require("opusdec", "install gstreamer1.0-plugins-base")
        require("alsasink", "install gstreamer1.0-alsa")

    sound = {
        "none": "no audio track (Step 1 again)",
        "test": "a test tick, NOT the microphone",
        "alsa": f"microphone {args.mic_device}",
    }[args.audio]

    width, height = args.size
    picture = "the camera" if args.video == "camera" else f"videotestsrc {args.pattern}"

    client = Client(f"ws://{server}/ws", args)
    client.start()

    print(f"pi webrtc: {picture} at {width}x{height}@{args.fps}, {args.encoder} encoder, {sound}")

    # x264enc on a Cortex-A72 is fine at 640x360 and marginal above it. When it
    # cannot keep up the queue leaks, so the symptom is a jerky picture rather
    # than a growing delay - which hides the cause unless it is said here.
    if args.encoder == "x264" and width * height > 640 * 480:
        print(f"           NOTE: {width}x{height} in software may drop frames; --encoder v4l2")
        print("           is the Pi 4's hardware encoder and costs no CPU at all")

    if args.no_talkback:
        print("           no talkback - the browser is not asked for its microphone")
    else:
        print(f"           talkback out of {args.speaker_device}")
    print(f"           signaling to ws://{server}/ws")

    # One card, two directions, no cancellation yet: the speaker is a few
    # centimetres from the microphones that feed the other direction.
    if not args.no_talkback and args.audio == "alsa" and args.speaker_device == args.mic_device:
        print("\n  NOTE: the microphone and the speaker are the same card, so the Pi will")
        print("        hear itself and can howl. Until Step 4 adds webrtcdsp, either put")
        print("        headphones on the Pi, keep the volume down, or send the voice")
        print("        where the microphone cannot reach it:")
        print("          --speaker-device hw:CARD=Headphones")

    # getUserMedia needs a secure context, and plain http:// to a LAN address is
    # not one. localhost is, which is why this says localhost and not the IP.
    print(f"\nopen http://localhost:{server.split(':')[1]}/webrtc.html on the server")
    print("machine (not the LAN address - see the page if it refuses), then Connect\n")

    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        client.stop()


if __name__ == "__main__":
    main()
