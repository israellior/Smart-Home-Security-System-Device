#!/usr/bin/env bash
# Checks everything the live call needs, in the order it would fail:
#   1. which Pi, which OS, which python
#   2. the LiveKit SDK, and the venv it has to live in
#   3. the GStreamer elements the pipeline is built from
#   4. the device credential, and what the two tokens actually grant
#   5. that nothing else is holding the camera or the sound card
#
# Read-only: it changes nothing, joins no room and publishes nothing.
#
# Usage:  ./check-livekit.sh http://192.168.0.219:4000 porch-1
#         CREDENTIAL=/etc/porchlight/credential ./check-livekit.sh ...
#
# The two things this deliberately does NOT do are run the pipeline and join
# LiveKit, because webrtc-video.py already does each on its own:
#   ./webrtc-video.py --url URL --device-id ID --credential-file F --dry-run
#       the camera, the card and the pipeline, with no network at all
#   ./webrtc-video.py ... --check
#       the credential and the tokens, with no camera at all
# Between them those two split every failure into one half or the other.

set -u

URL=${1:-http://192.168.0.219:4000}
DEVICE=${2:-porch-1}
CREDENTIAL=${CREDENTIAL:-/etc/porchlight/credential}
VENV=${VENV:-/opt/porchlight/venv}

ok()   { printf '  [ok]  %s\n' "$*"; }
bad()  { printf '  [!!]  %s\n' "$*"; }
info() { printf '        %s\n' "$*"; }
head2() { printf '\n== %s ==\n' "$*"; }

MISSING=0
note_missing() { MISSING=$((MISSING + 1)); }

# ---------------------------------------------------------------- 1. the machine

head2 "Machine"

if [ -r /proc/device-tree/model ]; then
  info "$(tr -d '\0' < /proc/device-tree/model)"
fi
info "kernel $(uname -r) ($(uname -m))"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  info "$PRETTY_NAME"
fi
info "app server $URL, device $DEVICE"

# ------------------------------------------------------------- 2. the SDK

head2 "The LiveKit SDK"

# The SDK is a pip package and python3-gi is an apt one, so the interpreter has
# to be able to see both. A venv made without --system-site-packages imports
# livekit and then dies on `import gi`, which reads as a GStreamer problem and
# is not one.
PYTHON=""
if [ -x "$VENV/bin/python3" ]; then
  PYTHON="$VENV/bin/python3"
  ok "venv at $VENV"
  if [ -f "$VENV/pyvenv.cfg" ] &&
     grep -qs 'include-system-site-packages *= *true' "$VENV/pyvenv.cfg"; then
    ok "it can see the system packages, so python3-gi is reachable"
  else
    bad "this venv was built WITHOUT --system-site-packages"
    info "so it cannot import gi, whatever else is installed. Rebuild it:"
    info "  rm -rf $VENV"
    info "  python3 -m venv --system-site-packages $VENV"
    info "  $VENV/bin/pip install livekit"
    note_missing
  fi
else
  bad "no venv at $VENV"
  info "The SDK cannot be apt-installed and Debian will refuse a bare pip"
  info "install into the system python (PEP 668). Make one:"
  info "  sudo python3 -m venv --system-site-packages $VENV"
  info "  sudo $VENV/bin/pip install livekit"
  note_missing
  PYTHON=$(command -v python3 || true)
  [ -n "$PYTHON" ] && info "falling back to $PYTHON for the checks below"
fi

if [ -n "$PYTHON" ]; then
  info "$("$PYTHON" --version 2>&1)"
  if "$PYTHON" -c 'import gi; gi.require_version("Gst","1.0"); from gi.repository import Gst' 2>/dev/null; then
    ok "python3-gi and the GStreamer bindings import"
  else
    bad "cannot import gi / Gst"
    info "  sudo apt install -y python3-gi python3-gst-1.0"
    note_missing
  fi
  if VERSION=$("$PYTHON" -c 'import importlib.metadata as m; print(m.version("livekit"))' 2>/dev/null); then
    ok "livekit $VERSION"
  else
    bad "the livekit package is not installed in this interpreter"
    info "  $VENV/bin/pip install livekit"
    note_missing
  fi
fi

# ------------------------------------------------------- 3. GStreamer elements

head2 "GStreamer elements"

# Named with what provides them, because "missing plugin" is only useful if it
# says which package to install.
check_element() {
  if gst-inspect-1.0 "$1" >/dev/null 2>&1; then
    ok "$1"
  else
    bad "$1 is missing - $2"
    note_missing
  fi
}

if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
  bad "gst-inspect-1.0 is not installed"
  info "  sudo apt install -y gstreamer1.0-tools"
  note_missing
else
  info "$(gst-launch-1.0 --version 2>/dev/null | head -1)"
  check_element appsink       "gstreamer1.0-plugins-base"
  check_element appsrc        "gstreamer1.0-plugins-base"
  check_element libcamerasrc  "gstreamer1.0-libcamera (or run with --video test)"
  check_element videotestsrc  "gstreamer1.0-plugins-base"
  check_element videoconvert  "gstreamer1.0-plugins-base"
  check_element textoverlay   "gstreamer1.0-plugins-base (only the delay clock)"
  check_element alsasrc       "gstreamer1.0-alsa"
  check_element alsasink      "gstreamer1.0-alsa"
  check_element audioconvert  "gstreamer1.0-plugins-base"
  check_element audioresample "gstreamer1.0-plugins-base"
  check_element level         "gstreamer1.0-plugins-good"
  # The pair that makes a one-card intercom possible at all. They find each
  # other by element name within one process, which is why the call keeps
  # capture and playback in a single pipeline.
  check_element webrtcdsp       "gstreamer1.0-plugins-bad (or run with --aec off)"
  check_element webrtcechoprobe "gstreamer1.0-plugins-bad (or run with --aec off)"

  # No x264enc or v4l2h264enc check: the call does not encode. LiveKit's SDK
  # does, past the appsink. The recorder still needs them - check-camera.sh is
  # where that is tested.
  info "no encoder is checked here: the call hands raw I420 to the SDK, which encodes"
fi

# ------------------------------------------------------------ 4. the credential

head2 "The credential and the two tokens"

if [ ! -r "$CREDENTIAL" ]; then
  bad "cannot read $CREDENTIAL"
  info "It is written onto the card when the device is built. There is no"
  info "enrolment call, so a missing one means the device must be re-minted."
  note_missing
else
  ok "$CREDENTIAL is readable"
  PERMS=$(stat -c '%a %U' "$CREDENTIAL" 2>/dev/null || echo "?")
  info "mode $PERMS"
  case "$PERMS" in
    6[04]0*|4[04]0*) : ;;
    *) bad "readable by more than its owner; chmod 600 it" ;;
  esac

  if [ -n "$PYTHON" ] && [ -x "$(dirname "$0")/webrtc-video.py" ]; then
    info "asking the app server for both tokens..."
    "$PYTHON" "$(dirname "$0")/webrtc-video.py" \
      --url "$URL" --device-id "$DEVICE" --credential-file "$CREDENTIAL" --check ||
      { bad "the app server would not mint tokens"; note_missing; }
  else
    info "webrtc-video.py is not beside this script, so the tokens are not checked"
    info "  curl -fO $URL/webrtc-video.py   # or wherever it is served from"
  fi
fi

# --------------------------------------------------- 5. who holds the hardware

head2 "The camera and the sound card"

# Both go to one process at a time. A second copy of the call, a stray
# rpicam-hello, or porchlightd's own recorder will hold either one, and the
# failure reads as "cannot open" rather than as "something else has it".
BUSY=0
for dev in /dev/video0 /dev/snd/pcmC*; do
  [ -e "$dev" ] || continue
  if command -v fuser >/dev/null 2>&1; then
    HOLDER=$(fuser "$dev" 2>/dev/null | tr -s ' ')
    if [ -n "$HOLDER" ]; then
      bad "$dev is held by pid(s)$HOLDER"
      BUSY=1
    fi
  fi
done
if [ "$BUSY" -eq 0 ]; then
  if command -v fuser >/dev/null 2>&1; then
    ok "nothing is holding the camera or the sound card"
  else
    info "fuser is not installed, so this could not be checked: sudo apt install -y psmisc"
  fi
else
  info "Stop it before starting a call. porchlightd's recorder releases both when"
  info "it stops, which is why the core stops a recording before it starts a call."
  note_missing
fi

# ------------------------------------------------------------------ verdict

head2 "Verdict"

if [ "$MISSING" -eq 0 ]; then
  ok "everything the call needs is here"
  info "Next, in this order - each one isolates a different half:"
  info "  ./webrtc-video.py --url $URL --device-id $DEVICE \\"
  info "      --credential-file $CREDENTIAL --dry-run"
  info "      the pipeline alone: camera, card, caps. No network."
  info "  ./webrtc-video.py ... --video test"
  info "      the whole call with videotestsrc, to take the camera out of it."
  info "  ./webrtc-video.py ..."
  info "      the real thing."
else
  bad "$MISSING thing(s) above need fixing first"
fi
exit "$MISSING"
