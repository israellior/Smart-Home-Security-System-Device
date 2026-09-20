#!/usr/bin/env bash
# Checks the ground everything else stands on, before any camera or pipeline exists:
#   1. which Pi and which OS
#   2. whether the WM8960 HAT's overlay loaded and ALSA sees the card
#   3. whether the WM8960's routing switches are on (it ships silent in both directions)
#   4. whether the card actually records anything, with real levels
#   5. which GStreamer elements exist, including the H.264 encoder
#
# Read-only: it changes nothing. Run it before installing packages to see what's
# missing, and again afterwards to confirm.
#
# Usage:  ./check-audio.sh
#
# Note: stop intercom.sh first. ALSA hands a hw: device to one process at a time,
# so a running arecord makes the recording test below fail as "busy".

set -u

ok()   { printf '  [ok]  %s\n' "$*"; }
bad()  { printf '  [!!]  %s\n' "$*"; }
info() { printf '        %s\n' "$*"; }
head2() { printf '\n== %s ==\n' "$*"; }

MISSING=0
note_missing() { MISSING=$((MISSING + 1)); }

# ---------------------------------------------------------------- 1. the machine

head2 "Machine"

if [ -r /proc/device-tree/model ]; then
  MODEL=$(tr -d '\0' < /proc/device-tree/model)
  info "$MODEL"
else
  info "model unknown (no /proc/device-tree/model)"
  MODEL=""
fi
info "kernel $(uname -r) ($(uname -m))"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  info "$PRETTY_NAME"
fi

case "$MODEL" in
  *"Pi 5"*) IS_PI5=1 ;;
  *) IS_PI5=0 ;;
esac

# ------------------------------------------------------------- 2. the sound card

head2 "WM8960 sound card"

# The overlay is what wires the I2S codec into ALSA. Recent Raspberry Pi OS ships it
# in the firmware, so no out-of-tree module needs compiling.
CONFIG=""
for f in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$f" ] && { CONFIG="$f"; break; }
done
if [ -n "$CONFIG" ]; then
  if grep -qs '^[[:space:]]*dtoverlay=.*wm8960' "$CONFIG"; then
    ok "$CONFIG enables the overlay: $(grep -s '^[[:space:]]*dtoverlay=.*wm8960' "$CONFIG" | head -1)"
  else
    bad "no wm8960 dtoverlay line in $CONFIG"
    note_missing
  fi
else
  bad "no config.txt found in /boot/firmware or /boot"
  note_missing
fi

OVERLAY=$(ls /boot/firmware/overlays/ /boot/overlays/ 2>/dev/null | grep -i 'wm8960' | head -1)
if [ -n "$OVERLAY" ]; then
  ok "overlay file present: $OVERLAY"
else
  bad "no wm8960 .dtbo in the overlays directory"
  info "this OS image has no in-tree overlay; Waveshare's installer would be needed"
  note_missing
fi

if lsmod 2>/dev/null | grep -qi 'wm8960'; then
  ok "kernel module loaded: $(lsmod | grep -i wm8960 | awk '{print $1}' | tr '\n' ' ')"
else
  bad "no wm8960 module in lsmod"
  note_missing
fi

# Waveshare's installer ships its own out-of-tree wm8960 modules. Their machine driver
# registers under the same name as the kernel's own simple-audio-card, so whichever
# loses the race aborts and leaves a card that enumerates but will not open: every
# snd_pcm_open returns EINVAL, in both directions, at every rate. Kernel 6.x has all
# of this in-tree, so these copies should not be present at all.
OOT=$(find "/lib/modules/$(uname -r)/updates" "/lib/modules/$(uname -r)/extra" \
  -name '*wm8960*' 2>/dev/null)
if [ -n "$OOT" ]; then
  bad "out-of-tree wm8960 modules present (Waveshare's) - these break the card:"
  printf '%s\n' "$OOT" | sed 's/^/          /'
  note_missing
else
  ok "no out-of-tree wm8960 modules; the in-tree driver is in charge"
fi

if dmesg 2>/dev/null | grep -q "Driver 'asoc-simple-card' is already registered"; then
  bad "two machine drivers collided at boot (asoc-simple-card already registered)"
  info "the card will enumerate but every open fails with Invalid argument"
  note_missing
fi

printf '\n  -- arecord -l --\n'
arecord -l 2>&1 | sed 's/^/  /'
printf '\n  -- aplay -l --\n'
aplay -l 2>&1 | sed 's/^/  /'

CARD_LINE=$(arecord -l 2>/dev/null | grep -i 'wm8960' | head -1)
CARD=$(printf '%s' "$CARD_LINE" | sed -n 's/^card \([0-9]\{1,\}\):.*/\1/p')
CARD_NAME=$(printf '%s' "$CARD_LINE" | sed -n 's/^card [0-9]\{1,\}: \([^ ]\{1,\}\).*/\1/p')

printf '\n'
if [ -n "$CARD" ]; then
  ok "capture card $CARD, name \"$CARD_NAME\""
  info "prefer hw:CARD=$CARD_NAME over hw:$CARD,0 - card numbers move between boots"
else
  bad "ALSA does not list a wm8960 capture device"
  note_missing
fi

# -------------------------------------------------------------- 3. the mixer

head2 "Mixer routing"

if [ -z "$CARD" ]; then
  info "skipped: no card"
else
  # The WM8960 is a routing matrix, not a simple card, and a switch left off makes it
  # record or play perfect silence with no error at all. Only these gate audio, so only
  # these are worth reporting: the codec's other switches (3D, Noise Gate, Deemphasis,
  # the zero-cross detectors, the unused LINPUT2/3 and RINPUT2/3 inputs, Boost Bypass)
  # are off by design, and listing them just buries the two that matter.
  printf '  -- the switches that gate audio --\n'
  for ctl in \
    "Capture" \
    "Left Input Mixer Boost" "Right Input Mixer Boost" \
    "Left Boost Mixer LINPUT1" "Right Boost Mixer RINPUT1" \
    "Left Output Mixer PCM" "Right Output Mixer PCM"; do
    state=$(amixer -c "$CARD" sget "$ctl" 2>/dev/null | grep -Eo '\[on\]|\[off\]' | head -1)
    case "$state" in
      '[on]')  ok  "$(printf '%-26s on' "$ctl")" ;;
      '[off]') bad "$(printf '%-26s OFF - this direction is silent' "$ctl")"; note_missing ;;
      *)       info "$(printf '%-26s no such control on this driver' "$ctl")" ;;
    esac
  done

  printf '\n  -- capture and playback levels --\n'
  amixer -c "$CARD" scontrols 2>/dev/null | sed -n "s/.*'\([^']*\)'.*/\1/p" \
    | grep -Ei 'capture|adc|boost|input|speaker|headphone|output' \
    | while read -r ctl; do
        val=$(amixer -c "$CARD" sget "$ctl" 2>/dev/null \
          | grep -Eo '\[[0-9]+%\]|\[on\]|\[off\]' | tr '\n' ' ')
        [ -n "$val" ] && printf '  %-42s %s\n' "$ctl" "$val"
      done
fi

# ------------------------------------------------------- 4. does it actually record

head2 "Recording test"

if [ -z "$CARD" ]; then
  info "skipped: no card"
elif ! command -v python3 >/dev/null 2>&1; then
  info "skipped: python3 not installed (needed to measure the levels)"
else
  # 48 kHz stereo is what Opus wants natively, so test that first. hw: (not plughw:)
  # on purpose: it fails loudly if the hardware can't do this rate, instead of
  # quietly resampling and hiding the answer. plughw: is tried last, because if only
  # that one works the problem is format negotiation, not the device.
  RECORDED=0
  for DEV in "hw:$CARD,0" "plughw:$CARD,0"; do
   for RATE in 48000 16000; do
    [ "$RECORDED" = 1 ] && break
    printf '  recording 3 s from %s at %s Hz, stereo... ' "$DEV" "$RATE"
    RAW=$(mktemp)
    if arecord -D "$DEV" -f S16_LE -r "$RATE" -c 2 -d 3 -t raw > "$RAW" 2>/tmp/arecord.err; then
      printf 'done\n'
      RECORDED=1
      python3 -c '
import sys, array, math
data = open(sys.argv[1], "rb").read()
a = array.array("h")
a.frombytes(data[: len(data) // 2 * 2])
if not a:
    print("  [!!]  no samples captured at all")
    sys.exit()
def stats(x):
    pk = max(max(x), -min(x)) / 32768.0
    rms = math.sqrt(sum(v * v for v in x) / len(x)) / 32768.0
    db = 20 * math.log10(rms) if rms > 0 else float("-inf")
    return pk, db
for name, ch in (("left ", a[0::2]), ("right", a[1::2])):
    pk, db = stats(ch)
    if pk == 0:      verdict = "DEAD - pure zeros, a routing switch is off"
    elif pk < 0.01:  verdict = "silent - raise Capture/Boost, or the mic hears nothing"
    elif pk < 0.05:  verdict = "very quiet - raise Capture volume"
    elif pk > 0.99:  verdict = "clipping - lower Capture volume"
    else:            verdict = "good level"
    print("  %s  peak %5.1f%%  rms %6.1f dBFS  -> %s" % (name, pk * 100, db, verdict))
' "$RAW"
      rm -f "$RAW"
    else
      printf 'FAILED\n'
      sed 's/^/        /' /tmp/arecord.err
      rm -f "$RAW"
      if grep -qs 'busy' /tmp/arecord.err; then
        info "something else holds the device - stop intercom.sh and rerun"
        RECORDED=2
        break
      fi
    fi
   done
   [ "$RECORDED" != 0 ] && break
  done

  if [ "$RECORDED" = 1 ]; then
    info "talk while it records, or the levels only prove the room is quiet"
  elif [ "$RECORDED" = 0 ]; then
    note_missing
    # "Invalid argument" from snd_pcm_open means the device refused to open at all,
    # before any format was negotiated. These three answer why.
    printf '\n  -- what the capture hardware says it accepts --\n'
    arecord -D "hw:$CARD,0" --dump-hw-params -d 1 -t raw > /dev/null 2>/tmp/hwparams.txt
    sed 's/^/    /' /tmp/hwparams.txt

    printf '\n  -- can it play, or is both directions broken? --\n'
    if speaker-test -D "hw:$CARD,0" -c 2 -r 48000 -t sine -l 1 > /tmp/spk.txt 2>&1; then
      ok "playback opened fine, so only capture is broken"
    else
      bad "playback fails too, so the card cannot open in either direction"
      tail -5 /tmp/spk.txt | sed 's/^/        /'
    fi

    printf '\n  -- kernel messages about this card --\n'
    if dmesg 2>/dev/null | grep -iE 'wm8960|i2s|asoc' | tail -20 | sed 's/^/    /'; then :; else
      info "dmesg is restricted for this user - rerun as: sudo dmesg | grep -iE 'wm8960|i2s|asoc' | tail -20"
    fi

    printf '\n  -- is anything else holding the sound devices? --\n'
    fuser -v /dev/snd/* 2>&1 | sed 's/^/    /' || info "fuser not installed (psmisc)"
  fi
fi

# -------------------------------------------------------------- 5. GStreamer

head2 "GStreamer"

if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
  bad "gst-inspect-1.0 not installed - nothing else here can be checked"
  note_missing
else
  info "$(gst-inspect-1.0 --version | head -1)"
  printf '\n'
  check_el() {
    if gst-inspect-1.0 "$1" >/dev/null 2>&1; then
      ok "$(printf '%-16s %s' "$1" "$2")"
    else
      bad "$(printf '%-16s %s' "$1" "$2")"
      note_missing
    fi
  }
  check_el webrtcbin      "the whole WebRTC engine (plugins-bad)"
  check_el opusenc        "voice codec, Pi -> browser (plugins-base)"
  check_el opusdec        "voice codec, browser -> Pi"
  check_el rtpopuspay     "Opus into RTP packets"
  check_el rtpopusdepay   "RTP packets back into Opus"
  check_el alsasrc        "the WM8960's microphones (gstreamer1.0-alsa)"
  check_el alsasink       "the WM8960's speaker output"
  check_el audioconvert   "format glue"
  check_el audioresample  "rate glue, 16k <-> 48k"
  check_el webrtcdsp      "echo cancellation, needed at Step 4"
  check_el videotestsrc   "the fake camera for Step 1"
  check_el textoverlay    "burns the Pi's clock into the picture, to measure delay"
  check_el rtph264pay     "H.264 into RTP - Phase 2 replaces this one by hand"
  printf '\n'
  check_el v4l2h264enc    "HARDWARE H.264 encoder"
  check_el x264enc        "software H.264 encoder (plugins-ugly)"
  printf '\n'
  # Not needed until Step 5, but free to check now.
  if gst-inspect-1.0 libcamerasrc >/dev/null 2>&1; then
    ok "libcamerasrc     camera source, ready for Step 5"
  else
    info "libcamerasrc     absent - only needed at Step 5 (gstreamer1.0-libcamera)"
  fi
fi

head2 "Python bindings (the Pi's signaling client will need these)"

python3 - <<'PY' 2>/dev/null || { printf '  [!!]  python3 could not import the GStreamer bindings\n'; MISSING=$((MISSING + 1)); }
import gi
for ns, ver in (("Gst", "1.0"), ("GstWebRTC", "1.0"), ("GstSdp", "1.0")):
    try:
        gi.require_version(ns, ver)
        __import__("gi.repository." + ns)
        print("  [ok]  %s" % ns)
    except Exception as e:
        print("  [!!]  %s - %s" % (ns, e))
        raise SystemExit(1)
PY

# The Pi's signaling client talks WebSocket to server.js with this one.
if python3 -c 'import websocket' 2>/dev/null; then
  ok "websocket (python3-websocket)"
else
  bad "websocket - run: sudo apt install -y python3-websocket"
  note_missing
fi

# --------------------------------------------------------------- 6. video devices

head2 "Video devices"

ls /dev/video* 2>/dev/null | sed 's/^/  /' || info "no /dev/video* devices"
if [ "$IS_PI5" = 1 ]; then
  info "Pi 5: there is no hardware H.264 encoder on this board, so v4l2h264enc"
  info "and /dev/video11 are expected to be absent. Use x264enc instead."
fi

# ------------------------------------------------------------------- summary

head2 "Summary"

if [ "$MISSING" -eq 0 ]; then
  ok "everything checked is present"
else
  bad "$MISSING check(s) failed - see the [!!] lines above"
fi

if [ -n "$CARD" ]; then
  printf '\n  For the current PCM intercom:\n'
  printf '    MIC_DEVICE=hw:CARD=%s MIC_CHANNELS=2 SPEAKER_DEVICE=hw:CARD=%s ./intercom.sh SERVER_IP\n' \
    "$CARD_NAME" "$CARD_NAME"
  printf '\n  For the GStreamer pipelines to come:\n'
  printf '    alsasrc device=hw:CARD=%s   alsasink device=hw:CARD=%s\n' "$CARD_NAME" "$CARD_NAME"
  printf '\n  If a direction is silent, turn its routing on (names come from the list above):\n'
  printf '    amixer -c %s sset "Capture" 80%%\n' "$CARD"
  printf '    amixer -c %s sset "Left Input Mixer Boost" on\n' "$CARD"
  printf '    amixer -c %s sset "Left Boost Mixer LINPUT1" on\n' "$CARD"
  printf '    amixer -c %s sset "Right Input Mixer Boost" on\n' "$CARD"
  printf '    amixer -c %s sset "Right Boost Mixer RINPUT1" on\n' "$CARD"
  printf '    amixer -c %s sset "Left Output Mixer PCM" on\n' "$CARD"
  printf '    amixer -c %s sset "Right Output Mixer PCM" on\n' "$CARD"
  printf '    amixer -c %s sset "Speaker" 80%%\n' "$CARD"
  printf '    sudo alsactl store            # keep it across reboots\n'
fi
printf '\n'
