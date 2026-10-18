#!/bin/bash
# SFace from OpenCV's ONNX to the TFLite the doorbell runs, reproducibly.
#
#   image/buildroot/package/porchlight-face-models/convert-sface.sh [WORKDIR]
#
# The image does not convert anything at build time - that would put
# TensorFlow on the build host. Instead this was run once, its output is
# attached to the GitHub release porchlight-face-models.mk downloads from, and
# the .hash file holds its checksum. Running it again with the versions below
# gives the same bytes (checked twice on 2026-10-18), so the release can be
# verified, or remade, from the original.
#
# float32 only. onnx2tf's float16 does not load on LiteRT's CPU, and its
# 8-bit dynamic-range version gives codes unrelated to the original's
# (cosine 0.10 where 1.00 is identical) - so both are rejected.
#
# Linux or WSL, Python 3.12, about 2 GB of downloads the first time.
set -euo pipefail

WORK=${1:-$HOME/sface-work}
ONNX_URL=https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx
ONNX_SHA256=0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79
OUT_SHA256=a36324b28cf05b7712323744c36dd3dc911a84b17c099662ca5abdaa991a7ae3

mkdir -p "$WORK" && cd "$WORK"
if [ ! -d venv ]; then
	python3 -m venv venv
	. venv/bin/activate
	pip install -q --upgrade pip
	pip install -q onnx2tf==2.6.9 tensorflow==2.19.1 tf_keras==2.19.0 onnx==1.20.1 \
		onnxruntime==1.26.0 onnx_graphsurgeon==0.6.1 sng4onnx==2.0.1 numpy==2.1.3 \
		ai-edge-litert psutil
else
	. venv/bin/activate
fi
export TF_CPP_MIN_LOG_LEVEL=3

[ -f sface.onnx ] || curl -fsSL -o sface.onnx "$ONNX_URL"
echo "$ONNX_SHA256  sface.onnx" | sha256sum -c -

rm -rf tf_out
# The input is fixed at one 112x112 face: the original leaves the batch open,
# and a fixed shape is what lets the converter fold everything it can.
onnx2tf -i sface.onnx -o tf_out -ois data:1,3,112,112 > convert.log 2>&1 \
	|| { tail -30 convert.log; exit 1; }
cp tf_out/sface_float32.tflite sface_2021dec_float32.tflite

# The proof: the same faces through both, and the codes must agree.
python - <<'EOF'
import numpy as np
import onnxruntime as ort
from ai_edge_litert.interpreter import Interpreter

rng = np.random.default_rng(1)
faces = rng.uniform(0, 255, (8, 112, 112, 3)).astype(np.float32)
onnx = ort.InferenceSession("sface.onnx")
lite = Interpreter(model_path="sface_2021dec_float32.tflite")
lite.allocate_tensors()
inp, out = lite.get_input_details()[0], lite.get_output_details()[0]
unit = lambda v: v / np.linalg.norm(v)
worst = 1.0
for f in faces:
    ref = onnx.run(["fc1"], {"data": f.transpose(2, 0, 1)[None]})[0][0]
    lite.set_tensor(inp["index"], f[None])
    lite.invoke()
    worst = min(worst, float(unit(lite.get_tensor(out["index"])[0]) @ unit(ref)))
print(f"worst cosine to the ONNX original: {worst:.6f}")
raise SystemExit(0 if worst > 0.9999 else 1)
EOF

echo "$OUT_SHA256  sface_2021dec_float32.tflite" | sha256sum -c - \
	|| echo "different bytes from the release - fine if the versions changed, but then update the .hash"
echo "$WORK/sface_2021dec_float32.tflite"
