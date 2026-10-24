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

By hand, pictures are read as PPM (P6). The recorder hands over frames from
memory, and the photos the server sends are JPEG, read with Pillow.

The saved faces are one JSON file: person ids, the ids of their photos and the
codes made from each, nothing else - no names, which the server keeps, and no
pictures. The file is tagged with the model that made its codes, and a file
from another model is ignored rather than compared, because its codes would
match nobody and look like strangers.

`sync` is the server's list arriving, by way of server-bridge.py: the whole
list, every time. It codes each photo it has not coded before, forgets every
person and photo no longer listed, saves, and prints one JSON line for the
bridge to send back - see "Faces" in docs/server-brief.md.

    porchlight-faces.py find IMAGE.ppm [--crops DIR]
    porchlight-faces.py compare IMAGE.ppm IMAGE.ppm ...
    porchlight-faces.py enroll --store FILE --id ID IMAGE.ppm ...
    porchlight-faces.py match --store FILE IMAGE.ppm ...
    porchlight-faces.py sync --request FILE [--store FILE]
    porchlight-faces.py bench
"""

import argparse
import importlib
import io
import json
import os
import re
import sys
import time
import urllib.request

import numpy as np

MODEL_DIR = "/usr/local/share/porchlight"
FIND_MODEL = os.path.join(MODEL_DIR, "face_detection_full_range.tflite")
CODE_MODEL = os.path.join(MODEL_DIR, "face_code.tflite")
STORE = "/data/faces/faces.json"


def model_id(code_model=CODE_MODEL):
    """Which model makes the codes, from the file installed beside it.

    One place says it - the package that installs the model - so this script,
    the bridge's hello and the image cannot disagree. Changing the model means
    a new id there, and every saved face is made again from its photos rather
    than compared across models."""
    try:
        with open(os.path.splitext(code_model)[0] + ".id") as f:
            return f.read().strip() or "unknown"
    except OSError:
        return "sface_2021dec_float32"  # the only one there has been, run from a source tree


CODE_MODEL_ID = model_id()

# The server's limits, held here too: a list longer than this is cut, not
# trusted, and anything named otherwise than an id is not one.
MAX_PEOPLE = 10
MAX_PHOTOS = 20
MAX_PHOTO_BYTES = 1024 * 1024
ID = re.compile(r"^[A-Za-z0-9_-]{1,32}$")

# A face narrower than this, in the picture it was found in, is not coded:
# below it SFace is guessing, and a guess that matches somebody is worse than
# no answer.
MIN_FACE_PX = 48

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


class Store:
    """The saved faces:

        {"model": ..., "revision": 8 | null,
         "people": [{"id": ..., "photos": [{"id": ..., "codes": [[...], ...]}]}]}

    Codes are kept per photo so that a list arriving with one photo added codes
    one photo, not all of them again. `revision` is the server's list last
    applied, and what the bridge's hello reports; a file from another model
    reads as no revision at all, so the server sends every photo again.

    The first form - codes straight under the person, no photos - is still
    read, as one photo called "local": it is what `enroll` made by hand."""

    def __init__(self, path=STORE):
        self.path = path
        self.people = {}  # person id -> {photo id -> (n, 128) array}
        self.revision = None
        self.ignored = None
        try:
            with open(path) as f:
                data = json.load(f)
        except FileNotFoundError:
            return
        if data.get("model") != CODE_MODEL_ID:
            self.ignored = f"made by {data.get('model')!r}, not {CODE_MODEL_ID!r}"
            return
        self.revision = data.get("revision") if isinstance(data.get("revision"), int) else None
        for person in data.get("people", []):
            photos = person.get("photos")
            if photos is None:
                photos = [{"id": "local", "codes": person.get("codes", [])}]
            for photo in photos:
                codes = np.array(photo.get("codes", []), np.float64)
                if codes.ndim == 2 and codes.shape[1] == 128 and len(codes):
                    self.people.setdefault(str(person["id"]), {})[str(photo["id"])] = codes

    def add(self, person_id, code, photo_id="local"):
        photos = self.people.setdefault(person_id, {})
        old = photos.get(photo_id)
        photos[photo_id] = code[None] if old is None else np.vstack([old, code])

    def codes_of(self, person_id, photo_id):
        return self.people.get(person_id, {}).get(photo_id)

    def save(self):
        data = {"model": CODE_MODEL_ID, "revision": self.revision,
                "people": [{"id": pid,
                            "photos": [{"id": phid,
                                        "codes": [[round(float(v), 6) for v in c] for c in codes]}
                                       for phid, codes in sorted(photos.items())]}
                           for pid, photos in sorted(self.people.items())]}
        os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        # The recorder and the bridge run as the same user; nobody else needs
        # to read who this doorbell knows.
        os.chmod(tmp, 0o640)
        os.replace(tmp, self.path)

    def match(self, code, at_least, margin):
        """(person id or None, best score). A person is named only when their
        best code clears at_least and beats every other person by margin: two
        people scoring alike is a guess, and a guess is reported as nobody."""
        scores = sorted(((max(float(np.max(c @ code)) for c in photos.values()), pid)
                         for pid, photos in self.people.items() if photos),
                        reverse=True)
        if not scores:
            return None, 0.0
        best, pid = scores[0]
        runner_up = scores[1][0] if len(scores) > 1 else -1.0
        if best >= at_least and best - runner_up >= margin:
            return pid, best
        return None, best


def faces_in(path, finder, coder):
    rgb = read_ppm(path)
    out = []
    for face in finder.find(rgb):
        crop = straighten(rgb, face)
        out.append((face, crop, coder.code(crop)))
    return rgb, out


def largest(found):
    return max(found, key=lambda f: f[0]["box"][2], default=None)


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


def cmd_enroll(options):
    """The largest face of each picture, saved under one id."""
    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
    store = Store(options.store)
    if store.ignored:
        print(f"{options.store}: {store.ignored}; starting it again")
        store.people = {}
    added = 0
    for path in options.images:
        _, found = faces_in(path, finder, coder)
        face = largest(found)
        if face is None:
            print(f"{path}: no face")
        elif face[0]["box"][2] < MIN_FACE_PX:
            print(f"{path}: face only {face[0]['box'][2]:.0f} px wide")
        else:
            store.add(options.id, face[2])
            added += 1
            print(f"{path}: added ({face[0]['box'][2]:.0f} px, score {face[0]['score']:.2f})")
    store.save()
    total = sum(len(c) for c in store.people.get(options.id, {}).values())
    print(f"{options.id}: {total} code(s); {added} added now")


def cmd_match(options):
    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
    store = Store(options.store)
    if store.ignored:
        print(f"{options.store}: {store.ignored}")
    for path in options.images:
        _, found = faces_in(path, finder, coder)
        if not found:
            print(f"{path}: no face")
        for n, (face, _, code) in enumerate(found):
            person, score = store.match(code, options.at_least, options.margin)
            print(f"{path} face {n}: {person or 'nobody known'} (best {score:.2f})")


def download(url):
    """The photo's bytes, from a signed URL the server made. At most
    MAX_PHOTO_BYTES: a photo is a cropped face, and anything bigger is not one."""
    if not url.startswith(("https://", "http://")):
        raise ValueError("not a web address")
    with urllib.request.urlopen(url, timeout=30) as response:
        data = response.read(MAX_PHOTO_BYTES + 1)
    if len(data) > MAX_PHOTO_BYTES:
        raise ValueError(f"more than {MAX_PHOTO_BYTES} bytes")
    return data


def decode(data):
    """RGB from JPEG (or anything else Pillow reads). Imported here so that
    every other command works on a system without Pillow."""
    from PIL import Image
    image = Image.open(io.BytesIO(data))
    # Checked before decoding: the header is read, the pixels are not yet.
    if image.width * image.height > 4096 * 4096:
        raise ValueError(f"{image.width}x{image.height} is not a face crop")
    return np.asarray(image.convert("RGB"))


def code_photo(rgb, finder, coder):
    """(code, None) or (None, the reason in the contract's words).

    The app crops one face with a margin, so the face found should be the
    only sizeable one. A second face at least half its size means the crop
    caught two people, and which of them to save is not a guess to make."""
    found = sorted(finder.find(rgb), key=lambda f: -f["box"][2])
    if not found:
        return None, "no face"
    if found[0]["box"][2] < MIN_FACE_PX:
        return None, "face too small"
    if len(found) > 1 and found[1]["box"][2] >= found[0]["box"][2] / 2:
        return None, "more than one face"
    return coder.code(straighten(rgb, found[0])), None


def cmd_sync(options):
    """Apply the server's list, as server-bridge.py hands it over: code what is
    new, forget what is gone, save, and print the answer as one JSON line.

    A photo that fails is left out and said so, never fatal to the rest: one
    blurred picture must not cost someone their other twelve. Failed photos
    are not remembered either, so the next list tries them again."""
    with open(options.request) as f:
        request = json.load(f)
    revision = request.get("revision") if isinstance(request.get("revision"), int) else None
    store = Store(options.store)
    finder = coder = None
    results, kept = [], {}

    for person in (request.get("people") or [])[:MAX_PEOPLE]:
        pid = person.get("id") if isinstance(person, dict) else None
        if not isinstance(pid, str) or not ID.match(pid):
            continue
        for photo in (person.get("photos") or [])[:MAX_PHOTOS]:
            phid = photo.get("id") if isinstance(photo, dict) else None
            if not isinstance(phid, str) or not ID.match(phid):
                continue
            codes = store.codes_of(pid, phid)
            if codes is None:
                try:
                    data = download(str(photo.get("url", "")))
                except Exception as error:  # noqa: BLE001 - reported per photo
                    note(f"sync: {pid}/{phid}: could not download - {error!r}")
                    results.append({"id": phid, "ok": False, "error": "could not download"})
                    continue
                try:
                    rgb = decode(data)
                except Exception as error:  # noqa: BLE001
                    note(f"sync: {pid}/{phid}: not a picture - {error!r}")
                    results.append({"id": phid, "ok": False, "error": "not a picture"})
                    continue
                if finder is None:  # only loaded when there is something new to code
                    finder, coder = FaceFinder(options.find_model), FaceCoder(options.code_model)
                code, error = code_photo(rgb, finder, coder)
                if error:
                    results.append({"id": phid, "ok": False, "error": error})
                    continue
                codes = code[None]
            kept.setdefault(pid, {})[phid] = codes
            results.append({"id": phid, "ok": True})

    store.people, store.revision = kept, revision
    store.save()
    note(f"sync: revision {revision}, {len(kept)} people, "
         f"{sum(r['ok'] for r in results)} of {len(results)} photos usable")
    print(json.dumps({"revision": revision, "photos": results}), flush=True)


def note(text):
    print(f"porchlight-faces: {text}", file=sys.stderr, flush=True)


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
    enroll = sub.add_parser("enroll", help="save the largest face of each picture under one id")
    enroll.add_argument("--store", default=STORE)
    enroll.add_argument("--id", required=True)
    enroll.add_argument("images", nargs="+")
    match = sub.add_parser("match", help="who each face is, against the saved faces")
    match.add_argument("--store", default=STORE)
    match.add_argument("--at-least", type=float, default=0.45)
    match.add_argument("--margin", type=float, default=0.05)
    match.add_argument("images", nargs="+")
    sync = sub.add_parser("sync", help="apply the server's list of people (see server-bridge.py)")
    sync.add_argument("--request", required=True, help="the faces-sync frame, as JSON")
    sync.add_argument("--store", default=STORE)
    sub.add_parser("bench", help="time each step on a frame of noise")
    options = parser.parse_args()
    {"find": cmd_find, "compare": cmd_compare, "enroll": cmd_enroll, "match": cmd_match,
     "sync": cmd_sync, "bench": cmd_bench}[options.command](options)


if __name__ == "__main__":
    sys.exit(main())
