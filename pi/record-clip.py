#!/usr/bin/env python3
"""gst-launch-1.0 -e, plus the one call it cannot make, plus a person detector.

porchlightd records a clip by running this with a pipeline description, and
stops it with SIGINT. SIGINT becomes EOS, EOS flushes the muxer, and the exit
code says whether it got that far - exactly the contract gst-launch -e had, so
the daemon's side of it did not change.

  record-clip.py [--system-clock] [--camera-property NAME=VALUE ...] \
                 [--detect-model M ...] -- mp4mux name=mux ! filesink location=... ...

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

The AAC encoder is checked the same way: one the system lacks is replaced by
one it has (AAC_ENCODERS), because the name comes from a config on /data that
outlives the image it was written for.

--detect-model turns on the person detector. The PIR only wakes the camera; a
motion alert waits until this says whether anybody is there. The pipeline must
then carry an appsink named "detect" on a branch of the camera's tee - the
detector cannot open the camera itself, because only one process can hold it.
It prints exactly one line on stdout, which is the daemon's pipe:

  {"verdict": "person" | "no-person" | "unknown", "why": "...", ...}

"unknown" means it could not judge - no runtime, no model, too dark, too slow,
stopped first - and the daemon alerts on it exactly as it would on "person".
Only a "no-person" earned by looking at enough bright frames silences an
alert. Nothing the detector does can fail the recording: it is a thread that
gets a copy of the frames, and any exception in it becomes "unknown".

Exit codes: 0 the muxer saw EOS and the file is finished, 1 GStreamer reported
an error, 2 the description did not parse.
"""

import argparse
import collections
import importlib
import json
import signal
import sys
import threading
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

# In the order they are tried. ai-edge-litert is what TensorFlow ships the
# interpreter as now; tflite-runtime is the older name, and has no wheels for
# the Python on Debian 13; full TensorFlow works too but takes seconds to import.
RUNTIMES = (
    ("ai_edge_litert.interpreter", "Interpreter"),
    ("tflite_runtime.interpreter", "Interpreter"),
    ("tensorflow.lite", "Interpreter"),
)

# COCO's first class, in the class ids the detection post-processing emits.
PERSON = 0


def note(text):
    print(f"record-clip: {text}", file=sys.stderr, flush=True)


# Every AAC encoder a clip can use, best first. Which exist differs by system:
# the Buildroot image has voaacenc and, since FFmpeg left it, no avenc_aac;
# Raspberry Pi OS has whatever gstreamer1.0-libav and -plugins-bad brought.
AAC_ENCODERS = ("voaacenc", "avenc_aac", "fdkaacenc")


def available_aac(tokens):
    """tokens, with an AAC encoder this system lacks swapped for one it has.

    The daemon names the encoder from its config, and the config lives on
    /data, which an update never touches - so a device whose config says
    avenc_aac keeps saying it after an update that took FFmpeg away, and every
    clip would fail to parse. Swapped here, where GStreamer can be asked.
    """
    present = [name for name in AAC_ENCODERS if Gst.ElementFactory.find(name)]
    result = []
    for token in tokens:
        if token in AAC_ENCODERS and token not in present:
            if not present:
                note(f"no AAC encoder here ({', '.join(AAC_ENCODERS)}); leaving {token}")
            else:
                note(f"{token} is not on this system; recording AAC with {present[0]}")
                token = present[0]
        result.append(token)
    return result


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


def load_interpreter_class():
    tried = []
    for module_name, attribute in RUNTIMES:
        try:
            module = importlib.import_module(module_name)
        except ImportError as error:
            tried.append(f"{module_name} ({error})")
            continue
        return getattr(module, attribute), module_name
    raise RuntimeError("no TFLite runtime: " + "; ".join(tried))


class PersonModel:
    """A TFLite SSD detector with the standard post-processing op.

    COCO SSD MobileNet and EfficientDet-Lite both end in
    TFLite_Detection_PostProcess, whose four outputs are boxes, classes, scores
    and a count, named by suffix. They are found by name rather than by
    position because the position differs between models, and a classes tensor
    read as scores would report a person in every frame.
    """

    def __init__(self, path, size, numpy):
        self.numpy = numpy
        interpreter_class, self.runtime = load_interpreter_class()
        # Two threads: the encoder needs most of a core of its own, and the
        # clip matters more than the speed of the verdict.
        self.interpreter = interpreter_class(model_path=path, num_threads=2)
        self.interpreter.allocate_tensors()

        self.input = self.interpreter.get_input_details()[0]
        shape = tuple(int(n) for n in self.input["shape"])
        if shape[1:3] != (size, size):
            raise ValueError(f"the model wants {shape[2]}x{shape[1]} and the pipeline gives "
                             f"{size}x{size} - set detect.input_size to match")

        by_name = {d["name"]: d["index"] for d in self.interpreter.get_output_details()}
        base = "TFLite_Detection_PostProcess"
        try:
            self.classes = by_name[f"{base}:1"]
            self.scores = by_name[f"{base}:2"]
            self.count = by_name[f"{base}:3"]
        except KeyError:
            raise ValueError("the model's outputs are not TFLite_Detection_PostProcess's: "
                             + ", ".join(sorted(by_name))) from None

    def person_score(self, pixels):
        """The best person score in one frame, 0 to 1."""
        numpy = self.numpy
        dtype = self.input["dtype"]
        if dtype == numpy.uint8:
            tensor = pixels
        elif dtype == numpy.int8:
            tensor = (pixels.astype(numpy.int16) - 128).astype(numpy.int8)
        else:
            tensor = (pixels.astype(numpy.float32) - 127.5) / 127.5
        self.interpreter.set_tensor(self.input["index"], tensor[numpy.newaxis, ...])
        self.interpreter.invoke()

        classes = self.interpreter.get_tensor(self.classes)[0]
        scores = self.interpreter.get_tensor(self.scores)[0]
        count = int(self.interpreter.get_tensor(self.count).flatten()[0])
        best = 0.0
        for i in range(min(count, len(scores))):
            if int(classes[i]) == PERSON:
                best = max(best, float(scores[i]))
        return best


class Detector:
    """Looks at the newest frame it has been given, as fast as it can.

    offer() runs on GStreamer's streaming thread and only stores the frame:
    the newest one wins and older ones are dropped, so a slow model costs
    judged frames and never stalls the pipeline. Everything slow happens on
    this object's own thread.
    """

    # Frames are skipped for this long after the first one arrives, while
    # auto-exposure settles. A camera opened cold delivers a few frames that
    # are far too dark or too bright, and those would read as nobody.
    SETTLE_SECONDS = 0.5
    # Two hits in the last three judged frames. One frame alone is too easy
    # to fool; three in a row costs a second on a slow Pi.
    HITS_NEEDED = 2
    HITS_OF = 3
    REPORT_SECONDS = 2.0

    def __init__(self, options, started):
        self.options = options
        self.started = started
        self.deadline = started + options.detect_window
        self.cond = threading.Condition()
        self.frame = None
        self.stopping = False
        self.finished = False
        self.offered = 0
        self.settling = 0
        self.dark = 0
        self.judged = 0
        self.infer_seconds = 0.0
        self.best = 0.0
        self.thread = threading.Thread(target=self.run, name="detector", daemon=True)

    def start(self):
        self.thread.start()

    def offer(self, data):
        with self.cond:
            if self.finished:
                return
            self.offered += 1
            self.frame = data
            self.cond.notify()

    def finish(self, why):
        """The recording is over. Says unknown if nothing has been said yet."""
        with self.cond:
            self.stopping = True
            self.cond.notify()
        # Long enough for an inference already running to end, and no longer:
        # the daemon is waiting on this process's exit.
        self.thread.join(timeout=1.0)
        self.say("unknown", why)

    def say(self, verdict, why):
        with self.cond:
            if self.finished:
                return
            self.finished = True
        line = {
            "verdict": verdict,
            "why": why,
            "ms": int((time.monotonic() - self.started) * 1000),
            "frames": self.offered,
            "judged": self.judged,
            "dark": self.dark,
            "best": round(self.best, 3),
        }
        note(f"detect: {verdict} after {line['ms']} ms - {why}")
        try:
            sys.stdout.write(json.dumps(line) + "\n")
            sys.stdout.flush()
        except OSError as error:
            note(f"detect: could not tell the daemon: {error}")

    def run(self):
        try:
            self.detect()
        except Exception as error:  # noqa: BLE001 - every failure means "could not judge"
            self.say("unknown", f"the detector failed: {error!r}")

    def detect(self):
        import numpy  # here, so a recording without detection never pays for it

        loading = time.monotonic()
        model = PersonModel(self.options.detect_model, self.options.detect_size, numpy)
        note(f"detect: model loaded with {model.runtime} in "
             f"{int((time.monotonic() - loading) * 1000)} ms")

        size = self.options.detect_size
        threshold = self.options.detect_min_score / 100.0
        hits = collections.deque(maxlen=self.HITS_OF)
        first_frame_at = None
        last_report = time.monotonic()

        while True:
            with self.cond:
                while self.frame is None and not self.stopping:
                    remaining = self.deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    self.cond.wait(remaining)
                frame, self.frame = self.frame, None
                stopping = self.stopping

            now = time.monotonic()
            if stopping:
                self.say("unknown", "the recording ended before a verdict")
                return
            if now >= self.deadline:
                self.conclude()
                return
            if frame is None:
                continue

            if first_frame_at is None:
                first_frame_at = now
            if now - first_frame_at < self.SETTLE_SECONDS:
                self.settling += 1
                continue

            pixels = numpy.frombuffer(frame, dtype=numpy.uint8)
            if pixels.size != size * size * 3:
                raise ValueError(f"a frame of {pixels.size} bytes, expected {size}x{size} RGB")
            # Not true luma, but the mean of R, G and B is close enough to
            # tell a lit porch from a dark one, and costs nothing.
            if float(pixels.mean()) < self.options.detect_min_luma:
                self.dark += 1
                continue

            began = time.monotonic()
            score = model.person_score(pixels.reshape(size, size, 3))
            self.infer_seconds += time.monotonic() - began
            self.judged += 1
            self.best = max(self.best, score)
            hits.append(score >= threshold)
            if sum(hits) >= self.HITS_NEEDED:
                self.say("person", f"score {score:.2f}")
                return

            if now - last_report >= self.REPORT_SECONDS:
                last_report = now
                note(f"detect: {self.offered} frames in, {self.judged} judged, {self.dark} dark, "
                     f"{self.mean_infer_ms()} ms each, best {self.best:.2f}")

    def conclude(self):
        """The window is over and nobody was seen. Is that worth believing?"""
        window = self.options.detect_window
        if self.judged >= self.options.detect_min_frames:
            self.say("no-person", f"{self.judged} frames judged, best score {self.best:.2f}")
        elif self.offered == 0:
            self.say("unknown", "no frames reached the detector")
        elif self.dark > self.judged:
            self.say("unknown", f"too dark to judge: {self.dark} dark frames, {self.judged} bright")
        else:
            self.say("unknown", f"only {self.judged} frames judged in {window:g}s - too slow "
                                f"({self.mean_infer_ms()} ms each)")

    def mean_infer_ms(self):
        return int(self.infer_seconds * 1000 / self.judged) if self.judged else 0


def attach_detector(pipeline, options, started):
    """Wires the appsink to a Detector, or says unknown at once if it cannot."""
    detector = Detector(options, started)
    sink = pipeline.get_by_name("detect")
    if sink is None:
        detector.say("unknown", "the pipeline has no appsink named detect")
        return detector

    def on_sample(appsink):
        # Never raise out of here: this is GStreamer's thread, and a Python
        # exception in it would end the stream the clip is being recorded from.
        try:
            sample = appsink.emit("pull-sample")
            if sample is not None:
                buffer = sample.get_buffer()
                ok, info = buffer.map(Gst.MapFlags.READ)
                if ok:
                    try:
                        # Copied, because the buffer goes back to its pool the
                        # moment it is unmapped and the detector reads it later.
                        detector.offer(bytes(info.data))
                    finally:
                        buffer.unmap(info)
        except Exception as error:  # noqa: BLE001
            note(f"detect: a frame was lost: {error!r}")
        return Gst.FlowReturn.OK

    sink.set_property("emit-signals", True)
    sink.connect("new-sample", on_sample)
    # Before PLAYING, so the model loads while the camera is still starting.
    detector.start()
    return detector


def bench(options):
    """The detector with no camera: does the runtime load, and how fast is it?

    The detector's half on its own, the way --check is the call's tokens on
    their own. A runtime that will not import otherwise shows up only as every
    motion coming back "unknown". The timing is of an idle Pi; a recording's
    x264 encoder takes most of a core, so expect slower inside a real clip.
    """
    import numpy

    loading = time.monotonic()
    try:
        model = PersonModel(options.detect_model, options.detect_size, numpy)
    except Exception as error:  # noqa: BLE001 - reported, not raised
        note(f"bench: the model will not load: {error}")
        return 1
    note(f"bench: {options.detect_model} loaded with {model.runtime} in "
         f"{int((time.monotonic() - loading) * 1000)} ms")

    size = options.detect_size
    # Noise rather than black, so the model does real work on every frame.
    frame = numpy.random.default_rng(0).integers(0, 256, (size, size, 3), dtype=numpy.uint8)
    model.person_score(frame)  # the first inference pays for setup; not counted
    runs = 20
    began = time.monotonic()
    for _ in range(runs):
        score = model.person_score(frame)
    each = (time.monotonic() - began) * 1000 / runs
    note(f"bench: {each:.0f} ms a frame over {runs} frames, so about {1000 / each:.1f} fps "
         f"(person score on noise: {score:.2f}, which should be near 0)")
    return 0


def main():
    # The window is measured from here, the nearest this process gets to the
    # moment the PIR fired. Importing GStreamer has already happened.
    started = time.monotonic()

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--system-clock", action="store_true",
                        help="pin the pipeline to the system monotonic clock")
    parser.add_argument("--camera-property", action="append", default=[],
                        metavar="NAME=VALUE",
                        help="set on the element named camera if it has it (repeatable)")
    detect = parser.add_argument_group("person detection")
    detect.add_argument("--detect-model", help="a TFLite SSD detector; turns detection on")
    detect.add_argument("--detect-size", type=int, default=300,
                        help="the square input the appsink delivers (default 300)")
    detect.add_argument("--detect-window", type=float, default=8.0,
                        help="seconds to look before saying no-person (default 8)")
    detect.add_argument("--detect-min-score", type=int, default=50,
                        help="percent a person must score to count (default 50)")
    detect.add_argument("--detect-min-luma", type=int, default=40,
                        help="mean brightness out of 255 below which a frame is too dark")
    detect.add_argument("--detect-min-frames", type=int, default=8,
                        help="bright frames judged before no-person is believed")
    detect.add_argument("--detect-bench", action="store_true",
                        help="no camera, no pipeline: load the model, time it, exit")
    parser.add_argument("pipeline", nargs=argparse.REMAINDER,
                        help="the pipeline, as gst-launch-1.0 would take it, after --")
    options = parser.parse_args()
    if options.detect_bench:
        if not options.detect_model:
            parser.error("--detect-bench needs --detect-model")
        return bench(options)
    tokens = options.pipeline[1:] if options.pipeline[:1] == ["--"] else options.pipeline
    if not tokens:
        parser.error("no pipeline given")

    Gst.init(None)
    tokens = available_aac(tokens)
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

    detector = attach_detector(pipeline, options, started) if options.detect_model else None

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
        if detector is not None:
            detector.finish("the pipeline would not start")
        pipeline.set_state(Gst.State.NULL)
        return 1
    loop.run()
    # Before the pipeline is torn down, and before the exit the daemon is
    # waiting on: the verdict has to be in the pipe ahead of the pidfd.
    if detector is not None:
        detector.finish("the recording ended before a verdict")
    pipeline.set_state(Gst.State.NULL)
    return outcome["code"]


if __name__ == "__main__":
    sys.exit(main())
