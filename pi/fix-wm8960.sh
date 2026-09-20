#!/usr/bin/env bash
# Unpicks Waveshare's WM8960 installer so the kernel's own driver can have the card.
#
# The fault, from dmesg:
#     snd_soc_wm8960: loading out-of-tree module taints kernel.
#     Error: Driver 'asoc-simple-card' is already registered, aborting...
#
# Waveshare's machine driver registers a platform driver under the same name as the
# kernel's in-tree simple-audio-card. Whichever loses the race aborts, and what is
# left is a card that enumerates in `arecord -l` but whose PCM was never built - so
# every snd_pcm_open returns EINVAL, capture and playback, at every rate, on hw: and
# plughw: alike. Mixer routing has nothing to do with it.
#
# Kernel 6.x carries the codec, the machine driver and the overlay in-tree, so the
# cure is to take Waveshare's copies out of the way and let
# `dtoverlay=wm8960-soundcard` be serviced by the kernel's own.
#
#   ./fix-wm8960.sh                 look at everything, change nothing (default)
#   sudo ./fix-wm8960.sh --apply    move Waveshare's files aside, then reboot
#   sudo ./fix-wm8960.sh --restore  put every one of them back
#
# --apply never deletes anything. Everything it takes away is moved into
# /var/backups/wm8960-fix with a manifest, because the one mistake that costs a
# reflash here is removing the *in-tree* module by accident - and --restore exists
# so that mistake is survivable even if this script is the one that makes it.
#
# It also refuses to --apply if the survey finds something it does not understand,
# in particular an in-tree module that has been overwritten rather than shadowed.
# That case needs a package reinstall, not a move, and the report says which.

set -u

BACKUP=/var/backups/wm8960-fix
MODE=survey

case "${1:-}" in
  "")         MODE=survey ;;
  --apply)    MODE=apply ;;
  --restore)  MODE=restore ;;
  -h|--help)  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *)          printf 'unknown option: %s (try --help)\n' "$1" >&2; exit 2 ;;
esac

ok()    { printf '  [ok]  %s\n' "$*"; }
bad()   { printf '  [!!]  %s\n' "$*"; }
warn()  { printf '  [??]  %s\n' "$*"; }
info()  { printf '        %s\n' "$*"; }
head2() { printf '\n== %s ==\n' "$*"; }

# A blocker is something the script does not understand well enough to act on.
# One of them is enough to refuse --apply outright.
BLOCKERS=0
block() { bad "$*"; BLOCKERS=$((BLOCKERS + 1)); }
plan()  { printf '  [->]  %s\n' "$*"; }

need_root() {
  if [ "$(id -u)" -ne 0 ]; then
    printf '\n  this needs root: sudo %s --%s\n\n' "$0" "$MODE"
    exit 1
  fi
}

KREL=$(uname -r)
MODDIR="/lib/modules/$KREL"

# --------------------------------------------------------------------- restore
# Before the survey, not after it: putting the files back is a bookkeeping job
# that reads the manifest and nothing else, and a survey printed first would be
# describing the half-fixed state nobody asked about.

if [ "$MODE" = restore ]; then
  need_root
  head2 "Restoring"

  if [ ! -f "$BACKUP/manifest" ]; then
    bad "no manifest at $BACKUP - there is nothing to restore"
    exit 1
  fi

  while IFS="$(printf '\t')" read -r a b; do
    case "$a" in
      MASKED_SERVICE)
        systemctl unmask "$b" >/dev/null 2>&1
        systemctl enable "$b" >/dev/null 2>&1
        ok "unmasked and re-enabled $b"
        ;;
      *)
        if [ -f "$a" ]; then
          mkdir -p "$(dirname "$b")"
          mv -f "$a" "$b" && ok "restored $b"
        else
          bad "missing from the backup: $a"
        fi
        ;;
    esac
  done < "$BACKUP/manifest"

  depmod -a "$KREL" && ok "depmod -a"
  rm -rf "$BACKUP"
  printf '\n'
  info "The DKMS entry is not restored by this - if you need Waveshare's module"
  info "back, reinstall it from \$HOME/WM8960-Audio-HAT."
  info "Reboot to go back to how it was: sudo reboot"
  printf '\n'
  exit 0
fi

# ------------------------------------------------------------------ the machine

head2 "Machine"

if [ -r /proc/device-tree/model ]; then
  info "$(tr -d '\0' < /proc/device-tree/model)"
fi
info "kernel $KREL ($(uname -m))"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  info "$PRETTY_NAME"
fi

BOOTOVL=""
for d in /boot/firmware/overlays /boot/overlays; do
  [ -d "$d" ] && { BOOTOVL="$d"; break; }
done
CONFIG=""
for f in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$f" ] && { CONFIG="$f"; break; }
done

# ---------------------------------------------------------- what is loaded now

head2 "What is loaded right now"

if lsmod | grep -qi wm8960; then
  lsmod | grep -i wm8960 | sed 's/^/        /'
  # modinfo without a path answers "which file would modprobe pick", which is the
  # question that matters when two copies of the same module exist on disk.
  LOADED_FILE=$(modinfo -F filename snd_soc_wm8960 2>/dev/null)
  [ -n "$LOADED_FILE" ] && info "modprobe would load: $LOADED_FILE"
else
  info "no wm8960 module is loaded"
fi

if [ -d /sys/bus/platform/drivers/asoc-simple-card ]; then
  ok "asoc-simple-card driver is registered"
  BOUND=$(ls /sys/bus/platform/drivers/asoc-simple-card/ 2>/dev/null | grep -v '^\(bind\|unbind\|uevent\|module\)$')
  if [ -n "$BOUND" ]; then
    info "bound to: $(printf '%s' "$BOUND" | tr '\n' ' ')"
  else
    warn "but nothing is bound to it - the sound card never attached"
  fi
fi

printf '\n  -- /proc/asound/cards --\n'
sed 's/^/        /' /proc/asound/cards 2>/dev/null || info "no /proc/asound"

# A card whose machine driver aborted still shows up here, but with no pcm*
# directories underneath. That absence *is* the EINVAL, seen from the other side.
printf '\n  -- does the card have any PCM devices? --\n'
FOUND_CARD=0
for d in /proc/asound/card*; do
  [ -d "$d" ] || continue
  id=$(cat "$d/id" 2>/dev/null)
  case "$id" in
    *wm8960*|*WM8960*) ;;
    *) continue ;;
  esac
  FOUND_CARD=1
  PCMS=$(ls -d "$d"/pcm* 2>/dev/null | tr '\n' ' ')
  if [ -n "$PCMS" ]; then
    ok "card \"$id\" has PCM devices: $PCMS"
    info "if this is true and opens still fail, the problem is NOT this script's"
  else
    # Not a blocker: this is the symptom the whole script exists to cure, so
    # finding it is confirmation, not an obstacle.
    bad "card \"$id\" exists but has no PCM devices at all"
    info "this is the EINVAL seen from the other side - there is nothing to open"
  fi
done
[ "$FOUND_CARD" = 0 ] && warn "no wm8960 card in /proc/asound at all"

# --------------------------------------------------------- the modules on disk

head2 "wm8960 modules on disk"

OOT_MODULES=""
SUSPECT_MODULES=""
INTREE_OK=0

# Modules only. Waveshare's installer also drops a copy of its .dtbo under
# /lib/modules/<rel>/dtb/overlays/, which nothing ever loads from there and which
# would otherwise be reported as a module in an unrecognised place.
MODULE_LIST=$(find "$MODDIR" -name '*wm8960*' \
  \( -name '*.ko' -o -name '*.ko.xz' -o -name '*.ko.zst' -o -name '*.ko.gz' \) \
  2>/dev/null | sort)
OTHER_FILES=$(find "$MODDIR" -name '*wm8960*' \
  ! \( -name '*.ko' -o -name '*.ko.xz' -o -name '*.ko.zst' -o -name '*.ko.gz' \) \
  2>/dev/null | sort)

if [ -z "$MODULE_LIST" ]; then
  if grep -qs wm8960 "$MODDIR/modules.builtin"; then
    ok "no wm8960 .ko files, but the codec is built into the kernel"
    INTREE_OK=1
  else
    warn "no wm8960 modules found anywhere under $MODDIR"
  fi
else
  # Origin is decided twice over, and both have to agree: where the file sits, and
  # what the file says about itself. A module built out of tree has no intree tag
  # no matter which directory somebody dropped it into, and that is what catches an
  # installer that overwrote the kernel's own copy instead of shadowing it.
  while IFS= read -r m; do
    [ -n "$m" ] || continue
    intree=$(modinfo -F intree "$m" 2>/dev/null)
    owner=$(dpkg -S "$m" 2>/dev/null | cut -d: -f1)
    [ -n "$owner" ] || owner="not owned by any package"

    case "$m" in
      */updates/*|*/extra/*)
        OOT_MODULES="$OOT_MODULES$m
"
        bad "$m"
        info "out-of-tree location, intree='${intree:-no}', $owner"
        ;;
      */kernel/*)
        if [ "$intree" = "Y" ]; then
          INTREE_OK=1
          ok "$m"
          info "in-tree and intact - this one stays"
        else
          SUSPECT_MODULES="$SUSPECT_MODULES$m
"
          block "$m sits in the in-tree path but was NOT built in-tree"
          info "intree='${intree:-no}', $owner"
          info "the installer overwrote the kernel's own module rather than"
          info "shadowing it. Moving it would leave nothing behind. Reinstall"
          info "the owning package instead - see the plan at the end."
        fi
        ;;
      *)
        warn "$m is in a location this script does not recognise, leaving it alone"
        info "intree='${intree:-no}', $owner"
        ;;
    esac
  done <<EOF
$MODULE_LIST
EOF
fi

if [ -n "$OTHER_FILES" ]; then
  printf '\n'
  info "also under $MODDIR, but not modules and not loaded from there:"
  printf '%s\n' "$OTHER_FILES" | sed 's/^/          /'
fi

# --------------------------------------------------------------------- DKMS

head2 "DKMS"

DKMS_SPECS=""
if command -v dkms >/dev/null 2>&1; then
  DKMS_RAW=$(dkms status 2>/dev/null | grep -i wm8960)
  if [ -n "$DKMS_RAW" ]; then
    printf '%s\n' "$DKMS_RAW" | sed 's/^/        /'
    # Both dkms formats seen in the wild: "name/version, kernel, arch: state"
    # and the older "name, version, kernel, arch: state".
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      spec=${line%%:*}
      first=$(printf '%s' "$spec" | cut -d, -f1 | tr -d ' ')
      case "$first" in
        */*) DKMS_SPECS="$DKMS_SPECS$first
" ;;
        *)
          ver=$(printf '%s' "$spec" | cut -d, -f2 | tr -d ' ')
          [ -n "$ver" ] && DKMS_SPECS="$DKMS_SPECS$first/$ver
"
          ;;
      esac
    done <<EOF
$DKMS_RAW
EOF
    DKMS_SPECS=$(printf '%s' "$DKMS_SPECS" | sort -u)
    bad "DKMS is rebuilding Waveshare's module on every kernel update"
    info "so removing the .ko alone would not hold - the DKMS entry goes too"
  else
    ok "no wm8960 entry in dkms status"
  fi
else
  info "dkms is not installed, so nothing is being rebuilt on kernel updates"
fi

# When a DKMS module shadows one the kernel already ships, DKMS moves the
# kernel's copy aside and keeps it - that is what "(Original modules exist)" in
# `dkms status` means - and `dkms remove` puts it back. So an absent in-tree
# module is not necessarily a missing one, and this is the difference between
# "removing this leaves the card with no driver" and "removing this hands the
# card back to the kernel's own driver".
DKMS_ORIGINALS=$(find /var/lib/dkms -path '*original_module*' -name '*wm8960*' 2>/dev/null | sort)
if [ -n "$DKMS_ORIGINALS" ]; then
  printf '\n'
  ok "DKMS is holding the kernel's own module(s) aside, and will put them back:"
  printf '%s\n' "$DKMS_ORIGINALS" | sed 's/^/          /'
fi

# Deferred until now because the answer depends on what DKMS is holding.
if [ "$INTREE_OK" = 0 ] && [ -n "$OOT_MODULES" ]; then
  printf '\n'
  if [ -n "$DKMS_ORIGINALS" ]; then
    ok "nothing in-tree is visible under $MODDIR/kernel, and that is expected:"
    info "DKMS took it away when it installed Waveshare's, and gives it back on"
    info "removal. The card ends up on the kernel's own driver, which is the point."
  else
    block "no in-tree wm8960 module anywhere, and DKMS is not holding one either"
    info "removing Waveshare's copies would leave the card with no driver at all."
    info "Find out where the kernel's own went before going any further:"
    info "  ls -lR /var/lib/dkms/*/original_module/ 2>&1 | head -40"
    info "  dpkg -L linux-image-$KREL | grep wm8960"
  fi
fi

# ------------------------------------------------------------------ the overlay

head2 "The device tree overlay"

OVERLAY_INSTALLED=""
OVERLAY_CANON=""
OVERLAY_DIFFERS=unknown

if [ -n "$BOOTOVL" ] && [ -f "$BOOTOVL/wm8960-soundcard.dtbo" ]; then
  OVERLAY_INSTALLED="$BOOTOVL/wm8960-soundcard.dtbo"
  ok "in use: $OVERLAY_INSTALLED"
else
  warn "no wm8960-soundcard.dtbo in ${BOOTOVL:-the overlays directory}"
fi

# On Raspberry Pi OS the files in /boot/firmware/overlays are *copies*, placed
# there by the kernel package's postinst. dpkg therefore does not own them, and
# the pristine originals live under /usr/lib. That copy is the yardstick for
# whether Waveshare replaced the overlay or merely added one.
for c in \
  "/usr/lib/linux-image-$KREL/overlays/wm8960-soundcard.dtbo" \
  "/usr/lib/firmware/$KREL/device-tree/overlays/wm8960-soundcard.dtbo"; do
  [ -f "$c" ] && { OVERLAY_CANON="$c"; break; }
done
if [ -z "$OVERLAY_CANON" ]; then
  OVERLAY_CANON=$(find /usr/lib -name 'wm8960-soundcard.dtbo' 2>/dev/null | head -1)
fi

if [ -n "$OVERLAY_CANON" ]; then
  ok "the distribution ships one: $OVERLAY_CANON"
  if [ -n "$OVERLAY_INSTALLED" ]; then
    if cmp -s "$OVERLAY_CANON" "$OVERLAY_INSTALLED"; then
      OVERLAY_DIFFERS=no
      ok "the installed overlay is byte-for-byte the distribution's"
    else
      OVERLAY_DIFFERS=yes
      bad "the installed overlay differs from the distribution's"
      info "Waveshare replaced it; its own machine driver is what it asks for"
    fi
  fi
else
  warn "this OS does not appear to ship a wm8960-soundcard overlay"
  if [ -n "$BOOTOVL" ] && grep -qs 'wm8960' "$BOOTOVL/README"; then
    info "...but $BOOTOVL/README documents one, so the firmware knows it"
  else
    block "with no distribution overlay there is nothing to fall back to"
  fi
fi

if [ -n "$CONFIG" ]; then
  LINE=$(grep -s '^[[:space:]]*dtoverlay=.*wm8960' "$CONFIG" | head -1)
  if [ -n "$LINE" ]; then
    ok "$CONFIG asks for it: $LINE"
  else
    block "$CONFIG has no wm8960 dtoverlay line"
    info "add: dtoverlay=wm8960-soundcard"
  fi
fi

# -------------------------------------------------------- the rest of the mess

head2 "Waveshare's other leftovers"

SERVICE=""
for s in /usr/lib/systemd/system/wm8960-soundcard.service \
         /lib/systemd/system/wm8960-soundcard.service \
         /etc/systemd/system/wm8960-soundcard.service; do
  [ -f "$s" ] && { SERVICE="$s"; break; }
done
if [ -n "$SERVICE" ]; then
  bad "$SERVICE"
  info "state: $(systemctl is-enabled wm8960-soundcard.service 2>&1), $(systemctl is-active wm8960-soundcard.service 2>&1)"
else
  ok "no wm8960-soundcard.service"
fi

ASOUND=""
if [ -f /etc/asound.conf ] && grep -qsi 'wm8960' /etc/asound.conf; then
  ASOUND=/etc/asound.conf
  warn "$ASOUND names the card"
  info "not the cause of EINVAL on hw: - that bypasses it - but it is the"
  info "installer's, and it will point at the wrong card once this is fixed"
else
  ok "no wm8960-specific /etc/asound.conf"
fi

if [ -d "$HOME/WM8960-Audio-HAT" ]; then
  warn "$HOME/WM8960-Audio-HAT is still here (the installer's source tree)"
  if [ -x "$HOME/WM8960-Audio-HAT/uninstall.sh" ]; then
    info "it has its own uninstall.sh. This script does the same job but"
    info "reversibly; use whichever you trust more, not both at once."
  fi
fi

# --------------------------------------------------------------- what dmesg says

head2 "What the kernel said at boot"

if dmesg 2>/dev/null | grep -qiE 'wm8960|asoc|simple-card'; then
  dmesg | grep -iE 'wm8960|asoc|simple-card|i2s' | tail -20 | sed 's/^/        /'
else
  info "dmesg is restricted for this user - rerun this script with sudo to see it"
fi

# ------------------------------------------------------------------- the verdict

head2 "Verdict"

if [ "$BLOCKERS" -gt 0 ]; then
  bad "$BLOCKERS thing(s) above need a human decision - --apply is refused"
  printf '\n'
  if [ -n "$SUSPECT_MODULES" ]; then
    info "An in-tree module was overwritten. Reinstall its package rather than"
    info "moving the file, then run this script again:"
    printf '%s' "$SUSPECT_MODULES" | while IFS= read -r m; do
      [ -n "$m" ] || continue
      pkg=$(dpkg -S "$m" 2>/dev/null | cut -d: -f1)
      if [ -n "$pkg" ]; then
        printf '          sudo apt-get install --reinstall %s\n' "$pkg"
      else
        printf '          %s is owned by no package - it can only have come from\n' "$m"
        printf '          the installer, so it is safe to move, but do it knowingly.\n'
      fi
    done
  fi
  printf '\n'
  exit 1
fi

if [ -z "$OOT_MODULES" ] && [ -z "$DKMS_SPECS" ] && [ -z "$SERVICE" ] && \
   [ "$OVERLAY_DIFFERS" != yes ] && [ -z "$ASOUND" ]; then
  ok "nothing of Waveshare's is left - this script has no work to do"
  info "if the card still will not open, the cause is something else."
  info "Run ./check-audio.sh and look at the --dump-hw-params section."
  exit 0
fi

printf '  The plan:\n\n'
[ -n "$SERVICE" ]       && plan "disable and mask wm8960-soundcard.service"
# printf '%s\n', not '%s': the command substitution that built DKMS_SPECS ate its
# trailing newline, and `read` reports failure at an unterminated last line, so
# the loop body would never run at all.
[ -n "$DKMS_SPECS" ]    && printf '%s\n' "$DKMS_SPECS" | while IFS= read -r s; do
  [ -n "$s" ] && printf '  [->]  dkms remove %s --all\n' "$s"
done
[ -n "$DKMS_ORIGINALS" ] && printf '        (that removal is also what puts the kernel'"'"'s own module back)\n'
[ -n "$OOT_MODULES" ]   && plan "move any out-of-tree wm8960 module left over to $BACKUP"
[ "$OVERLAY_DIFFERS" = yes ] && plan "restore $OVERLAY_INSTALLED from $OVERLAY_CANON"
[ -n "$ASOUND" ]        && plan "move $ASOUND to $BACKUP"
plan "depmod -a, then you reboot"

printf '\n'
info "Nothing is deleted. --restore puts all of it back."

if [ "$MODE" = survey ]; then
  printf '\n'
  info "This was the survey. To carry it out:  sudo $0 --apply"
  printf '\n'
  exit 0
fi

# ------------------------------------------------------------------- doing it

if [ "$MODE" = apply ]; then
  need_root
  head2 "Applying"

  if [ -e "$BACKUP" ]; then
    bad "$BACKUP already exists, so a previous --apply was not undone"
    info "run --restore first, or move that directory aside yourself"
    exit 1
  fi
  mkdir -p "$BACKUP/files" || exit 1
  MANIFEST="$BACKUP/manifest"
  : > "$MANIFEST"

  # The manifest records the original absolute path of every file moved, so
  # --restore never has to guess where something came from.
  stash() {
    src=$1
    dest="$BACKUP/files/$(printf '%s' "$src" | sed 's#^/##; s#/#__#g')"
    if mv "$src" "$dest"; then
      printf '%s\t%s\n' "$dest" "$src" >> "$MANIFEST"
      ok "moved $src"
    else
      bad "could not move $src"
    fi
  }

  if [ -n "$SERVICE" ]; then
    systemctl disable --now wm8960-soundcard.service >/dev/null 2>&1
    systemctl mask wm8960-soundcard.service >/dev/null 2>&1
    ok "wm8960-soundcard.service disabled and masked"
    printf 'MASKED_SERVICE\twm8960-soundcard.service\n' >> "$MANIFEST"
  fi

  # DKMS first: `dkms remove` takes its own installed .ko away, so doing it after
  # the sweep would just leave an entry pointing at a file that is no longer there.
  if [ -n "$DKMS_SPECS" ]; then
    printf '%s\n' "$DKMS_SPECS" | while IFS= read -r s; do
      [ -n "$s" ] || continue
      printf '        dkms remove %s --all\n' "$s"
      dkms remove "$s" --all 2>&1 | sed 's/^/          /'
    done
  fi

  # The removal above is what hands the kernel's own module back. Check it landed
  # before sweeping anything else away, because the one unrecoverable-by-reboot
  # outcome here is a card with no driver at all.
  if [ "$INTREE_OK" = 0 ]; then
    if find "$MODDIR/kernel" -name '*wm8960*' 2>/dev/null | grep -q .; then
      ok "the kernel's own module is back in $MODDIR/kernel"
    else
      bad "no in-tree wm8960 module appeared after the dkms removal"
      info "stopping here rather than sweeping away the only copies left."
      info "Nothing has been moved yet; the service mask is undone by --restore."
      info "Look at: ls -lR /var/lib/dkms/*/original_module/"
      exit 1
    fi
  fi

  # Re-scan: DKMS may already have taken some of these away.
  for m in $(find "$MODDIR"/updates "$MODDIR"/extra -name '*wm8960*' 2>/dev/null); do
    stash "$m"
  done

  if [ "$OVERLAY_DIFFERS" = yes ]; then
    stash "$OVERLAY_INSTALLED"
    if cp "$OVERLAY_CANON" "$OVERLAY_INSTALLED"; then
      ok "restored the distribution's overlay"
    else
      bad "could not write $OVERLAY_INSTALLED - restore it by hand from $OVERLAY_CANON"
    fi
  fi

  [ -n "$ASOUND" ] && stash "$ASOUND"

  depmod -a "$KREL" && ok "depmod -a"

  printf '\n'
  ok "done. Now reboot: sudo reboot"
  printf '\n'
  info "After the reboot, in this order:"
  info "  1. ./check-audio.sh          - the card should open and record"
  info "  2. apply the amixer lines it prints, then: sudo alsactl store"
  info "     (the in-tree driver starts from its own defaults, so the routing"
  info "      you set up under the old driver will not have carried over)"
  info "  3. ./webrtc-video.py <server-ip>"
  printf '\n'
  info "The card's ALSA name is set by the overlay, so it may not still be"
  info "wm8960soundcard. check-audio.sh prints the right one; if it changed,"
  info "pass it on:  ./webrtc-video.py <server-ip> --mic-device hw:CARD=<name>"
  printf '\n'
  info "If the card is worse after the reboot: sudo $0 --restore"
  printf '\n'
  exit 0
fi
