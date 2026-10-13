#!/bin/bash
# A signed RAUC update bundle from the image just built.
#
#   image/buildroot/make-bundle.sh [VERSION]
#     -> ~/buildroot/output/images/porchlight-VERSION.raucb
#
# The bundle carries the root filesystem - and so the kernel, which lives in
# /boot inside it - plus a manifest naming the device it is for. RAUC on the
# device refuses one whose signature does not chain to its keyring, or whose
# compatible is not porchlight-rpi4.
#
# The private key is not in this repository and must never be: whoever holds
# it can put any system on every doorbell. It defaults to ~/porchlight-keys.
set -euo pipefail

BR=${BUILDROOT:-$HOME/buildroot}
KEYS=${PORCHLIGHT_KEYS:-$HOME/porchlight-keys}
VERSION=${1:-$(date -u +%Y%m%d%H%M)}
IMAGES=$BR/output/images
export PATH="$BR/output/host/bin:$BR/output/host/sbin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

for f in "$KEYS/dev.key.pem" "$KEYS/dev.cert.pem" "$IMAGES/rootfs.ext4"; do
	[ -e "$f" ] || { echo "make-bundle: $f is missing" >&2; exit 1; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp -L "$IMAGES/rootfs.ext4" "$WORK/rootfs.ext4"
cat > "$WORK/manifest.raucm" <<EOF
[update]
compatible=porchlight-rpi4
version=$VERSION

[bundle]
# verity: the device checks every block against a hash tree as it reads,
# so a bundle damaged after its signature was checked is still caught.
format=verity

[image.rootfs]
filename=rootfs.ext4
EOF

OUT="$IMAGES/porchlight-$VERSION.raucb"
rm -f "$OUT"
rauc bundle --cert="$KEYS/dev.cert.pem" --key="$KEYS/dev.key.pem" "$WORK" "$OUT"
rauc info --keyring="$KEYS/dev.cert.pem" "$OUT"
ls -lh "$OUT"
