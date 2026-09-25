#!/usr/bin/env bash
# Takes a Pi from a fresh Raspberry Pi OS install to a doorbell that is
# connected to the app server, in one pass, and says what it did.
#
# It is the missing half of the credential: the server mints one and prints it
# once, and until now everything after that was a dozen hand-typed commands
# whose order mattered and whose failures all looked alike. This is that list,
# checked rather than remembered.
#
#   ./provision.sh --url https://porchlight.example --device-id porch-1
#   ./provision.sh --url https://porchlight.example --device-id porch-1 --apply
#   ./provision.sh --url https://porchlight.example --device-id porch-1 --verify
#
# **The default is a survey and changes nothing**, like fix-wm8960.sh: it
# reports what is already right, what it would do, and what it cannot do for
# you. --apply carries it out. --verify runs only the three proofs at the end,
# which is what to run when a device that used to work has stopped.
#
# The credential is never an argument. It is prompted for, or read from a file
# you have already written - arguments are world-readable in /proc and land in
# shell history, and this secret cannot be rotated over the network.
#
# It expects the repository's own layout beside it: this script in pi/, the
# daemon's source in porchlightd/. That holds however the tree arrived - a
# tarball, scp, or a clone - so it makes no assumption about a dev file server
# being reachable.

set -u

URL=""
DEVICE=""
APPLY=0
VERIFY_ONLY=0
CRED_FILE=""
SOURCE=""

usage() {
  cat >&2 <<'USAGE'
usage: provision.sh --url URL --device-id ID [--apply] [--verify]
                    [--credential-file PATH] [--source DIR]

  --url               the app server, e.g. https://porchlight.example
  --device-id         this doorbell's slug, e.g. porch-1
  --apply             carry out the changes (the default only reports)
  --verify            skip installation; run the three proofs and exit
  --credential-file   read the credential from here instead of prompting
  --source            the source tree (default: the parent of this script)
USAGE
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL=${2:-}; shift 2 ;;
    --device-id) DEVICE=${2:-}; shift 2 ;;
    --credential-file) CRED_FILE=${2:-}; shift 2 ;;
    --source) SOURCE=${2:-}; shift 2 ;;
    --apply) APPLY=1; shift ;;
    --verify) VERIFY_ONLY=1; shift ;;
    -h|--help) usage ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage ;;
  esac
done

[ -n "$URL" ] || usage
[ -n "$DEVICE" ] || usage

if [ -z "$SOURCE" ]; then
  SOURCE=$(cd "$(dirname "$0")/.." && pwd)
fi

LIB=/usr/local/lib/porchlight
ETC=/etc/porchlight
VENV=/opt/porchlight/venv
SPOOL=/var/lib/porchlight/spool
SHARE=/usr/local/share/porchlight
BIN=/usr/local/bin/porchlightd
UNIT=/etc/systemd/system/porchlightd.service
CRED=$ETC/credential
SERVICE_USER=porchlight

ok()    { printf '  [ok]  %s\n' "$*"; }
bad()   { printf '  [!!]  %s\n' "$*"; PROBLEMS=$((PROBLEMS + 1)); }
info()  { printf '        %s\n' "$*"; }
head2() { printf '\n== %s ==\n' "$*"; }

PROBLEMS=0
PLANNED=0

# Every change to the machine goes through here, so the survey and the apply
# can never disagree about what would happen: they run the same list.
run() {
  if [ "$APPLY" -eq 1 ]; then
    if "$@"; then
      return 0
    fi
    bad "failed: $*"
    return 1
  fi
  PLANNED=$((PLANNED + 1))
  printf '  [->]  would run: %s\n' "$*"
  return 0
}

as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    run "$@"
  else
    run sudo "$@"
  fi
}

# ---------------------------------------------------------------- the machine

head2 "This machine"

if [ -r /proc/device-tree/model ]; then
  info "$(tr -d '\0' < /proc/device-tree/model)"
fi
info "kernel $(uname -r) ($(uname -m))"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  info "$PRETTY_NAME"
  case "${VERSION_ID:-}" in
    13) ;;
    *) info "note: written against Debian 13 trixie. libgpiod 2.x is the one"
       info "      dependency older releases cannot satisfy - 12 ships 1.6.3,"
       info "      whose C API is an unrelated one." ;;
  esac
fi
info "source tree $SOURCE"
info "app server $URL, device $DEVICE"
if [ "$APPLY" -eq 1 ]; then
  info "applying"
else
  info "survey only - nothing here changes the machine. Add --apply."
fi

for needed in pi/server-bridge.py pi/upload-clip.py pi/webrtc-video.py; do
  [ -f "$SOURCE/$needed" ] || bad "$SOURCE/$needed is missing - wrong --source?"
done

if [ "$VERIFY_ONLY" -eq 1 ]; then
  info "--verify: skipping to the proofs"
fi

# ---------------------------------------------------------------- packages

if [ "$VERIFY_ONLY" -eq 0 ]; then
head2 "Packages"

# Split by what they are for, because a missing one in each group fails in a
# completely different place: the first group at the recorder, the second in
# the call's own pipeline, the third at `import`, the fourth at build time.
PACKAGES="
gstreamer1.0-tools
gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad
gstreamer1.0-libav
gstreamer1.0-alsa
gstreamer1.0-libcamera
rpicam-apps
python3-gi
python3-gst-1.0
python3-websocket
python3-venv
libgpiod-dev
cmake
g++
"

MISSING_PACKAGES=""
for package in $PACKAGES; do
  if dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q "install ok installed"; then
    :
  else
    MISSING_PACKAGES="$MISSING_PACKAGES $package"
  fi
done

if [ -z "$MISSING_PACKAGES" ]; then
  ok "all present"
else
  info "missing:$MISSING_PACKAGES"
  # shellcheck disable=SC2086
  as_root apt-get install -y $MISSING_PACKAGES
fi

# libgpiod 1.x is not a fallback, it is a different library, and the failure is
# a compile error deep in gpio_input.cpp rather than anything about versions.
if command -v pkg-config >/dev/null 2>&1; then
  GPIOD_VERSION=$(pkg-config --modversion libgpiod 2>/dev/null || true)
  case "$GPIOD_VERSION" in
    2.*) ok "libgpiod $GPIOD_VERSION" ;;
    "")  info "libgpiod not found by pkg-config yet - it arrives with the packages above" ;;
    *)   bad "libgpiod $GPIOD_VERSION: the daemon needs 2.x, whose C API is unrelated to 1.x" ;;
  esac
fi

# ---------------------------------------------------------------- user and paths

head2 "The user, the groups and the directories"

if id "$SERVICE_USER" >/dev/null 2>&1; then
  ok "user $SERVICE_USER exists"
else
  as_root useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi

# The card, the camera and the GPIO lines. The unit asks for these as
# SupplementaryGroups, but only if the groups exist and the user is in them.
for group in audio video gpio; do
  if getent group "$group" >/dev/null 2>&1; then
    if id -nG "$SERVICE_USER" 2>/dev/null | tr ' ' '\n' | grep -qx "$group"; then
      ok "$SERVICE_USER is in $group"
    else
      as_root usermod -aG "$group" "$SERVICE_USER"
    fi
  else
    info "no group '$group' on this system - skipping"
  fi
done

for directory in "$ETC" "$LIB" "$SHARE" "$SPOOL"; do
  if [ -d "$directory" ]; then
    ok "$directory"
  else
    as_root mkdir -p "$directory"
  fi
done
as_root chown "$SERVICE_USER" "$SPOOL"

# ---------------------------------------------------------------- the scripts

head2 "The Python halves"

# All three are installed, not just the two the daemon runs directly: the call
# is started by the daemon through the venv's interpreter, and a device with a
# working socket and no webrtc-video.py fails only when somebody presses Watch.
for script in server-bridge.py upload-clip.py webrtc-video.py; do
  if [ -f "$LIB/$script" ] && cmp -s "$SOURCE/pi/$script" "$LIB/$script"; then
    ok "$script is current"
  else
    as_root install -m 0755 "$SOURCE/pi/$script" "$LIB/$script"
  fi
done

# ---------------------------------------------------------------- the venv

head2 "The venv and the LiveKit SDK"

# --system-site-packages is not optional and its absence is the single most
# misread failure here: the SDK imports, `import gi` does not, and the message
# is about GStreamer. See check-livekit.sh, which says the same thing.
if [ -x "$VENV/bin/python3" ]; then
  ok "venv at $VENV"
  if grep -qs 'include-system-site-packages *= *true' "$VENV/pyvenv.cfg"; then
    ok "it can see the system packages"
  else
    bad "this venv was built WITHOUT --system-site-packages, so it cannot import gi"
    info "  sudo rm -rf $VENV   then run this again"
  fi
else
  as_root python3 -m venv --system-site-packages "$VENV"
fi

if [ -x "$VENV/bin/python3" ] && "$VENV/bin/python3" -c 'import livekit.rtc' 2>/dev/null; then
  ok "the livekit SDK imports"
else
  as_root "$VENV/bin/pip" install --upgrade livekit
fi

if [ -x "$VENV/bin/python3" ]; then
  if "$VENV/bin/python3" -c 'import gi; gi.require_version("Gst","1.0"); from gi.repository import Gst' 2>/dev/null; then
    ok "and so do the GStreamer bindings, in the same interpreter"
  else
    bad "the venv cannot import gi. Both halves have to work in one interpreter."
  fi
fi

# ---------------------------------------------------------------- the daemon

head2 "The daemon"

if [ -x "$BIN" ]; then
  ok "$BIN is installed"
elif [ -x "$SOURCE/porchlightd/build/porchlightd" ]; then
  ok "a build exists at $SOURCE/porchlightd/build/porchlightd"
  as_root install -m 0755 "$SOURCE/porchlightd/build/porchlightd" "$BIN"
else
  # Deliberately not built here. A build takes minutes and fails in ways worth
  # reading - a missing libgpiod 2.x above all - and burying that inside a
  # provisioning run turns one clear compile error into "the script failed".
  bad "there is no daemon to install. Build it first:"
  info "  cmake -S $SOURCE/porchlightd -B $SOURCE/porchlightd/build \\"
  info "        -DCMAKE_BUILD_TYPE=Release -DPORCHLIGHT_GPIO=ON -DBUILD_TESTING=OFF"
  info "  cmake --build $SOURCE/porchlightd/build -j4"
  info "then run this again. -DPORCHLIGHT_GPIO=ON is what the button, the PIR"
  info "and the LED need; without it the daemon refuses those backends by name."
fi

# ---------------------------------------------------------------- the config

head2 "The configuration"

EXAMPLE="$SOURCE/porchlightd/porchlightd.example.json"
CONFIG="$ETC/porchlightd.json"

if [ -f "$CONFIG" ]; then
  ok "$CONFIG exists - left alone"
  # Reported rather than corrected: this file is the operator's, and the two
  # values worth checking are the two that make the device talk to the wrong
  # server or answer to the wrong name.
  CONFIGURED_ID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("device_id",""))' "$CONFIG" 2>/dev/null || true)
  CONFIGURED_URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("server",{}).get("base_url",""))' "$CONFIG" 2>/dev/null || true)
  [ "$CONFIGURED_ID" = "$DEVICE" ] || bad "device_id in the config is '$CONFIGURED_ID', not '$DEVICE'"
  [ "$CONFIGURED_URL" = "$URL" ] || bad "server.base_url is '$CONFIGURED_URL', not '$URL'"
elif [ ! -f "$EXAMPLE" ]; then
  bad "no config, and no $EXAMPLE to build one from"
else
  info "writing one from the tracked example, with this device's own values"
  TEMP_CONFIG=$(mktemp)
  # Edited as JSON rather than with sed: a config that parses but means
  # something else is worse than one that fails to parse, and the daemon
  # refuses to start on either.
  if python3 - "$EXAMPLE" "$TEMP_CONFIG" "$DEVICE" "$URL" <<'EDIT'
import json, sys
source, target, device_id, url = sys.argv[1:5]
config = json.load(open(source))
config["device_id"] = device_id
config.setdefault("server", {})["base_url"] = url
# The real hardware, which the example cannot default to: a binary built
# without -DPORCHLIGHT_GPIO=ON refuses these by name, which is the point.
config.setdefault("backends", {})["input"] = "gpio"
config["backends"]["led"] = "gpio"
json.dump(config, open(target, "w"), indent=2)
open(target, "a").write("\n")
EDIT
  then
    as_root install -m 0644 "$TEMP_CONFIG" "$CONFIG"
    [ "$APPLY" -eq 1 ] || info "(and would put $DEVICE / $URL / gpio backends in it)"
  else
    bad "could not build a config from $EXAMPLE"
  fi
  rm -f "$TEMP_CONFIG"
fi

# ---------------------------------------------------------------- the credential

head2 "The credential"

credential_shape_ok() {
  # pl_<deviceId>_<secret>, and the device id inside it has to be this device.
  # Pasting porch-2's credential into porch-1 is otherwise diagnosed as a
  # rejected credential twenty minutes later, on the wrong machine.
  case "$1" in
    "pl_${DEVICE}_"*) return 0 ;;
    pl_*) info "that credential is for a different device id"; return 1 ;;
    *) info "that does not look like a credential (expected pl_${DEVICE}_...)"; return 1 ;;
  esac
}

if [ -s "$CRED" ]; then
  ok "$CRED exists"
  if credential_shape_ok "$(cat "$CRED" 2>/dev/null || echo unreadable)"; then
    ok "and names this device"
  else
    bad "the credential on this card is not this device's"
  fi
  # The trap this catches is not a missing file, it is a readable one: the unit
  # runs as $SERVICE_USER, and a credential written by hand belongs to whoever
  # typed it. That fails as an authentication error, which is the wrong hunt.
  if [ "$(stat -c '%U' "$CRED" 2>/dev/null)" = "$SERVICE_USER" ]; then
    ok "owned by $SERVICE_USER, so the unit can read it"
  else
    as_root chown "$SERVICE_USER" "$CRED"
  fi
  as_root chmod 0600 "$CRED"
elif [ -n "$CRED_FILE" ]; then
  if [ -s "$CRED_FILE" ]; then
    as_root install -m 0600 -o "$SERVICE_USER" "$CRED_FILE" "$CRED"
  else
    bad "$CRED_FILE is empty or missing"
  fi
elif [ "$APPLY" -eq 1 ]; then
  info "Mint it on the server first, which prints it exactly once:"
  info "  node scripts/mint-device.mjs --device-id $DEVICE"
  printf '        paste it here (it will not echo): '
  read -rs TYPED
  printf '\n'
  if [ -z "$TYPED" ]; then
    bad "nothing typed; the device stays unprovisioned"
  elif credential_shape_ok "$TYPED"; then
    # printf, not echo: the credential is compared verbatim on the server and
    # nothing should depend on a trailing newline being stripped. Written
    # through a pipe rather than as an argument, for the reason at the top of
    # this file - even here, where the only reader would be root.
    if [ "$(id -u)" -eq 0 ]; then
      WRITE_CRED() { cat > "$CRED"; }
    else
      WRITE_CRED() { sudo tee "$CRED" >/dev/null; }
    fi
    if printf '%s' "$TYPED" | WRITE_CRED; then
      as_root chown "$SERVICE_USER" "$CRED"
      as_root chmod 0600 "$CRED"
      ok "written to $CRED"
    else
      bad "could not write $CRED"
    fi
  else
    bad "refused: that is not this device's credential"
  fi
  unset TYPED
else
  info "no credential yet. --apply prompts for one, or pass --credential-file."
  info "It is minted on the server and shown once:"
  info "  node scripts/mint-device.mjs --device-id $DEVICE"
  PLANNED=$((PLANNED + 1))
fi

# ---------------------------------------------------------------- the chime

head2 "The chime"

if [ -f "$SHARE/chime.wav" ]; then
  ok "$SHARE/chime.wav"
elif ! command -v gst-launch-1.0 >/dev/null 2>&1; then
  info "no gst-launch-1.0 to make one with, and no chime.wav installed."
  info "The doorbell works without it: a missing sound is one warning, and"
  info "the alert goes out over a different path entirely."
else
  info "making a two-tone one"
  if [ "$APPLY" -eq 1 ]; then
    TEMP_CHIME=$(mktemp -u /tmp/chime-XXXXXX.wav)
    if gst-launch-1.0 -q -e concat name=c ! audioconvert ! wavenc \
        ! filesink location="$TEMP_CHIME" \
        audiotestsrc wave=sine freq=784 num-buffers=18 \
          ! audio/x-raw,rate=48000,channels=1,format=S16LE ! c. \
        audiotestsrc wave=sine freq=622 num-buffers=32 \
          ! audio/x-raw,rate=48000,channels=1,format=S16LE ! c. >/dev/null 2>&1; then
      as_root install -m 0644 "$TEMP_CHIME" "$SHARE/chime.wav"
    else
      # Not a failure worth stopping for: the chime is best effort and the
      # alert goes out over a different path entirely.
      info "could not build one. Any WAV will do; chime.sound says where it is."
    fi
    rm -f "$TEMP_CHIME"
  else
    PLANNED=$((PLANNED + 1))
    printf '  [->]  would make %s with gst-launch\n' "$SHARE/chime.wav"
  fi
fi

# ---------------------------------------------------------------- the unit

head2 "The systemd unit"

SOURCE_UNIT="$SOURCE/porchlightd/systemd/porchlightd.service"
if [ ! -f "$SOURCE_UNIT" ]; then
  bad "$SOURCE_UNIT is missing"
elif [ -f "$UNIT" ] && cmp -s "$SOURCE_UNIT" "$UNIT"; then
  ok "installed and current"
else
  as_root install -m 0644 "$SOURCE_UNIT" "$UNIT"
  as_root systemctl daemon-reload
fi

fi  # VERIFY_ONLY

# ---------------------------------------------------------------- the proofs

head2 "Does it actually work"

# Three questions in the order they fail, each isolating a different half.
# Running them in the other order costs an evening: a socket that will not open
# and a credential that is refused look identical from the daemon's log.
PROVED=0

if [ ! -s "$CRED" ]; then
  info "no credential yet, so none of the three can be answered"
elif [ ! -x "$LIB/server-bridge.py" ]; then
  info "server-bridge.py is not installed yet"
else
  info "1/3  the credential, over HTTP - no socket involved"
  if "$LIB/server-bridge.py" --url "$URL" --device-id "$DEVICE" \
      --credential-file "$CRED" --check; then
    ok "the server knows this device and accepts its credential"
    PROVED=$((PROVED + 1))
  else
    bad "the credential, the URL or the route is wrong - and nothing below can pass"
  fi

  info "2/3  the signaling socket, which is what the daemon actually holds"
  if "$LIB/server-bridge.py" --url "$URL" --device-id "$DEVICE" \
      --credential-file "$CRED" --probe; then
    ok "the handshake completes"
    PROVED=$((PROVED + 1))
  else
    bad "the socket did not come up. If 1/3 passed, this is a proxy or a"
    info "firewall that carries HTTPS and drops the WebSocket upgrade."
  fi

  if [ -x "$VENV/bin/python3" ] && [ -x "$LIB/webrtc-video.py" ]; then
    info "3/3  both LiveKit tokens, with no camera involved"
    if "$VENV/bin/python3" "$LIB/webrtc-video.py" --url "$URL" --device-id "$DEVICE" \
        --credential-file "$CRED" --check; then
      ok "the call can be minted"
      PROVED=$((PROVED + 1))
    else
      bad "the token endpoints refused. Alerts and clips will still work."
    fi
  else
    info "3/3  skipped: the venv or webrtc-video.py is not installed yet"
  fi
fi

if [ -x "$BIN" ] && [ -f "$ETC/porchlightd.json" ]; then
  if "$BIN" --print-pipeline "$ETC/porchlightd.json" >/dev/null 2>&1; then
    ok "the config parses and the recorder's pipeline builds"
  else
    bad "the daemon will not read $ETC/porchlightd.json:"
    "$BIN" --print-pipeline "$ETC/porchlightd.json" 2>&1 | head -3 | sed 's/^/        /'
  fi
fi

# ---------------------------------------------------------------- starting it

head2 "Starting it"

if [ "$PROVED" -lt 2 ]; then
  info "not enabling the service: the link is not proved yet."
  info "The daemon would start, chime, record and queue - all of which work"
  info "offline - and never deliver anything. Fix the above first."
elif systemctl is-enabled porchlightd >/dev/null 2>&1; then
  ok "porchlightd is enabled"
  as_root systemctl restart porchlightd
else
  as_root systemctl enable --now porchlightd
fi

if [ "$APPLY" -eq 1 ] && systemctl is-active porchlightd >/dev/null 2>&1; then
  ok "running"
  info "journalctl -u porchlightd -f    # 'server connected' is the line to wait for"
fi

# ---------------------------------------------------------------- summary

head2 "Summary"

if [ "$APPLY" -eq 0 ]; then
  info "$PLANNED change(s) to make. Re-run with --apply."
fi
if [ "$PROBLEMS" -eq 0 ]; then
  ok "nothing wrong found"
else
  bad "$PROBLEMS problem(s) above"
fi
info "proved $PROVED of 3"

# The proofs are what this exits on. Whether packages were already installed is
# not interesting to a caller; whether the doorbell can reach its server is.
[ "$PROVED" -eq 3 ] && [ "$PROBLEMS" -eq 0 ]
