#!/bin/sh
# Publishes a built bundle, so every doorbell updates to it.
#
#   image/buildroot/make-bundle.sh release-14
#   image/buildroot/publish-release.sh release-14 "Clearer Wi-Fi signal in the app"
#
# The doorbells take it between 3 and 4 in the morning, their own time, unless
# an owner switched automatic updates off or presses Update now first. The
# newest published release is the one they are brought to - publishing an
# older version again is how a fleet is rolled back.
#
# Three steps, the same shape as a clip upload: the server registers the
# release and hands back a grant for exactly this file, the bundle goes
# straight to the bucket, and the server checks it arrived before offering it.
# Re-running after a failure is safe until the publish step succeeds; after
# that the version is fixed - doorbells may already be running it.
#
# The signing key never leaves this machine and the server never sees it: the
# bundle is signed by make-bundle.sh, and a doorbell's RAUC refuses one that
# was not. RELEASE_TOKEN only lets this script put a bundle where the doorbells
# look - it cannot make one they would install.
#
# The token is the server's RELEASE_TOKEN (infra/scripts/put-secret.sh in the
# app repo), kept beside the signing key in ~/porchlight-keys/release-token,
# or given as PORCHLIGHT_RELEASE_TOKEN.

set -eu

VERSION=${1:?usage: publish-release.sh VERSION [NOTES]}
NOTES=${2:-}
BR=${BUILDROOT:-$HOME/buildroot}
KEYS=${PORCHLIGHT_KEYS:-$HOME/porchlight-keys}
API=${PORCHLIGHT_API:-https://api.porchl.stream}
BUNDLE=${BUNDLE:-$BR/output/images/porchlight-$VERSION.raucb}
export PATH="$BR/output/host/bin:$PATH"

TOKEN=${PORCHLIGHT_RELEASE_TOKEN:-}
if [ -z "$TOKEN" ] && [ -r "$KEYS/release-token" ]; then
	TOKEN=$(tr -d ' \n' < "$KEYS/release-token")
fi
[ -n "$TOKEN" ] || { echo "publish: no token - put it in $KEYS/release-token" >&2; exit 1; }
[ -r "$BUNDLE" ] || { echo "publish: $BUNDLE is missing - run make-bundle.sh $VERSION" >&2; exit 1; }

# The version the doorbell will report is the one inside the bundle, not the
# file name. Checked against the signature as well, so a bundle this
# machine's key did not sign never reaches the bucket.
INFO=$(rauc info --keyring="$KEYS/dev.cert.pem" "$BUNDLE" 2>/dev/null) || {
	echo "publish: rauc does not accept $BUNDLE with this machine's certificate" >&2
	exit 1
}
INNER=$(printf '%s\n' "$INFO" | sed -n "s/^Version:[[:space:]]*'\{0,1\}\([^']*\)'\{0,1\}[[:space:]]*$/\1/p" | head -n1)
if [ "$INNER" != "$VERSION" ]; then
	echo "publish: the bundle says it is version '$INNER', not '$VERSION'" >&2
	exit 1
fi

BYTES=$(stat -c %s "$BUNDLE")
SHA=$(sha256sum "$BUNDLE" | cut -d' ' -f1)
echo "publish: $VERSION, $BYTES bytes, sha256 $SHA"

# JSON built by python, so quotes and newlines in the notes survive.
BODY=$(python3 -c 'import json,sys; print(json.dumps({"version": sys.argv[1], "notes": sys.argv[2], "bytes": int(sys.argv[3]), "sha256": sys.argv[4]}))' \
	"$VERSION" "$NOTES" "$BYTES" "$SHA")

GRANT=$(curl -fsS -X POST "$API/api/releases" \
	-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d "$BODY") || {
	echo "publish: the server refused the release (see above)" >&2
	exit 1
}
URL=$(printf '%s' "$GRANT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["url"])')

echo "publish: uploading"
curl -fsS -T "$BUNDLE" -H "Content-Type: application/octet-stream" "$URL" >/dev/null

curl -fsS -X POST "$API/api/releases/$VERSION/publish" -H "Authorization: Bearer $TOKEN" >/dev/null
echo "publish: $VERSION is published; doorbells take it tonight, or now from the app"
