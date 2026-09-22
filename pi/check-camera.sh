#!/usr/bin/env bash
# Checks the camera the way check-audio.sh checks the sound card:
#   1. which Pi and which OS
#   2. whether the firmware detected a camera at all, and which sensor
#   3. whether libcamera's own tools can open it and take a picture
#   4. whether GStreamer can, through libcamerasrc - which is what we actually use
#   5. whether the encoder keeps up at the size we intend to send
#
# Read-only: it changes nothing, and leaves one still in /tmp.
#
# Usage:  ./check-camera.sh              640x360, webrtc-video.py's default
#         ./check-camera.sh 1280x720     check a bigger one still keeps up
#
# Note: a camera goes to one process at a time, exactly like an ALSA hw: device.
# Stop webrtc-video.py first or every test below fails as "device busy".

set -u

SIZE=${1:-640x360}
WIDTH=${SIZE%x*}
HEIGHT=${SIZE#*x}
FPS=${FPS:-30}

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
info "testing at ${WIDTH}x${HEIGHT} @ ${FPS} fps"

# ------------------------------------------------------------- 2. the firmware

head2 "Firmware and kernel"

# camera_auto_detect=1 is what makes the firmware probe the CSI connector and
# load the right sensor overlay by itself. Without it nothing appears, however
# well the ribbon is seated. A hand-written dtoverlay=imx708 also works, and
# overrides the auto-detect, so either one is enough.
CONFIG=""
for f in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$f" ] && { CONFIG="$f"; break; }
done
if [ -z "$CONFIG" ]; then
  bad "no config.txt found in /boot/firmware or /boot"
  note_missing
else
  if grep -qs '^[[:space:]]*camera_auto_detect=1' "$CONFIG"; then
    ok "$CONFIG has camera_auto_detect=1"
  elif grep -qs '^[[:space:]]*dtoverlay=imx' "$CONFIG"; then
    ok "sensor overlay set by hand: $(grep -s '^[[:space:]]*dtoverlay=imx' "$CONFIG" | head -1)"
  else
    bad "neither camera_auto_detect=1 nor a dtoverlay=imx708 line in $CONFIG"
    info "add  camera_auto_detect=1  and reboot"
    note_missing
  fi

  # The pre-libcamera stack. start_x=1 hands the camera to the closed firmware
  # blob instead, and then libcamera finds nothing at all.
  if grep -qs '^[[:space:]]*start_x=1' "$CONFIG"; then
    bad "start_x=1 is set - that is the old firmware camera stack, libcamera cannot coexist"
    note_missing
  fi
fi

# The sensor driver and the ISP are separate modules; the sensor is the one that
# proves the ribbon is on the right way round.
SENSOR=$(lsmod 2>/dev/null | awk '/^imx/ {print $1}' | tr '\n' ' ')
if [ -n "$SENSOR" ]; then
  ok "sensor driver loaded: $SENSOR"
else
  bad "no imx* sensor module loaded - the firmware did not find a camera"
  info "check the ribbon: silver contacts face DOWN into the connector, blue"
  info "stiffener up; the latch must be pressed back down evenly on both sides;"
  info "and it goes in the socket silkscreened CAMERA, not the identical DISPLAY"
  note_missing
fi

# bcm2835-isp registers a dozen nodes whether or not a camera exists, and it is
# noisy enough to bury the handful of lines that actually say something. The ISP
# is reported separately, as one line, for exactly that reason.
#
# Do not put a bare "csi" in this pattern: it matches SCSI and iSCSI, and fills
# the section with disk subsystem lines that have nothing to do with a camera.
# The CSI receiver is called unicam on a Pi 4 and rp1-cfe on a Pi 5.
printf '\n  -- kernel messages about the sensor and the CSI receiver --\n'
CAMLOG=$(dmesg 2>/dev/null | grep -iE 'imx[0-9]|unicam|rp1-cfe|csi2|camera' | grep -vi 'bcm2835-isp')
if [ -n "$CAMLOG" ]; then
  printf '%s\n' "$CAMLOG" | head -20 | sed 's/^/    /'
else
  info "nothing at all - the firmware never probed a sensor. That is what an"
  info "absent, reversed or unseated ribbon looks like; they are indistinguishable"
  info "from here, and all three are fixed the same way."
fi
if dmesg 2>/dev/null | grep -qi 'bcm2835-isp.*Loaded'; then
  info "(the ISP loaded normally, but it does that with no camera attached too)"
fi

printf '\n  -- video devices --\n'
ls /dev/video* /dev/media* 2>/dev/null | sed 's/^/    /' || info "no /dev/video* devices at all"
# This is the single clearest yes/no in the whole script. Every other node here
# belongs to the ISP or the H.264 codec and exists on a Pi with no camera at all;
# video0 is the CSI receiver, and it appears only when a sensor bound to it.
printf '\n'
if [ -e /dev/video0 ]; then
  ok "/dev/video0 exists - a sensor is bound to the CSI receiver"
  info "it carries raw Bayer, not a picture; the ISP makes the picture, and"
  info "libcamera drives both halves, which is why we use libcamerasrc"
else
  bad "no /dev/video0 - nothing is bound to the CSI receiver"
  info "video10-23 and video31 are the ISP and the H.264 codec. They are present"
  info "on a Pi with no camera whatsoever, so they prove nothing about one."
  note_missing
fi

# CSI is not hot-pluggable, and camera_auto_detect only probes at boot, so a
# camera fitted to a running Pi is invisible until it reboots - and fitting one
# to a running Pi can damage it.
if [ -z "$SENSOR" ]; then
  printf '\n'
  info "if the camera was fitted since the last boot, it cannot be seen yet:"
  info "the firmware probes the connector once, at boot, and CSI is not hot-pluggable"
fi

# ------------------------------------------------- 2b. did the firmware look?

# Everything above is Linux's view, and Linux is downstream of the decision.
# camera_auto_detect is done by the VideoCore firmware, before Linux exists: it
# probes the connector over I2C and, if it recognises a sensor, applies that
# sensor's overlay. When it finds nothing, Linux is never asked to bind anything
# and there is nothing in dmesg to see - which is exactly what an absent camera
# and a reversed ribbon both look like from up here.
head2 "What the firmware decided"

printf '  -- camera-related lines in %s --\n' "${CONFIG:-config.txt}"
if [ -n "$CONFIG" ]; then
  grep -nE '^[[:space:]]*(camera_auto_detect|start_x|gpu_mem|dtoverlay=(imx|ov)|dtparam=i2c_vc)' \
    "$CONFIG" | sed 's/^/    /' || info "none beyond the camera_auto_detect checked above"
fi

printf '\n  -- what the firmware reports (informational only) --\n'
if command -v vcgencmd >/dev/null 2>&1; then
  info "$(vcgencmd get_camera 2>&1)"
  # Do NOT derive a verdict from this line. supported= and detected= are legacy
  # camera-stack fields, and on this OS "libcamera interfaces" reads 0 as well:
  # verified 2026-09-22 against a Camera Module 3 that rpicam-hello was listing
  # correctly at that same moment. An earlier version of this script called it
  # decisive and would have reported a working camera as absent.
  info "all three fields read 0 even with a working camera here - ignore them;"
  info "/dev/video0 and the device tree below are what actually know"
else
  info "vcgencmd not installed (libraspberrypi-bin)"
fi

# dtoverlay -l lists only what was applied at RUNTIME. Everything in config.txt
# is applied by the firmware before Linux starts and never appears here, so an
# empty list is normal and says nothing either way. Do not read a verdict into
# it - the device tree below is the thing that actually knows.
printf '\n  -- overlays applied at runtime (config.txt ones never show here) --\n'
if command -v dtoverlay >/dev/null 2>&1; then
  dtoverlay -l 2>&1 | sed 's/^/    /'
else
  info "dtoverlay not installed (libraspberrypi-bin)"
fi

printf '\n  -- is a sensor in the device tree? --\n'
# -L is not optional: /proc/device-tree is a symlink to /sys/firmware/devicetree/
# base, and find will not descend into a symlinked starting point without it. The
# first version of this check reported "no sensor node" against a camera that was
# working perfectly, for exactly that reason.
SENSOR_NODE=$(find -L /proc/device-tree -maxdepth 6 \( -name 'imx*@*' -o -name 'ov*@*' \) 2>/dev/null | head -5)
if [ -n "$SENSOR_NODE" ]; then
  ok "sensor node(s) present:"
  printf '%s\n' "$SENSOR_NODE" | sed 's/^/        /'
else
  bad "no sensor node under /proc/device-tree - no camera overlay was applied"
  note_missing
fi

# The firmware's own log says what it found when it probed, in words. This is
# the only place that distinguishes "looked and saw nothing" from "never looked".
printf '\n  -- the VideoCore firmware log --\n'
if ! command -v vclog >/dev/null 2>&1; then
  info "vclog not installed (raspberrypi-sys-mods / libraspberrypi-bin)"
elif [ "$(id -u)" != 0 ]; then
  info "needs root - rerun as:  sudo vclog --msg 2>&1 | grep -i 'cam\\|imx'"
else
  VCLOG=$(vclog --msg 2>&1 | grep -iE 'cam|imx|i2c' | tail -15)
  if [ -n "$VCLOG" ]; then
    printf '%s\n' "$VCLOG" | sed 's/^/    /'
  else
    info "the firmware log mentions no camera at all"
  fi
fi

# If the sensor is powered and the ribbon is sound, it answers on the camera I2C
# bus whether or not any overlay loaded. An IMX708 sits at 0x1a. This is as close
# to a continuity test on the ribbon as software can get.
printf '\n  -- is the sensor answering on I2C? --\n'
ls /dev/i2c-* 2>/dev/null | sed 's/^/    /' || info "no i2c buses at all"
info "i2c-1 is the GPIO header, which is the HAT's bus; i2c-20 and i2c-21 are the"
info "two HDMI outputs. The camera's own bus is created BY the sensor overlay, so"
info "its absence is a consequence of the overlay never loading, not a new fault."
printf '\n'
if ! command -v i2cdetect >/dev/null 2>&1; then
  info "i2cdetect not installed - for this test:  sudo apt install -y i2c-tools"
else
  PROBED=0
  for BUS in 10 0 11; do
    [ -e "/dev/i2c-$BUS" ] || continue
    PROBED=1
    # cut away the "10:" row label before looking at the cells. Grepping the raw
    # output matches those labels as if they were device addresses and invents
    # devices at 0x10, 0x20 and so on, which an earlier version of this did.
    #
    # A cell is "--" for nothing, a two-digit address for a device that answered,
    # or "UU" for an address i2cdetect SKIPPED because a kernel driver is bound
    # to it. UU is not proof the chip replied - the driver may be failing every
    # transaction - but it does mean the overlay got as far as claiming it.
    CELLS=$(i2cdetect -y "$BUS" 2>/dev/null | tail -n +2 | cut -d: -f2-)
    FOUND=$(printf '%s' "$CELLS" | grep -oE '\b[0-9a-f]{2}\b' | tr '\n' ' ')
    BOUND=$(printf '%s' "$CELLS" | grep -c 'UU')
    if [ -n "$FOUND" ]; then
      ok "bus $BUS: device(s) answering at $FOUND   (imx708 is 1a, its focus motor 0c)"
    elif [ "$BOUND" -gt 0 ]; then
      ok "bus $BUS: $BOUND address(es) held by a bound driver (UU) - normal once"
      info "        the camera is working; i2cdetect skips those rather than probing"
    else
      info "bus $BUS: nothing answering and no driver bound"
    fi
  done
  [ "$PROBED" = 0 ] && info "no camera I2C bus exists - which is itself a consequence of no overlay"
fi

# ------------------------------------------------------- 3. libcamera userspace

head2 "libcamera"

RPICAM=""
for c in rpicam-hello libcamera-hello; do
  command -v "$c" >/dev/null 2>&1 && { RPICAM=$c; break; }
done

if [ -z "$RPICAM" ]; then
  bad "no rpicam-hello or libcamera-hello - install rpicam-apps"
  note_missing
else
  ok "$RPICAM present"
  printf '\n  -- %s --list-cameras --\n' "$RPICAM"
  LIST=$("$RPICAM" --list-cameras 2>&1)
  printf '%s\n' "$LIST" | sed 's/^/    /'
  printf '\n'

  if printf '%s' "$LIST" | grep -qi 'no cameras available'; then
    bad "libcamera sees no camera, though the kernel may have loaded a driver"
    note_missing
  elif printf '%s' "$LIST" | grep -qi 'imx708'; then
    ok "Camera Module 3 (imx708) detected"
    # There are two tuning files: imx708.json for the standard lens and
    # imx708_wide.json for the 120-degree one. The wide lens with the standard
    # tuning looks washed out at the edges, so it is worth knowing which loaded.
    if printf '%s' "$LIST" | grep -qi 'imx708_wide'; then
      ok "the WIDE variant is recognised, so the wide tuning file is in use"
    else
      info "detected as plain imx708, not imx708_wide - if this is the wide module"
      info "the lens shading tuning is the standard one. Harmless, but the corners"
      info "will look worse than they need to."
    fi
  else
    info "a camera is present but it is not an imx708:"
    printf '%s' "$LIST" | grep -i 'available cameras' -A3 | sed 's/^/        /'
  fi
fi

# ----------------------------------------------------------- 4. take a picture

head2 "Still capture"

JPEG=""
for c in rpicam-jpeg rpicam-still libcamera-jpeg libcamera-still; do
  command -v "$c" >/dev/null 2>&1 && { JPEG=$c; break; }
done

if [ -z "$JPEG" ]; then
  info "skipped: no rpicam-jpeg / rpicam-still"
elif [ -z "$SENSOR" ]; then
  info "skipped: no sensor driver, so there is nothing to photograph"
else
  SHOT=/tmp/check-camera.jpg
  rm -f "$SHOT"
  printf '  %s, 2 s of autofocus and autoexposure... ' "$JPEG"
  if "$JPEG" --nopreview -t 2000 -o "$SHOT" >/tmp/rpicam.err 2>&1; then
    BYTES=$(wc -c < "$SHOT" 2>/dev/null || echo 0)
    printf 'done\n'
    if [ "$BYTES" -gt 20000 ]; then
      ok "wrote $SHOT, $BYTES bytes"
      info "copy it off and look at it - this is the only test that proves the lens"
      info "cap is off and the camera is pointing where you think:"
      # SUDO_USER, not whoami: this script is often run under sudo, and printing
      # "scp root@..." sends people at an account that refuses SSH by default.
      info "  scp ${SUDO_USER:-$(whoami)}@\$(hostname -I | awk '{print \$1}'):$SHOT ."
    else
      bad "wrote only $BYTES bytes - that is not a real picture"
      note_missing
    fi
  else
    printf 'FAILED\n'
    tail -8 /tmp/rpicam.err | sed 's/^/        /'
    if grep -qsi 'busy\|in use' /tmp/rpicam.err; then
      info "something else holds the camera - stop webrtc-video.py and rerun"
    fi
    note_missing
  fi
fi

# -------------------------------------------------------------- 5. GStreamer

head2 "GStreamer"

if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
  bad "gst-launch-1.0 not installed - nothing else here can be checked"
  note_missing
  CAN_GST=0
else
  info "$(gst-inspect-1.0 --version | head -1)"
  CAN_GST=1
  if gst-inspect-1.0 libcamerasrc >/dev/null 2>&1; then
    ok "libcamerasrc present"
  else
    bad "libcamerasrc missing - run: sudo apt install -y gstreamer1.0-libcamera"
    note_missing
    CAN_GST=0
  fi
fi

# Runs a pipeline for a fixed number of frames and reports how long that took.
# The elapsed time is the whole point: a pipeline that completes in 1 s for 30
# frames is running at 30 fps, and one that takes 4 s is not going to hold up a
# live call however cleanly it exits.
gst_try() {
  label=$1
  shift
  printf '  %-30s ' "$label"
  start=$(date +%s.%N)
  if out=$(timeout 30 gst-launch-1.0 -q "$@" 2>&1); then
    elapsed=$(awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
    fps=$(awk -v n=60 -v t="$elapsed" 'BEGIN{if (t>0) printf "%d", n/t; else print 0}')
    printf 'ok, 60 frames in %ss (~%s fps)\n' "$elapsed" "$fps"
    # Two thirds of the target is the line between "a little jitter in the
    # measurement" and "this stage cannot feed a live call".
    if [ "$fps" -lt $((FPS * 2 / 3)) ]; then
      info "that is well under $FPS fps - this stage is the bottleneck"
      note_missing
    fi
    return 0
  fi
  printf 'FAILED\n'
  printf '%s\n' "$out" | grep -iE 'error|warning' | head -6 | sed 's/^/        /'
  note_missing
  return 1
}

if [ "$CAN_GST" = 1 ] && [ -n "$SENSOR" ]; then
  CAPS="video/x-raw,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1"

  # libcamerasrc has NO num-buffers property. num-buffers comes from GstBaseSrc,
  # and libcamerasrc derives straight from GstElement, so asking for it fails the
  # whole pipeline at parse time. identity's eos-after does the same job anywhere
  # in the chain, and works with any source.
  STOP="identity eos-after=60"

  printf '\n  -- the camera alone --\n'
  gst_try "libcamerasrc -> fakesink" \
    libcamerasrc ! "$CAPS" ! $STOP ! fakesink sync=false

  printf '\n  -- the camera through an encoder, which is the real path --\n'
  if gst-inspect-1.0 x264enc >/dev/null 2>&1; then
    gst_try "-> x264enc (software)" \
      libcamerasrc ! "$CAPS" ! $STOP \
      ! videoconvert ! video/x-raw,format=I420 \
      ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 \
      ! fakesink sync=false
  fi
  if gst-inspect-1.0 v4l2h264enc >/dev/null 2>&1; then
    gst_try "-> v4l2h264enc (hardware)" \
      libcamerasrc ! "$CAPS" ! $STOP \
      ! videoconvert ! video/x-raw,format=I420 \
      ! v4l2h264enc extra-controls="controls,repeat_sequence_header=1,video_bitrate=2000000" \
      ! video/x-h264,level=\(string\)4 ! fakesink sync=false
  fi

  # Which pixel format libcamerasrc actually settled on decides whether that
  # videoconvert above is real work or a no-op. I420 straight out of the ISP
  # costs nothing; anything else is a full-frame conversion per frame.
  printf '\n  -- what format the camera negotiated --\n'
  timeout 20 gst-launch-1.0 -v libcamerasrc ! "$CAPS" ! identity eos-after=5 ! fakesink 2>&1 \
    | grep -oE 'video/x-raw[^"]*format=\(string\)[A-Za-z0-9]+' | head -1 | sed 's/^/    /' \
    || info "could not tell"

  # Which SENSOR mode libcamera chose, which is a different question and matters
  # more. The imx708's 1536x864 mode reads only the centre 3072x1728 of the
  # sensor - a 2/3 crop - while 2304x1296 and 4608x2592 read the full frame. On
  # the WIDE lens that crop throws away the field of view the wide lens exists
  # for, and nothing downstream can get it back.
  printf '\n  -- which sensor mode it chose (watch the crop) --\n'
  MODELOG=$(LIBCAMERA_LOG_LEVELS='*:INFO' timeout 20 gst-launch-1.0 -q \
    libcamerasrc ! "$CAPS" ! identity eos-after=5 ! fakesink 2>&1 \
    | grep -iE 'selected|sensor format|mode' | head -6)
  if [ -n "$MODELOG" ]; then
    printf '%s\n' "$MODELOG" | sed 's/^/    /'
  else
    info "libcamera logged nothing about it; compare fields of view instead, by"
    info "eye, between --sensor-mode full and --sensor-mode auto"
  fi
  info "1536x864 is the binned mode and reads only the centre 3072x1728 of the"
  info "array - on the wide lens that is a third of the frame width thrown away."
  info "webrtc-video.py forces 2304x1296 by default to avoid it."
elif [ "$CAN_GST" = 1 ]; then
  info "skipped: no sensor driver loaded, so there is nothing for GStreamer to open"
fi

# ------------------------------------------------------------------- summary

head2 "Summary"

if [ "$MISSING" -eq 0 ]; then
  ok "everything checked is present and fast enough"
  printf '\n  Next, on the Pi:\n'
  printf '    ./webrtc-video.py 192.168.0.219                       camera + mic + speaker\n'
  printf '    ./webrtc-video.py 192.168.0.219 --size 1280x720 --encoder v4l2\n'
  printf '    ./webrtc-video.py 192.168.0.219 --video test          videotestsrc again\n'
elif [ -n "$SENSOR" ]; then
  # The camera is there; whatever failed is downstream of it, so the ribbon
  # advice below would send you to take apart hardware that is working.
  bad "$MISSING check(s) failed - see the [!!] lines above"
  info "the camera itself was detected, so the fault is downstream of it:"
  info "a missing package, a pipeline that will not build, or an encoder too slow"
else
  bad "$MISSING check(s) failed - see the [!!] lines above"
  printf '\n  If nothing was detected at all, in order of how often it is the cause:\n'
  printf '    1. the ribbon is in the DISPLAY connector, not CAMERA\n'
  printf '    2. the ribbon latch was never clamped, or it is in the wrong way round\n'
  printf '    3. camera_auto_detect=1 is missing from config.txt (then reboot)\n'
  printf '    4. the Pi was not powered off when the ribbon went in\n'
  # This script is read-only, so it will not load an overlay itself - but that is
  # the one test that separates a cable fault from a software one, so it says how.
  printf '\n  To tell a dead cable from a software fault, force the overlay by hand.\n'
  printf '  Not persistent; a reboot undoes it:\n'
  printf '    sudo dtoverlay imx708 && sleep 2 && dmesg | tail -30\n'
  printf '    sudo i2cdetect -y 10          # the muxed bus it creates, NOT i2c-0\n'
  printf '\n'
  printf '    "failed to read chip id 708, with error -5"  -> -EIO, no ACK on the\n'
  printf '      bus. Overlay, drivers and regulator are all fine and the module is\n'
  printf '      not electrically reachable: seating, cable, or the camera itself.\n'
  printf '    /dev/video0 appears                          -> the camera is fine and\n'
  printf '      only auto-detect failed. Pin dtoverlay=imx708 in config.txt.\n'
fi
printf '\n'
