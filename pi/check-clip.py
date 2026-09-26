#!/usr/bin/env python3
"""Why a clip claims to be two hundred hours long.

Two halves, both read-only as far as the doorbell is concerned:

  check-clip.py FILE.mp4 [...]    read the timing out of clips that exist
  check-clip.py --drill           record six seconds several ways, then read them

A player takes a clip's length from the movie header, and the movie header is
the longest track *including any gap in front of it*. So a video track and an
audio track stamped against two different zero points make a clip whose length
is the distance between those zero points, and it plays for its real length and
then sits there. The edit list is where that gap is written down, and it is
what this prints - per track, next to how much media the track really holds.

The drill takes one half away at a time, because a clip can only say *that* a
track is offset, not which clock put it there. Stop the daemon first: the camera
and the card each go to one process at a time.

  sudo systemctl stop porchlightd
  python3 check-clip.py --drill
  sudo systemctl start porchlightd

Nothing is installed, and the drill writes only under /tmp.
"""

import argparse
import json
import os
import signal
import struct
import subprocess
import sys
import time

CONFIG = "/etc/porchlight/porchlightd.json"
CONTAINERS = {b"moov", b"trak", b"edts", b"mdia", b"minf", b"stbl"}


# ------------------------------------------------------------------ reading MP4

def boxes(data, start, end):
    offset = start
    while offset + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, offset)
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, offset + 8)[0]
            header = 16
        elif size == 0:
            size = end - offset
        if size < header:
            return
        yield kind, offset + header, offset + size
        offset += size


def find(data, start, end, path):
    """Every box at the end of path, e.g. [b"trak", b"mdia", b"mdhd"]."""
    for kind, body, stop in boxes(data, start, end):
        if kind != path[0]:
            continue
        if len(path) == 1:
            yield body, stop
        elif kind in CONTAINERS:
            yield from find(data, body, stop, path[1:])


def first(data, start, end, path):
    return next(find(data, start, end, path), None)


def full_box(data, body):
    return data[body], body + 4


def read_mvhd(data, body):
    version, at = full_box(data, body)
    if version == 1:
        return struct.unpack_from(">IQ", data, at + 16)
    return struct.unpack_from(">II", data, at + 8)


def read_mdhd(data, body):
    return read_mvhd(data, body)          # same layout up to the duration


def read_elst(data, body):
    version, at = full_box(data, body)
    count = struct.unpack_from(">I", data, at)[0]
    at += 4
    entries = []
    for _ in range(count):
        if version == 1:
            segment, media = struct.unpack_from(">Qq", data, at)
            at += 20
        else:
            segment, media = struct.unpack_from(">Ii", data, at)
            at += 12
        entries.append((segment, media))
    return entries


def read_stts(data, body):
    _, at = full_box(data, body)
    count = struct.unpack_from(">I", data, at)[0]
    samples = total = 0
    for i in range(count):
        n, delta = struct.unpack_from(">II", data, at + 4 + 8 * i)
        samples += n
        total += n * delta
    return samples, total


def hms(seconds):
    if seconds < 120:
        return f"{seconds:.2f}s"
    whole = int(seconds)
    return f"{whole // 3600}:{whole // 60 % 60:02d}:{whole % 60:02d} ({seconds:.0f}s)"


def inspect(path):
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"\n{path}  ({len(data)} bytes)")
    moov = first(data, 0, len(data), [b"moov"])
    if moov is None:
        print("  no moov box: the muxer never finished. That is a missing EOS,")
        print("  not a timing fault, and it does not play at all.")
        return None
    mvhd = first(data, *moov, [b"mvhd"])
    movie_scale, movie_duration = read_mvhd(data, mvhd[0])
    movie = movie_duration / movie_scale
    print(f"  movie        {hms(movie)}   <- what a player shows")

    longest_media = 0.0
    worst_gap = 0.0
    for trak in find(data, *moov, [b"trak"]):
        hdlr = first(data, *trak, [b"mdia", b"hdlr"])
        kind = data[hdlr[0] + 8:hdlr[0] + 12].decode("ascii", "replace")
        name = {"vide": "video", "soun": "audio"}.get(kind, kind)
        mdhd = first(data, *trak, [b"mdia", b"mdhd"])
        scale, _ = read_mdhd(data, mdhd[0])
        stts = first(data, *trak, [b"mdia", b"minf", b"stbl", b"stts"])
        samples, ticks = read_stts(data, stts[0]) if stts else (0, 0)
        media = ticks / scale if scale else 0.0
        longest_media = max(longest_media, media)
        rate = f", {samples / media:.1f}/s" if media else ""
        print(f"  {name:<12} {hms(media)} of media, {samples} samples{rate}")

        elst = first(data, *trak, [b"edts", b"elst"])
        if elst is None:
            continue
        for segment, media_time in read_elst(data, elst[0]):
            length = segment / movie_scale
            if media_time == -1:
                worst_gap = max(worst_gap, length)
                print(f"  {'':<12} starts after a gap of {hms(length)}")
            elif media_time:
                print(f"  {'':<12} skips its first {hms(media_time / scale)}")

    if movie > longest_media + 1.0:
        print(f"  VERDICT      the movie is {hms(movie - longest_media)} longer than its")
        print("               longest track. One track is stamped against a different")
        print("               zero than the other, and the gap above is the distance.")
    else:
        print("  VERDICT      consistent: the movie is as long as its media.")
    return movie, longest_media, worst_gap


# ------------------------------------------------------------------ recording

def recorder_config():
    try:
        with open(CONFIG) as handle:
            return json.load(handle).get("recorder", {})
    except (OSError, ValueError):
        return {}


def video_branch(cfg):
    # The same tokens porchlightd's build_pipeline() emits, in the same order.
    return ["libcamerasrc", "af-mode=continuous",
            'sensor-config="sensor/config,width=2304,height=1296,depth=10"', "!",
            f"video/x-raw,format=I420,width={cfg['width']},height={cfg['height']},"
            f"framerate={cfg['fps']}/1", "!",
            "queue", "!", "videoconvert", "!",
            "x264enc", "tune=zerolatency", "speed-preset=ultrafast",
            f"bitrate={cfg['bitrate']}", f"key-int-max={cfg['fps']}", "!",
            "video/x-h264,profile=constrained-baseline", "!",
            "h264parse", "!", "queue", "!", "mux."]


def audio_branch(cfg, *extra, test=False):
    source = (["audiotestsrc", "is-live=true"] if test else
              ["alsasrc", f'device="{cfg["device"]}"', *extra,
               "buffer-time=200000", "latency-time=20000"])
    return [*source, "!", "queue", "!", "audioconvert", "!", "audioresample", "!",
            "audio/x-raw,rate=48000,channels=1", "!", cfg["aac"], "!",
            "aacparse", "!", "queue", "!", "mux."]


def record_pinned(description, output, seconds):
    """The same pipeline in-process, with the clock pinned the way webrtc-video.py
    pins it. gst-launch has no switch for this, which is the whole question."""
    import gi
    gi.require_version("Gst", "1.0")
    from gi.repository import Gst
    Gst.init(None)
    pipeline = Gst.parse_launch(description)
    pipeline.use_clock(Gst.SystemClock.obtain())
    pipeline.set_state(Gst.State.PLAYING)
    time.sleep(seconds)
    pipeline.send_event(Gst.Event.new_eos())
    message = pipeline.get_bus().timed_pop_filtered(
        15 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipeline.set_state(Gst.State.NULL)
    if message is None:
        print("  no EOS within 15s")
        return False
    if message.type == Gst.MessageType.ERROR:
        error, _ = message.parse_error()
        print(f"  error: {error.message}")
        return False
    return True


def record(label, branches, output, seconds, pinned=False):
    args = ["gst-launch-1.0", "-e", "mp4mux", "name=mux", "faststart=true", "!",
            "filesink", f'location="{output}"']
    for branch in branches:
        args += branch
    print(f"\n== {label}")
    try:
        os.unlink(output)
    except FileNotFoundError:
        pass
    if pinned:
        if not record_pinned(" ".join(args[2:]), output, seconds):
            return None
        return inspect(output)
    child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
    time.sleep(seconds)
    child.send_signal(signal.SIGINT)
    try:
        out, _ = child.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill()
        out, _ = child.communicate()
        print("  no EOS within 15s; killed")
    if child.returncode != 0 or not os.path.exists(output):
        print(f"  gst-launch exited {child.returncode}. Its last lines:")
        for line in out.strip().splitlines()[-6:]:
            print(f"    {line}")
        return None
    return inspect(output)


def drill(seconds, only=""):
    raw = recorder_config()
    cfg = {"width": raw.get("width", 640), "height": raw.get("height", 360),
           "fps": raw.get("fps", 30), "bitrate": raw.get("video_bitrate_kbps", 2000),
           "device": raw.get("audio_device", "hw:CARD=wm8960soundcard"),
           "aac": raw.get("aac_element", "avenc_aac")}
    print(f"recording {seconds}s each, {cfg['width']}x{cfg['height']}@{cfg['fps']}, "
          f"{cfg['device']}, {cfg['aac']}")
    video = video_branch(cfg)
    runs = [
        ("A  exactly what porchlightd records: camera + card, provide-clock=false",
         [video, audio_branch(cfg, "provide-clock=false")]),
        ("B  camera alone", [video]),
        ("C  card alone, provide-clock=false", [audio_branch(cfg, "provide-clock=false")]),
        ("D  card alone, as it comes", [audio_branch(cfg)]),
        ("E  camera + card, provide-clock=false, the card's own timestamps off",
         [video, audio_branch(cfg, "provide-clock=false", "use-driver-timestamps=false")]),
        ("F  camera + a test tone instead of the card",
         [video, audio_branch(cfg, test=True)]),
        ("G  as A, but in Python with the clock pinned, as webrtc-video.py does",
         [video, audio_branch(cfg)], True),
        ("H  as A, with the card not slaving itself to the pipeline clock",
         [video, audio_branch(cfg, "provide-clock=false", "slave-method=none")]),
        ("I  as A, with the call's 40 ms card buffer instead of 200 ms",
         [video, ["alsasrc", f'device="{cfg["device"]}"', "provide-clock=false",
                  "buffer-time=40000", "latency-time=10000",
                  *audio_branch(cfg)[audio_branch(cfg).index("!"):]]]),
    ]
    runs = [run if len(run) == 3 else (*run, False) for run in runs]
    if only:
        runs = [run for run in runs if run[0][0] in only.upper()]
    results = {}
    for index, (label, branches, pinned) in enumerate(runs):
        results[label[0]] = record(label, branches, f"/tmp/check-clip-{label[0]}.mp4",
                                   seconds, pinned)

    print("\n== Summary: what a player would show for a clip of", f"{seconds}s")
    for label, _, _ in runs:
        result = results[label[0]]
        shown = "failed to record" if result is None else hms(result[0])
        print(f"  {label[:62]:<62} {shown}")
    print("\nA clip of the right length with ~30 video frames a second is a pass;")
    print("paste all of this back rather than a summary of it.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*", help="clips to read")
    parser.add_argument("--drill", action="store_true",
                        help="record six seconds several ways and read each")
    parser.add_argument("--seconds", type=int, default=6)
    parser.add_argument("--only", default="",
                        help="run only these drill variants, e.g. --only AGHI")
    options = parser.parse_args()
    if not options.drill and not options.files:
        parser.error("give clips to read, or --drill")
    for path in options.files:
        inspect(path)
    if options.drill:
        drill(options.seconds, options.only)


if __name__ == "__main__":
    sys.exit(main())
