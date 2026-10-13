#!/bin/sh

set -u
set -e

# Add a console on tty1
if [ -e ${TARGET_DIR}/etc/inittab ]; then
    grep -qE '^tty1::' ${TARGET_DIR}/etc/inittab || \
	sed -i '/GENERIC_SERIAL/a\
tty1::respawn:/sbin/getty -L  tty1 0 vt100 # HDMI console' ${TARGET_DIR}/etc/inittab
# systemd doesn't use /etc/inittab, enable getty.tty1.service instead
elif [ -d ${TARGET_DIR}/etc/systemd ]; then
    mkdir -p "${TARGET_DIR}/etc/systemd/system/getty.target.wants"
    ln -sf /lib/systemd/system/getty@.service \
       "${TARGET_DIR}/etc/systemd/system/getty.target.wants/getty@tty1.service"
fi

# The FAT boot partition, mounted where Raspberry Pi OS mounts it, because
# that is where porchlight-firstboot.py looks for the birth certificate.
# umask=0077: the certificate holds the credential until firstboot moves it.
# Pass 0, not 2: there is no fsck.vfat in the image to check it with.
mkdir -p "${TARGET_DIR}/boot/firmware"
grep -q ' /boot/firmware ' "${TARGET_DIR}/etc/fstab" || \
	echo '/dev/mmcblk0p1 /boot/firmware vfat defaults,noatime,umask=0077 0 0' \
		>> "${TARGET_DIR}/etc/fstab"

# The overlay is copied from a Windows checkout, where every file looks
# executable to WSL, so its permissions are set here rather than trusted.
# It holds no programs, so every file in it is 0644.
OVERLAY="$(dirname "$0")/rootfs-overlay"
(cd "${OVERLAY}" && find . -type f) | while read -r f; do
	chmod 0644 "${TARGET_DIR}/${f}"
done

# dropbear ignores authorized_keys if others could have written it.
if [ -d "${TARGET_DIR}/root/.ssh" ]; then
	chmod 700 "${TARGET_DIR}/root/.ssh"
	chmod 600 "${TARGET_DIR}/root/.ssh/authorized_keys"
fi
