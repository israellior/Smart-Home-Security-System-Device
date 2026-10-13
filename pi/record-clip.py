#!/usr/bin/env python3
"""gst-launch-1.0 -e, plus the one call it cannot make.

porchlightd records a clip by running this with a pipeline description, and
stops it with SIGINT. SIGINT becomes EOS, EOS flushes the muxer, and the exit
code says whether it got that far - exactly the contract gst-launch -e had, so
the daemon's side of it did not change.

  record-clip.py [--system-clock] [--camera-property NAME=VALUE ...] \
                 -- mp4mux name=mux ! filesink location=... ...

--system-clock is why this file exists. alsasrc offers the sound card as the
pipeline clock and libcamerasrc stamps its frames against the system monotonic
clock whatever the pipeline chose, so with both in one pipeline the video track
comes out as ten frames spread over two hundred hours. The fix is
pipeline.use_clock(), and there is no switch for it on a gst-launch command
line. alsasrc provide-clock=false looked like the equivalent and is not: the
drill on the Pi (check-clip.py, now in the git history) recorded it broken,
and this path whole.

--camera-property is the other thing a launch string cannot do safely. The
camera's properties differ between libcamera builds - sensor-config exists only
in Raspberry Pi's - and one the element lacks fails the whole parse, so a clip
records nothing. Each is set on the element named "camera" only if it has it.

Exit codes: 0 the muxer saw EOS and the file is finished, 1 GStreamer reported
an error, 2 the description did not parse.
"""

import argparse
import signal
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402


def note(text):
    print(f"record-clip: {text}", file=sys.stderr, flush=True)


def set_camera_properties(pipeline, settings):
    """Set NAME=VALUE on the element named "camera", each only if it exists.

    Gst.util_set_object_arg parses the value as the property's own type - an
    enum for af-mode, a structure for sensor-config - which is what the
    launch-string parser did with the same text.
    """
    camera = pipeline.get_by_name("camera")
    if camera is None:
        note("no element named camera; camera properties not set")
        return
    have = {spec.name for spec in camera.list_properties()}
    for setting in settings:
        name, _, value = setting.partition("=")
        if name not in have:
            note(f"camera: this libcamerasrc has no {name}; recording without it")
            continue
        Gst.util_set_object_arg(camera, name, value)
        note(f"camera: {name} set")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--system-clock", action="store_true",
                        help="pin the pipeline to the system monotonic clock")
    parser.add_argument("--camera-property", action="append", default=[],
                        metavar="NAME=VALUE",
                        help="set on the element named camera if it has it (repeatable)")
    parser.add_argument("pipeline", nargs=argparse.REMAINDER,
                        help="the pipeline, as gst-launch-1.0 would take it, after --")
    options = parser.parse_args()
    tokens = options.pipeline[1:] if options.pipeline[:1] == ["--"] else options.pipeline
    if not tokens:
        parser.error("no pipeline given")

    Gst.init(None)
    try:
        # parse_launchv is what gst-launch itself calls on its argv, so the
        # daemon's quoting - device="hw:CARD=..." - means here exactly what it
        # meant there.
        pipeline = Gst.parse_launchv(tokens)
    except GLib.Error as error:
        note(f"the pipeline does not parse: {error.message}")
        return 2
    if options.camera_property:
        set_camera_properties(pipeline, options.camera_property)
    if options.system_clock:
        pipeline.use_clock(Gst.SystemClock.obtain())

    loop = GLib.MainLoop()
    outcome = {"code": 1}
    stopping = {"sent": False}

    def on_message(_bus, message):
        if message.type == Gst.MessageType.EOS:
            outcome["code"] = 0
            loop.quit()
        elif message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            source = message.src.get_name() if message.src else "?"
            note(f"error from {source}: {error.message}")
            if debug:
                note(debug)
            loop.quit()
        return True

    def on_stop():
        # Once only. The daemon sends one SIGINT and then waits; a second EOS
        # would say nothing new, and it is SIGKILL that follows if this hangs.
        if not stopping["sent"]:
            stopping["sent"] = True
            pipeline.send_event(Gst.Event.new_eos())
        return True

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", on_message)
    # Through GLib rather than the signal module, so the handler runs on the
    # main loop between messages instead of interrupting it mid-dispatch.
    GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGINT, on_stop)
    GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGTERM, on_stop)

    if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
        # Most often the camera or the card held by something else. Waiting in
        # the loop would only turn that into a SIGKILL five seconds later.
        message = bus.timed_pop_filtered(0, Gst.MessageType.ERROR)
        if message is not None:
            on_message(bus, message)
        note("the pipeline would not start")
        pipeline.set_state(Gst.State.NULL)
        return 1
    loop.run()
    pipeline.set_state(Gst.State.NULL)
    return outcome["code"]


if __name__ == "__main__":
    sys.exit(main())
