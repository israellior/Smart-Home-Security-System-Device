#!/usr/bin/env python3
"""Faces on the doorbell itself: find them, straighten them, and turn each into
a face code that can be compared with another.

Three steps, each its own small piece:

  find      BlazeFace full-range (MediaPipe, Apache 2.0), 192x192 in: a box and
            six points per face - eyes, nose tip, mouth centre, ears.
  straighten  a similarity transform (turn, scale, shift - no shear) that puts
            the eyes, nose and mouth where the code model was trained to see
            them, and a 112x112 crop through it. numpy, not OpenCV: the image
            has no OpenCV, and this is twenty lines.
  code      SFace (OpenCV zoo, Apache 2.0), converted once from ONNX to TFLite
            so it runs on the same runtime as the person detector: 128 numbers,
            scaled to length 1, so comparing two is one dot product.

Two codes of the same person are close; of different people, far apart.
"Close" is a cosine similarity, 1.0 for identical. OpenCV's own threshold for
SFace is 0.363, measured on LFW; this doorbell's has to be measured at this
door (step 5 of the plan), not borrowed.

Codes are only comparable when the same model made them, in the same way, so
the doorbell makes every code itself - from the visitor and from the photos
the app sends alike.

Pictures are read as PPM (P6) because this image's Python has neither zlib nor
a JPEG decoder; the recorder will hand over frames from memory instead.

    porchlight-faces.py find IMAGE.ppm [--crops DIR]
    porchlight-faces.py compare IMAGE.ppm IMAGE.ppm ...
    porchlight-faces.py bench
"""

import argparse
import importlib
import os
import sys
import time

import numpy as np

MODEL_DIR = "/usr/local/share/porchlight"
FIND_MODEL = os.path.join(MODEL_DIR, "face_detection_full_range.tflite")
CODE_MODEL = os.path.join(MODEL_DIR, "face_code.tflite")

# Where SFace expects the eyes, nose and mouth in its 112x112 input - the
# template OpenCV's FaceRecognizerSF::alignCrop uses. BlazeFace gives the mouth
# centre rather than its corners, so the mouth is the corners' midpoint.
TEMPLATE = np.array([
    [38.2946, 51.6963],   # the person's right eye, on the left of the picture
    [73.5318, 51.5014],   # left eye
    [56.0252, 71.7366],   # nose tip
    [56.1396, 92.2848],   # mouth centre
], np.float64)
CODE_SIZE = 112

RUNTIMES = (
    ("ai_edge_litert.interpreter", "Interpreter"),
    ("tflite_runtime.interpreter", "Interpreter"),
    ("tensorflow.lite", "Interpreter"),
)


def interpreter(path, threads=2):
    tried = []
    for module_name, attribute in RUNTIMES:
        try:
            module = importlib.import_module(module_name)
        except ImportError as error:
            tried.append(f"{module_name} ({error})")
            continue
        found = getattr(module, attribute)(model_path=path, num_threads=threads)
        found.allocate_tensors()
        return found
    raise RuntimeError("no TFLite runtime: " + "; ".join(tried))


def read_ppm(path):
    """RGB as an (h, w, 3) uint8 array, from a binary PPM."""
    with open(path, "rb") as f:
        data = f.read()
    fields, at = [], 0
    while len(fields) < 4:  # magic, width, height, maxval - any whitespace between
        while data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b"#":
            at = data.index(b"\n", at)
            continue
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit binary PPM")
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data, np.uint8, w * h * 3, at + 1).reshape(h, w, 3)


def write_ppm(path, rgb):
    with open(path, "wb") as f:
        f.write(b"P6 %d %d 255\n" % (rgb.shape[1], rgb.shape[0]))
        f.write(np.ascontiguousarray(rgb).tobytes())


def resize(rgb, out_w, out_h):
    """Bilinear, in numpy. Shrinking by more than 2 averages first, so detail
    finer than the output's pixels does not alias into it."""
    h, w = rgb.shape[:2]
    k = int(min(w / out_w, h / out_h) // 2)
    if k >= 2:
        h2, w2 = h // k * k, w // k * k
        rgb = rgb[:h2, :w2].reshape(h2 // k, k, w2 // k, k, 3).mean(axis=(1, 3))
        h, w = rgb.shape[:2]
    return sample(rgb, *np.meshgrid((np.arange(out_w) + 0.5) * w / out_w - 0.5,
                                    (np.arange(out_h) + 0.5) * h / out_h - 0.5))


def sample(rgb, xs, ys):
    """Bilinear lookup of rgb at float coordinates; outside the picture is black."""
    h, w = rgb.shape[:2]
    x0, y0 = np.floor(xs).astype(int), np.floor(ys).astype(int)
    fx, fy = (xs - x0)[..., None], (ys - y0)[..., None]
    out = np.zeros(xs.shape + (3,), np.float32)
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            yy, xx = y0 + dy, x0 + dx
            inside = ((xx >= 0) & (xx < w) & (yy >= 0) & (yy < h))[..., None]
            px = rgb[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)].astype(np.float32)
            out += np.where(inside, px, 0) * wx * wy
    return out


class FaceFinder:
    """BlazeFace full-range: faces up to about five metres in its 192x192 input.

    The outputs are 2304 candidate boxes, one per anchor on a 48x48 grid, as
    offsets from that anchor, and a score for each. The anchor layout and the
    decoding are MediaPipe's own (face_detection_full_range.pbtxt): one layer,
    stride 4, fixed anchor size, coordinates scaled by 192, x before y.
    """

    SIZE = 192
    MIN_SCORE = 0.6
    OVERLAP = 0.3  # boxes overlapping more than this are the same face

    def __init__(self, path=FIND_MODEL):
        self.model = interpreter(path)
        self.input = self.model.get_input_details()[0]
        outputs = self.model.get_output_details()
        # Told apart by shape, not position: the order is not part of the model.
        self.boxes = next(o["index"] for o in outputs if o["shape"][-1] == 16)
        self.scores = next(o["index"] for o in outputs if o["shape"][-1] == 1)
        cells = self.SIZE // 4
        grid = (np.arange(cells) + 0.5) / cells
        ax, ay = np.meshgrid(grid, grid)
        self.anchors = np.stack([ax.ravel(), ay.ravel()], axis=1)

    def find(self, rgb):
        """Faces in one picture, as dicts with score, box and points in its pixels.

        The picture is letterboxed to a square first - BlazeFace was trained on
        faces at their real proportions - and everything is mapped back."""
        h, w = rgb.shape[:2]
        side = max(w, h)
        scale = self.SIZE / side
        inner_w, inner_h = round(w * scale), round(h * scale)
        square = np.zeros((self.SIZE, self.SIZE, 3), np.float32)
        top, left = (self.SIZE - inner_h) // 2, (self.SIZE - inner_w) // 2
        square[top:top + inner_h, left:left + inner_w] = resize(rgb, inner_w, inner_h)

        tensor = (square / 127.5 - 1.0)[None].astype(np.float32)
        self.model.set_tensor(self.input["index"], tensor)
        self.model.invoke()
        raw = self.model.get_tensor(self.boxes)[0]
        logits = np.clip(self.model.get_tensor(self.scores)[0, :, 0], -100, 100)
        scores = 1 / (1 + np.exp(-logits))

        keep = np.flatnonzero(scores >= self.MIN_SCORE)
        faces = []
        for i in keep[np.argsort(-scores[keep])]:
            r, (ax, ay) = raw[i] / self.SIZE, self.anchors[i]
            cx, cy, bw, bh = r[0] + ax, r[1] + ay, r[2], r[3]
            points = r[4:16].reshape(6, 2) + (ax, ay)
            # From the 192 square back to the picture's own pixels.
            to_px = lambda x, y: ((x * self.SIZE - left) / scale, (y * self.SIZE - top) / scale)
            x0, y0 = to_px(cx - bw / 2, cy - bh / 2)
            x1, y1 = to_px(cx + bw / 2, cy + bh / 2)
            box = (x0, y0, x1 - x0, y1 - y0)
            if any(overlap(box, f["box"]) > self.OVERLAP for f in faces):
                continue
            faces.append({
                "score": float(scores[i]),
                "box": box,
                "points": np.array([to_px(px, py) for px, py in points]),
            })
        return faces


def overlap(a, b):
    """Intersection over union of two (x, y, w, h) boxes."""
    ix = max(0.0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


def similarity(src, dst):
    """The turn, uniform scale and shift that best carries src onto dst
    (Umeyama's method, least squares), as a 2x3 matrix."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    s, d = src - mu_s, dst - mu_d
    u, sig, vt = np.linalg.svd(d.T @ s / len(src))
    flip = np.diag([1.0, np.sign(np.linalg.det(u @ vt))])
    rot = u @ flip @ vt
    scale = np.trace(np.diag(sig) @ flip) / (s ** 2).sum(1).mean()
    m = np.zeros((2, 3))
    m[:, :2] = scale * rot
    m[:, 2] = mu_d - scale * rot @ mu_s
    return m


def straighten(rgb, face):
    """The face as SFace wants it: 112x112, eyes level and where it expects."""
    m = similarity(face["points"][:4], TEMPLATE)
    # Each output pixel looks up where it came from: the inverse transform.
    inv = np.linalg.inv(np.vstack([m, [0, 0, 1]]))[:2]
    gx, gy = np.meshgrid(np.arange(CODE_SIZE, dtype=np.float64), np.arange(CODE_SIZE, dtype=np.float64))
    xs = inv[0, 0] * gx + inv[0, 1] * gy + inv[0, 2]
    ys = inv[1, 0] * gx + inv[1, 1] * gy + inv[1, 2]
    return np.clip(sample(rgb, xs, ys), 0, 255).astype(np.uint8)


class FaceCoder:
    """SFace: a straightened 112x112 face in, 128 numbers out, length 1.

    The ONNX original takes RGB in 0-255, channels first; the conversion to
    TFLite turns that to channels last, and the layout is read off the model
    rather than assumed."""

    def __init__(self, path=CODE_MODEL):
        self.model = interpreter(path)
        self.input = self.model.get_input_details()[0]
        self.output = self.model.get_output_details()[0]["index"]
        self.channels_first = int(self.input["shape"][1]) == 3

    def code(self, face112):
        tensor = face112.astype(np.float32)[None]
        if self.channels_first:
            tensor = tensor.transpose(0, 3, 1, 2)
        self.model.set_tensor(self.input["index"], np.ascontiguousarray(tensor))
        self.model.invoke()
        vec = self.model.get_tensor(self.output)[0].astype(np.float64)
        return vec / np.linalg.norm(vec)


def cosine(a, b):
    return float(np.dot(a, b))


def faces_in(path, finder, coder):
    rgb = read_ppm(path)
    out = []
    for face in finder.find(rgb):
        crop = straighten(rgb, face)
        out.append((face, crop, coder.code(crop)))
    return rgb, out


def cmd_find(options):
    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
    for path in options.images:
        began = time.monotonic()
        _, found = faces_in(path, finder, coder)
        ms = int((time.monotonic() - began) * 1000)
        print(f"{path}: {len(found)} face(s) in {ms} ms")
        for n, (face, crop, _) in enumerate(found):
            x, y, w, h = face["box"]
            print(f"  face {n}: score {face['score']:.2f}, box {w:.0f}x{h:.0f} at ({x:.0f},{y:.0f})")
            if options.crops:
                os.makedirs(options.crops, exist_ok=True)
                name = os.path.splitext(os.path.basename(path))[0]
                write_ppm(os.path.join(options.crops, f"{name}-face{n}.ppm"), crop)


def cmd_compare(options):
    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
    labels, codes = [], []
    for path in options.images:
        _, found = faces_in(path, finder, coder)
        name = os.path.splitext(os.path.basename(path))[0]
        for n, (face, _, code) in enumerate(found):
            labels.append(f"{name}#{n}")
            codes.append(code)
    width = max(len(label) for label in labels) if labels else 0
    print(" " * width + "".join(f"{n:>7d}" for n in range(len(labels))))
    for i, (label, a) in enumerate(zip(labels, codes)):
        print(f"{label:>{width}}" + "".join(f"{cosine(a, b):7.2f}" for b in codes) + f"   {i}")


def cmd_bench(options):
    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
    rng = np.random.default_rng(0)
    frame = rng.integers(0, 256, (1296, 2304, 3), dtype=np.uint8)
    face = {"points": TEMPLATE * 3 + 500}
    timings = {}
    for name, step in (("find, a full frame", lambda: finder.find(frame)),
                       ("straighten", lambda: straighten(frame, face)),
                       ("code", lambda: coder.code(straighten(frame, face)))):
        step()  # the first call pays for setting up
        began = time.monotonic()
        for _ in range(10):
            step()
        timings[name] = (time.monotonic() - began) * 100
        print(f"{name}: {timings[name]:.0f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--find-model", default=FIND_MODEL)
    parser.add_argument("--code-model", default=CODE_MODEL)
    sub = parser.add_subparsers(dest="command", required=True)
    find = sub.add_parser("find", help="faces in each picture, and their straightened crops")
    find.add_argument("images", nargs="+")
    find.add_argument("--crops", help="write each straightened face here as PPM")
    compare = sub.add_parser("compare", help="every face in these pictures against every other")
    compare.add_argument("images", nargs="+")
    sub.add_parser("bench", help="time each step on a frame of noise")
    options = parser.parse_args()
    {"find": cmd_find, "compare": cmd_compare, "bench": cmd_bench}[options.command](options)


if __name__ == "__main__":
    sys.exit(main())
