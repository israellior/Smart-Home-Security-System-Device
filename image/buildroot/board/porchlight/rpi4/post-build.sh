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

# dropbear ignores authorized_keys if others could have written it. The
# overlay is copied from a Windows checkout, whose permissions mean nothing,
# so they are set here rather than trusted.
if [ -d "${TARGET_DIR}/root/.ssh" ]; then
	chmod 700 "${TARGET_DIR}/root/.ssh"
	chmod 600 "${TARGET_DIR}/root/.ssh/authorized_keys"
fi
