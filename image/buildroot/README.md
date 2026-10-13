# The Porchlight image, built with Buildroot

A `BR2_EXTERNAL` tree: everything that makes the image *ours* lives here, and
Buildroot itself stays an untouched download. It builds a Raspberry Pi 4 SD
card image running systemd and NetworkManager - the two things the doorbell's
own programs assume, because they were written for Raspberry Pi OS - with Wi-Fi,
an SSH server, the two programs that run first at every boot
(`porchlight-firstboot.py`, `porchlight-setup.py`), and `porchlightd` running
as a service with its WebSocket bridge, the WM8960 chime, the recorder and
uploader on the Camera Module 3, and the live call through LiveKit - the
whole doorbell, on an image built from this repo.

**Status, 2026-10-13:** a freshly flashed card does the whole first boot on
the Pi, against production. A birth certificate on the boot partition became
`porch-2`; setup raised its access point, a phone chose the home network with
one Connect, and 0.3 s after setup finished `porchlightd` started as
`porchlight`, opened the button, PIR and LED lines, and its bridge connected:
the app shows the doorbell online, and a button press reaches it as a ring.
Getting there found four faults in `setup.py` and one gap in the image - see
"What the first boot found" below. A press now chimes from the HAT and records
a 15 s clip that uploads and confirms, and Watch in the app starts a call:
30 fps out, the microphone out with echo cancellation on, a viewer's voice
out of the speaker, and the call ending itself when the viewer leaves.

The LiveKit SDK is LiveKit's own prebuilt aarch64 wheel (`python-livekit`),
hash-pinned at 1.1.19: its core is Rust, and the wheel's native library needs
only libc, libm, libdl, libpthread and libgcc_s.

**The camera runs on upstream libcamera, not Raspberry Pi's.** Upstream's
`libcamerasrc` has no `sensor-config`, so libcamera picks the IMX708's binned
1536x864 mode, which reads only the centre of the sensor: about 98° of the
wide lens's 120°. Raspberry Pi's libcamera, as a package of our own, is what
gets the rest back. Its DMA heaps also needed a udev rule
(`60-dma-heap.rules`): without it libcamera, running as `porchlight`, logged
`Could not open any dma-buf provider` and found no camera.

## Getting in

The image holds no Wi-Fi password, and must not: it is the same for every
device, and it is on GitHub. A device is given its network the way a
customer's is: a birth certificate on the boot partition, then a phone on the
setup access point (`Porchlight-XXXX`, its password in the certificate) and
the page at `http://192.168.4.1`. NetworkManager keeps the profile on the card
and rejoins at every boot.

Without a certificate, or for a quick look, the HDMI console still works -
once, as root:

```sh
nmcli device status                                  # wlan0 must be listed as wifi
nmcli --ask device wifi connect "<network name>"     # prompts, so it stays out of shell history
ip addr show wlan0                                   # the address to ssh to
```

Then:

```powershell
ssh -i $HOME\.ssh\porchlight_ed25519 root@<address>
```

Dropbear has no `sftp-server`, so modern `scp` fails with
`sftp-server: not found`. Copy through a plain session instead:
`ssh ... root@<address> "cat > /path" < file`. And while the Pi is in setup
mode it is reachable at `192.168.4.1` from anything that joins its access
point.

The public half of that key is `rootfs-overlay/root/.ssh/authorized_keys`;
the private half never leaves the PC it was made on. A reflashed card makes a
new host key, so SSH will refuse with *REMOTE HOST IDENTIFICATION HAS CHANGED*
until the old one goes: `ssh-keygen -R <address>`.

## What the first boot found

Each of these passed every test on the build host and failed on the Pi.

- **Setup scanned before any scan had finished.** It asked for a rescan and
  read the list two seconds later, while NetworkManager was still in its own
  first scan at boot: 0 networks, on an image that boots faster than
  Raspberry Pi OS. It now uses `nmcli device wifi list --rescan yes`, which
  waits, and tries an empty result again.
- **Setup joined before the radio had looked.** After the access point goes
  down, NetworkManager's list is stale, and `nmcli device wifi connect` failed
  at once with "not found" on a network the page had just offered. It now
  waits for a scan that shows the network.
- **No CA certificates.** Every HTTPS request would have failed verification;
  `ca-certificates` is now selected by `porchlight-boot`.
- **The clock, at first boot.** A fresh card starts at systemd's built-in
  date, March 2026, which is before the server's certificate was issued, and
  setup called anything after 2025 "tls" - *could not be verified*. Setup now
  waits for timesyncd's `/run/systemd/timesync/synchronized` before the HTTPS
  check (about 20 s on the Pi), and OpenSSL's codes 9 and 10 read as "clock".
- **The access point's channel.** NetworkManager chose 6 one boot and 11 the
  next; on 11 the access point came and went every two minutes and a phone
  beside the Pi could not join. Setup pins channel 6.
- **The join, right after the access point.** Association timed out
  (`ASSOC-REJECT status_code=16`) three times within ten seconds of the
  access point going down, and NetworkManager reported that as missing
  secrets, so the page blamed the password. Setup retries three times, 15 s
  apart.

With all of them, on 2026-10-13: one Connect, 41 s to "joined SGisrael and
reached the app server".

Still open: the journal is kept in memory only, so a reboot takes the
evidence with it, and after the access point goes down the Wi-Fi country
briefly resets to the world domain - the `set chanspec ... fail` burst in
`dmesg` is that, and it is noise rather than a fault.

The Wi-Fi country is IL, from `rootfs-overlay/etc/modprobe.d/cfg80211.conf`
and `wireless-regdb`. Without one, every scan asked the chip for Japan-only
channels and logged `set chanspec ... fail, reason -52` for each.

## A/B updates: U-Boot and RAUC

The card is four partitions: **boot** (64 MB FAT: GPU firmware, U-Boot,
`boot.scr`, device trees, `config.txt`, `uboot.env`), **slot A** and
**slot B** (512 MB each: a whole root filesystem, each with its own kernel in
`/boot`), and **data** (`/data`: the credential, identity and config,
Wi-Fi profiles, SSH host keys, RAUC's status). An update writes the slot that
is not running; the data partition is never touched.

The firmware starts U-Boot; `boot.cmd` takes the first slot in
`BOOT_ORDER` with attempts left in `BOOT_x_LEFT`, spends one, saves, and
boots it. `porchlight-health` gives the attempts back (`rauc status
mark-good`) once porchlightd has run 60 s without restarting - steady, not
"reached the server", so an internet outage never rolls back a good update.
A freshly installed slot that never gets there is marked bad and the device
reboots into the other. A slot that runs out of attempts is skipped; if
both run out, both get three again rather than never booting.

```bash
image/buildroot/make-bundle.sh 2026.10.13-1        # -> output/images/porchlight-2026.10.13-1.raucb
ssh root@<pi> "cat > /data/update.raucb" < porchlight-2026.10.13-1.raucb
ssh root@<pi> "rauc install /data/update.raucb && reboot"
ssh root@<pi> "rauc status; fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT"
```

The signing key is in `~/porchlight-keys` on the build machine and nowhere
else; the image holds only its certificate (`package/porchlight-ab/keyring.pem`).

**The system log is on `/data` too** (`/data/journal`, bind-mounted at
`/var/log/journal`, held to 32 MB), so a boot that went wrong can be read on
the next good one: `journalctl --list-boots`, then `journalctl -b -1`. Each
slot has its own machine ID, so after an update the other slot's boots are in
a different directory: `journalctl -D /var/log/journal/<id>` reads them.

**Proven on the Pi, 2026-10-13:** a good bundle installed into the spare slot
in 25 s, booted, kept the identity, Wi-Fi and credential, and was marked good;
a bundle whose porchlightd fails was booted, judged on trial, marked bad after
five minutes, and the Pi went back to the slot before it on its own. The first
rollback attempt did not roll back - see `porchlight-health`: it trusted
`rauc status --output-format=json`, which this RAUC cannot produce, and an
empty answer meant "proven". It reads RAUC's status file now, and treats
anything it cannot read as on trial.

**A bundle updates the slots only.** The boot partition is shared by both
and nothing updates it after the factory: a change to `config.txt`,
`cmdline.txt`, `boot.scr`, U-Boot or the firmware needs a reflash, or an
update step of its own that does not exist yet.

**What the first boot on the Pi found** (2026-10-13), each fixed on the card
by hand, then here:

- **`dtoverlay=miniuart-bt` hangs U-Boot on the rainbow screen.** Buildroot's
  Pi config swaps the UARTs for a serial console; U-Boot, which sets up the
  serial port first thing, never gets further. Found by bisecting
  `config.txt` down to five lines and adding back. Removed; `enable_uart=1`
  stays. The kernel console is `serial0`, not `ttyAMA0`, which is now
  Bluetooth's.
- **A saved `fdt_addr` is stale.** U-Boot fills in where the firmware put
  this boot's device tree only if the variable is unset, and `saveenv` saved
  it - so the next boot, with different overlays, read garbage
  (`FDT_ERR_BADMAGIC`). `boot.cmd` copies it to `dtb`, deletes it, and saves.
- **U-Boot 2024.01, not Buildroot's 2026.01.** The faults above were the
  same on both; 2024.01 is what was proven, so it is pinned, with its hash in
  `board/porchlight/rpi4/patches/uboot/`. 2026.01 is untried with the fixes.
- **A failed boot waits at `U-Boot>` forever**, which on a doorbell is the
  same as dead. `boot.cmd` ends in `reset`, so the attempt is spent and the
  fallback happens.

The U-Boot prompt is how it was found: stop autoboot with a key, then
`printenv fdt_addr`, `md 0x03000000 4` (a device tree starts `d00dfeed`) and
`fdt addr 0x03000000; fdt header`. `uboot.env` on the FAT partition is
readable from Windows too: the counters say how many times it went round.

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

About an hour from clean on 4 cores: the kernel is half of it, systemd,
GLib and NetworkManager most of the rest. Buildroot remembers `BR2_EXTERNAL`,
so later commands in the same `~/buildroot` do not need it again. The result
is `output/images/sdcard.img`: a 32 MB FAT boot partition (firmware,
`config.txt`, kernel, device tree), mounted at `/boot/firmware` where
Raspberry Pi OS mounts it, and a 512 MB ext4 root, about 110 MB of it used.
The boot partition is 84% full, so the A/B layout will need a bigger one.

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

Switching on an option that changes *another* package's output is the same
trap. Adding the recorder turned on gobject-introspection, but libglib2 had
been built in Stage 1 without it, so it had never written `GLib-2.0.gir`, and
GStreamer failed with `Couldn't find include 'GLib-2.0.gir'`. The fix was
`brmake libglib2-dirclean all`; a clean build would never have hit it.

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
                        the bleeding-edge toolchain, systemd (networkd off),
                        NetworkManager with wpa_supplicant and dnsmasq, the
                        Pi's Wi-Fi firmware, dropbear, porchlightd, hostname
                        porchlight and the root password hash.
package/porchlightd/    The daemon, built from ../../porchlightd in this repo.
                        Selects libgpiod2, and json-for-modern-cpp so that
                        CMake finds nlohmann/json rather than fetching it from
                        GitHub mid-build. Installs /usr/local/bin/porchlightd
                        itself, because CMakeLists.txt has no install() rules,
                        the example config as /etc/porchlight/porchlightd.json,
                        and porchlightd.service, which preset-all enables.
                        Creates the porchlight user in audio, video and gpio,
                        and its spool directory.
package/porchlight-boot/
                        firstboot, setup and server-bridge.py from ../../pi,
                        and the two boot units, which preset-all enables.
                        Selects Python with ssl, python-gpiod,
                        python-websocket-client, dnsmasq and ca-certificates.
board/porchlight/rpi4/  Copies of Buildroot's Pi 4 files, now ours.
  config.txt            Read by the GPU firmware: kernel, 64-bit, overlays.
  cmdline.txt           The kernel command line: root=/dev/mmcblk0p2.
  post-build.sh         Runs before the root filesystem is packed: the HDMI
                        login prompt, the /boot/firmware line in fstab, and
                        the permissions on /root/.ssh, which a Windows
                        checkout cannot be trusted to carry.
  rootfs-overlay/       Copied over the root filesystem, permissions then set
                        by post-build.sh: root/.ssh/authorized_keys (a public
                        key), the Wi-Fi country, NetworkManager's
                        firewall-backend=none - the setup access point shares
                        nothing, and there is no nft or iptables to share with
                        - and the udev rule that gives /dev/gpiochip* to the
                        gpio group, without which porchlightd cannot open the
                        button, the PIR or the LED.
  post-image.sh         Runs genimage to make sdcard.img.
  genimage.cfg.in       The SD card's partition layout.
  patches/linux/linux.hash
                        SHA-256 of the kernel tarball. Downloads are refused
                        without a matching hash, so a new kernel needs a new one.
```
