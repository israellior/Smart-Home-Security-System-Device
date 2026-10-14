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
# Another root filesystem image instead of the one just built - a test copy
# with something changed in it, to prove an update or a rollback.
ROOTFS=${ROOTFS:-$IMAGES/rootfs.ext4}
export PATH="$BR/output/host/bin:$BR/output/host/sbin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

for f in "$KEYS/dev.key.pem" "$KEYS/dev.cert.pem" "$ROOTFS"; do
	[ -e "$f" ] || { echo "make-bundle: $f is missing" >&2; exit 1; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp -L "$ROOTFS" "$WORK/rootfs.ext4"
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

# The system.conf inside the image, loaded by this host's RAUC, which is the
# same version. Release-6 shipped a key RAUC rejects, and a device on it
# could neither install an update nor mark itself good - which nothing short
# of a Pi showed. Only data-directory is changed: /data is the device's.
debugfs -R "dump /etc/rauc/system.conf $WORK/system.conf" "$ROOTFS" 2>/dev/null
[ -s "$WORK/system.conf" ] || { echo "make-bundle: no /etc/rauc/system.conf in $ROOTFS" >&2; exit 1; }
sed -i "s|^data-directory=.*|data-directory=$WORK/data|" "$WORK/system.conf"
if ! rauc --conf="$WORK/system.conf" --keyring="$KEYS/dev.cert.pem" info "$OUT" >/dev/null 2>"$WORK/conf.err"; then
	echo "make-bundle: RAUC refuses this image's system.conf - a device on it could not update:" >&2
	cat "$WORK/conf.err" >&2
	rm -f "$OUT"
	exit 1
fi
echo "make-bundle: RAUC accepts the image's system.conf"
ls -lh "$OUT"
