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

# Every kernel module the doorbell loads (kernel-modules.txt), present as a
# file or built in. linux.config was cut down to that list, and a later change
# to it that drops one - the bundle-opening dm_verity, say - would otherwise
# build an image that boots, runs, and can never be updated.
KMODS=$(ls -d "${TARGET_DIR}"/lib/modules/*/ 2>/dev/null | head -1)
if [ -z "${KMODS}" ] || [ ! -f "${KMODS}/modules.builtin" ]; then
	echo "post-build: no kernel modules in ${TARGET_DIR}/lib/modules" >&2
	exit 1
fi
{ find "${KMODS}" -name '*.ko*'; cat "${KMODS}/modules.builtin"; } \
	| sed 's|.*/||; s|\.ko.*||' | tr - _ | sort -u > "${BUILD_DIR}/porchlight-kmods-present"
missing=$(grep -vE '^(#|$)' "$(dirname "$0")/kernel-modules.txt" | tr - _ \
	| grep -vxFf "${BUILD_DIR}/porchlight-kmods-present" || true)
if [ -n "${missing}" ]; then
	echo "post-build: the kernel lacks modules the doorbell needs:" ${missing} >&2
	exit 1
fi

# The root is read-only, so nothing can leave it needing a check, and the
# image has no fsck.ext4 to check it with: pass 1 only made every boot log
# "fsck failed". Pass 0.
sed -i 's|^\(/dev/root / auto ro 0\) 1$|\1 0|' "${TARGET_DIR}/etc/fstab"

# The FAT boot partition, mounted where Raspberry Pi OS mounts it, because
# that is where porchlight-firstboot.py looks for the birth certificate.
# umask=0077: the certificate holds the credential until firstboot moves it.
# Pass 0, not 2: there is no fsck.vfat in the image to check it with.
# Read-only: both slots boot from it, and nothing needs to write it any more -
# U-Boot's environment is in raw sectors now. firstboot alone makes it
# writable, for as long as it takes to destroy a birth certificate (its unit's
# drop-in, package porchlight-ab). The line is replaced rather than kept, so
# a tree built before this gets the new options.
mkdir -p "${TARGET_DIR}/boot/firmware"
sed -i '\| /boot/firmware |d' "${TARGET_DIR}/etc/fstab"
echo '/dev/mmcblk0p1 /boot/firmware vfat ro,noatime,umask=0077 0 0' \
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

# The data partition: what makes this device itself, kept apart from the two
# system slots so that an update - which replaces a whole slot - leaves it
# alone. porchlight-data.service (package porchlight-ab) fills it on a fresh
# device and puts the hostname back after an update.
mkdir -p "${TARGET_DIR}/data"
grep -q ' /data ' "${TARGET_DIR}/etc/fstab" || \
	echo '/dev/mmcblk0p4 /data ext4 defaults,noatime 0 0' >> "${TARGET_DIR}/etc/fstab"

# /etc/porchlight - credential, identity, the config firstboot patches - lives
# on /data. porchlightd's package installs its config as a default in
# /usr/share/porchlight/defaults; a real /etc/porchlight here is left over
# from a build made before the data partition, and only goes.
# The links are relative on purpose: Buildroot installs into a directory on
# the build machine, and anything written through an absolute /data link
# would land in the build machine's own /data.
if [ -d "${TARGET_DIR}/etc/porchlight" ] && [ ! -L "${TARGET_DIR}/etc/porchlight" ]; then
	rm -rf "${TARGET_DIR}/etc/porchlight"
fi
ln -sfn ../data/porchlight "${TARGET_DIR}/etc/porchlight"

# dropbear's host keys, so a device keeps the same SSH identity across
# updates. Buildroot points /etc/dropbear at /var/run, and its unit swaps that
# for a directory in /etc on first boot - which is inside the slot.
rm -rf "${TARGET_DIR}/etc/dropbear"
ln -sfn ../data/dropbear "${TARGET_DIR}/etc/dropbear"

# The mount point ships empty. Anything a reinstall wrote through the links
# above landed here, inside the build's target directory, and is not wanted.
rm -rf "${TARGET_DIR}/data/"*
