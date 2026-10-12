# The Porchlight image, built with Buildroot

A `BR2_EXTERNAL` tree: everything that makes the image *ours* lives here, and
Buildroot itself stays an untouched download. It builds a Raspberry Pi 4 SD
card image that boots to a login prompt on HDMI, with `porchlightd` in
`/usr/bin`. Nothing else of the doorbell is in it yet: no config file, none of
the Python halves, no WM8960 overlay, no camera, no network beyond DHCP on
Ethernet.

**Status, 2026-10-12:** the first config, before it was moved into this tree,
booted on a Pi 4. The image with `porchlightd` in it builds, the daemon
compiles with no warnings under GCC 15, and the binary is in the ext4 image -
all checked on the build host, not yet on the Pi.

## Buildroot version: 2026.02.3

Pinned on purpose. `.02` releases are Buildroot's LTS and get fixes for a
year; a different release would build a different image from the same
defconfig. Change it deliberately, in its own commit, and rebuild from clean.

## The toolchain: Bootlin bleeding-edge, for its kernel headers

Bootlin's ready-made aarch64 glibc toolchain, the **bleeding-edge** variant:
GCC 15 and kernel headers 5.15. Buildroot's own Pi 4 defconfig picks the
*stable* variant, whose headers are 5.4, and that cannot build libgpiod 2:
the GPIO v2 ioctls it uses arrived in Linux 5.10, and a program can only use
what the toolchain's copy of the kernel headers declares, whatever kernel the
Pi actually runs. `porchlightd` was hidden from menuconfig until this changed.

Bleeding-edge is the first entry in Buildroot's list, so it is the default,
and the defconfig says nothing about the toolchain at all. That is only safe
because the Buildroot version is pinned: on an upgrade, check that the default
has not moved.

## The build host

Linux, which here means WSL2 (Ubuntu 24.04). Three things that each cost time
the first time:

- **Build on WSL's own disk, never under `/mnt/c`.** Buildroot touches
  hundreds of thousands of small files, and every access through `/mnt/c`
  crosses the Windows bridge.
- **Run `make` with a Linux-only `PATH`.** WSL appends the Windows `PATH`,
  whose entries contain spaces, and Buildroot refuses to start:
  `Your PATH contains spaces, TABs, and/or newline (\n) characters`.
- **Reach this tree through a path with no spaces.** This repo lives under
  `Smart Camera Doorbell`, and Buildroot separates `BR2_EXTERNAL` trees with
  spaces. A symlink in the WSL home avoids it.

Once:

```bash
sudo apt install -y build-essential libncurses-dev rsync bc cpio unzip file wget git
git clone https://gitlab.com/buildroot.org/buildroot.git ~/buildroot
git -C ~/buildroot checkout 2026.02.3
ln -s "/mnt/c/Users/israe/Documents/Smart Camera Doorbell/smart-doorbell-device" ~/porchlight
echo "alias brmake='PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin make'" >> ~/.bashrc
```

## Building

```bash
cd ~/buildroot
brmake BR2_EXTERNAL=$HOME/porchlight/image/buildroot porchlight_rpi4_defconfig
brmake 2>&1 | tee build.log
```

About 50 minutes from clean on 4 cores, most of it the kernel. Buildroot
remembers `BR2_EXTERNAL`, so later commands in the same `~/buildroot` do not
need it again. The result is `output/images/sdcard.img`: a 32 MB FAT boot
partition (firmware, `config.txt`, kernel, device tree) and a 120 MB ext4 root.

To flash from Windows, copy it out first, because Raspberry Pi Imager does not
always browse `\\wsl.localhost\`:

```bash
cp output/images/sdcard.img /mnt/c/Users/israe/Backups/porchlight-sdcard.img
```

Then Imager → *Use Custom* → that file, and **no** OS customisation: it works
by leaving a script for Raspberry Pi OS to run, and this image has nothing that
would run it.

## Changing the config

```bash
brmake menuconfig        # change something
brmake savedefconfig     # writes it back into configs/porchlight_rpi4_defconfig
git diff                 # the change, as a reviewable line
```

**Buildroot does not notice that a built package's inputs changed.** Each
package leaves a stamp when it finishes, and a later `make` trusts the stamp.
After editing `config.txt` or `cmdline.txt`:

```bash
brmake rpi-firmware-rebuild all
```

After removing a package, or changing the CPU, toolchain or C library, only
`brmake clean && brmake` is trustworthy. A release build is always from clean.

After editing `porchlightd`'s source, `brmake porchlightd-rebuild all`. Its
source is this repository rather than a download, so the rebuild copies the
tree again with rsync and needs no version bump.

## The root password

Stored as a SHA-512 crypt hash, never in clear text, because this file is on
GitHub. Buildroot takes anything beginning `$6$` as already hashed, and every
`$` must be doubled in the defconfig or `make` reads it as a variable. To
change it:

```bash
openssl passwd -6        # prints $6$salt$hash
```

then put it in `BR2_TARGET_GENERIC_ROOT_PASSWD` with each `$` written `$$`. A
shipped device should have no root password at all, only SSH keys.

## What is here

```
external.desc           The tree's name, PORCHLIGHT, which gives the defconfig
                        $(BR2_EXTERNAL_PORCHLIGHT_PATH) to refer to it with.
Config.in               Our packages' menu entries, under "External options".
external.mk             Our packages' recipes, every package/*/*.mk.
configs/porchlight_rpi4_defconfig
                        Buildroot's raspberrypi4_64 with the board files below,
                        the bleeding-edge toolchain, porchlightd, hostname
                        porchlight and the root password hash.
package/porchlightd/    The daemon, built from ../../porchlightd in this repo.
                        Selects libgpiod2, and json-for-modern-cpp so that
                        CMake finds nlohmann/json rather than fetching it from
                        GitHub mid-build. Installs /usr/bin/porchlightd itself,
                        because CMakeLists.txt has no install() rules.
board/porchlight/rpi4/  Still unchanged copies of Buildroot's Pi 4 files.
  config.txt            Read by the GPU firmware: kernel, 64-bit, overlays.
  cmdline.txt           The kernel command line: root=/dev/mmcblk0p2.
  post-build.sh         Runs before the root filesystem is packed; adds the
                        HDMI login prompt.
  post-image.sh         Runs genimage to make sdcard.img.
  genimage.cfg.in       The SD card's partition layout.
  patches/linux/linux.hash
                        SHA-256 of the kernel tarball. Downloads are refused
                        without a matching hash, so a new kernel needs a new one.
```
