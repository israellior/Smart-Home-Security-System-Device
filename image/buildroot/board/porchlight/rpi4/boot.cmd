# U-Boot boot script: choose system slot A or B, then boot its kernel.
#
# RAUC owns the three variables below and changes them through fw_setenv from
# Linux: BOOT_ORDER lists the slots to try, first first, and BOOT_x_LEFT counts
# the attempts each has left. Every boot spends one attempt of the slot it
# chooses; Linux gives them back with `rauc status mark-good` once the system
# has proved itself. A slot that runs out is skipped, so an update that never
# comes up healthy falls back to the other slot after three tries.
#
# Compiled to boot.scr with mkimage at build time. U-Boot's standard boot finds
# it at the root of the FAT partition.

test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3

setenv slot
for s in ${BOOT_ORDER}; do
  if test -z "${slot}"; then
    if test "${s}" = "A" && test ${BOOT_A_LEFT} -gt 0; then
      setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
      setenv slot A
      setenv part 2
    elif test "${s}" = "B" && test ${BOOT_B_LEFT} -gt 0; then
      setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
      setenv slot B
      setenv part 3
    fi
  fi
done

if test -z "${slot}"; then
  # Both exhausted: nothing has proved itself, so give both their tries back
  # rather than leave a doorbell that never boots again.
  echo "porchlight: no slot has attempts left; resetting both and restarting"
  setenv BOOT_A_LEFT 3
  setenv BOOT_B_LEFT 3
  saveenv
  reset
fi

echo "porchlight: booting slot ${slot} (A left ${BOOT_A_LEFT}, B left ${BOOT_B_LEFT})"
# Saved before the kernel starts, so a crash anywhere after this still counts.
saveenv

# The firmware has already turned cmdline.txt and its own settings into the
# device tree's /chosen/bootargs. Keep all of that, and add only what
# differs per slot. panic=10: a kernel that panics reboots, and the reboot
# spends an attempt.
fdt addr ${fdt_addr}
fdt get value fw_bootargs /chosen bootargs
setenv bootargs "${fw_bootargs} root=/dev/mmcblk0p${part} rootwait rauc.slot=${slot} panic=10"

# Each slot carries its own kernel in /boot, so an update replaces kernel and
# system together. The device tree stays the firmware's, from the FAT partition.
load mmc 0:${part} ${kernel_addr_r} /boot/Image
booti ${kernel_addr_r} - ${fdt_addr}
