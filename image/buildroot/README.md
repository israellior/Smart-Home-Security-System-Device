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

**The camera runs on upstream libcamera, with one patch of ours.** Upstream
v0.7.0 has `CameraConfiguration::sensorConfig`, and the Pi's pipeline handler
honours it, but its `libcamerasrc` had no property to set it. Left to choose,
libcamera picks the IMX708's binned 1536x864 mode for a 640x360 stream, which
reads only the centre of the sensor: about 98° of the wide lens's 120°.
`patches/libcamera/0001-…sensor-config…patch` adds the property, in the
format Raspberry Pi's fork uses, so `record-clip.py` and `webrtc-video.py`
needed no change. One small patch was the alternative to building the whole
fork as a package of our own and keeping it in step. Since release-9 every
clip and call logs `Selected sensor format: 2304x1296`, and the call holds
30 fps in it.

Its DMA heaps also needed a udev rule (`60-dma-heap.rules`): without it
libcamera, running as `porchlight`, logged `Could not open any dma-buf
provider` and found no camera.

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
- **DNSSEC, with that clock.** On the release card (2026-10-14) every Connect
  failed as `no-dns`: systemd-resolved validated DNSSEC, and with the clock in
  March every signature read `signature-expired` - and the clock could never
  be set, because timesyncd needs DNS to find an NTP server. Earlier cards got
  lucky with which answers were validated. `rootfs-overlay/etc/systemd/
  resolved.conf.d/porchlight.conf` turns validation off; TLS already proves
  the server's name, and setup waits for the clock before that check.
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

Still open: after the access point goes down the Wi-Fi country
briefly resets to the world domain - the `set chanspec ... fail` burst in
`dmesg` is that, and it is noise rather than a fault.

The Wi-Fi country is IL, from `rootfs-overlay/etc/modprobe.d/cfg80211.conf`
and `wireless-regdb`. Without one, every scan asked the chip for Japan-only
channels and logged `set chanspec ... fail, reason -52` for each.

## A/B updates: U-Boot and RAUC

The card is U-Boot's environment in raw sectors, then four partitions on
4 MiB boundaries: **boot** (64 MB FAT, read-only: GPU firmware, U-Boot,
`boot.scr`, device trees, `config.txt`), **slot A** and
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
the next good one: `journalctl --list-boots`, then `journalctl -b -1`. Both
slots are one machine to it (see the machine ID, below), so the history runs
straight across updates. A card from before release-4 has one directory per
slot instead: `journalctl -D /var/log/journal/<id>` reads the others.

**Proven on the Pi, 2026-10-13:** a good bundle installed into the spare slot
in 25 s, booted, kept the identity, Wi-Fi and credential, and was marked good;
a bundle whose porchlightd fails was booted, judged on trial, marked bad after
five minutes, and the Pi went back to the slot before it on its own. The first
rollback attempt did not roll back - see `porchlight-health`: it trusted
`rauc status --output-format=json`, which this RAUC cannot produce, and an
empty answer meant "proven". It reads RAUC's status file now, and treats
anything it cannot read as on trial.

**Proven again from a freshly flashed release card, 2026-10-14**: birth
certificate, phone setup, a press (chime, ring, a clip uploaded), Watch both
ways; then release-2 into B, kept porch-2 and came up on trial and good; then
a broken release-2 into A, which was on trial, marked bad at 318 s, and the
Pi was back on B at 06:18 with nobody touching it; then release-3 into A,
good, with B still proven. Two more faults in `porchlight-health`, both only
reachable on a real device:

- **BusyBox awk's `printf "%d"` is 32-bit.** Uptime in microseconds stuck at
  2147483647 after 35.8 minutes, so a boot whose phone setup took longer
  than that never ended its 60 s wait or its deadline, and the slot was never
  marked good. It counts whole seconds in the shell now, which is 64-bit.
- **RAUC keeps `installed.transaction` only for the latest install.**
  Installing into A deleted B's, so B - already proven - came back on trial,
  where a struggling daemon would roll it back. Installs are judged by
  `installed.timestamp`, which RAUC never drops.

**Not done, and known:** the image still carries development access (a root
password hash and an SSH key), and bundles are signed with a development key
kept in WSL. A product needs both answered: no shell access, and a signing
key that lives somewhere guarded.

**A bundle updates the slots only.** The boot partition is shared by both
and nothing updates it after the factory: a change to `config.txt`,
`cmdline.txt`, `boot.scr`, U-Boot or the firmware needs a reflash, or an
update step of its own that does not exist yet.

## A read-only system slot

**From release-4 (2026-10-14).** The slot mounts `ro` and stays exactly as
the bundle wrote it, so a power cut has nothing on it to damage. What is
written goes to RAM, gone at reboot, or to `/data`, kept:

| Where | What |
|---|---|
| `/var` | A tmpfs, filled at every boot from the image's copy in `/usr/share/factory/var` - Buildroot's "var factory", which is what `# BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW is not set` brings. Packages' state, the clip spool (`porchlightd.service` creates it, since nothing made at install time survives) |
| `/data/journal` | The log, bind-mounted at `/var/log/journal`, synced every 30 s |
| `/data/timesync` | timesyncd's clock file, bind-mounted at `/var/lib/systemd/timesync`: a reboot starts at the right date, not at the build's |
| U-Boot's environment | The machine ID (below), beside the slot counters |

To change a file by hand for a test: `mount -o remount,rw /`, edit,
`mount -o remount,ro /`. The next update replaces it anyway.

**The machine ID.** systemd names the journal's directory after it, and on a
read-only `/etc` it cannot save one, so it makes up a new one every boot -
and `journalctl -b -1` would find nothing. The first boot's is saved in
U-Boot's environment by `porchlight-data` (`fw_setenv machine_id`), and `boot.cmd`
passes it back as `systemd.machine_id=`. Every later boot, in either slot, is
the same machine, and the first boot's log is already filed under that ID.

**The hostname** is set from `/data/porchlight/identity.json` at every boot
by `porchlight-data`, writing nothing. `/etc/hostname` says `porchlight`,
read-only, so NetworkManager has `hostname-mode=none`: by default it put that
name back whenever Wi-Fi came up. firstboot sets the name for its own boot.

**What the drills found**, each fixed:

- **No update could install.** RAUC mounts a bundle under `/mnt/rauc`, on the
  read-only root. `mount-prefix=/run/rauc` now - but release-4 shipped
  without it, so a device on release-4 updates only with
  `rauc --mount=/run/rauc install …`. A release that runs is not a release
  that can be left: test the update *from* every release, not just the
  release.
- **A power cut lost the last five minutes of log**, journald's default
  sync interval - including the button press the cut was timed against.
  `SyncIntervalSec=30s`.
- **"fsck failed" at every boot.** The root had fsck pass 1 and the image has
  no `fsck.ext4`; a read-only root needs no check. Pass 0.
- **The fix for the first one was spelled wrong, and nothing said so.**
  Release-6 had `mount-prefix=`; RAUC's key is `mountprefix=`, unlike the
  keys around it, and RAUC refused the whole file: no install, no status, no
  mark-good. `porchlight-health` threw mark-good's exit code away and printed
  "marked good", so slot B's attempts ran 3, 2, 1 with every boot looking
  healthy. Now a failed mark-good fails the unit, and `make-bundle.sh` loads
  the image's own `system.conf` with the host's RAUC and leaves no bundle if
  RAUC refuses it - proven against release-6's root filesystem. The check
  that had passed release-6 read the line in the file; only the program
  reading it could say whether it was right.
- **Every SSH login took eight seconds.** dropbear reverse-resolves each
  client, and systemd-resolved sent that out over mDNS and waited 7.6 s for
  an answer nobody gives; scripts with a 4 s connect timeout read it as a
  dead Pi. mDNS and LLMNR are off (`resolved.conf.d/porchlight.conf`);
  logins take under a second. `porch-2.local` no longer resolves - use the
  address.

**Proven on the Pi, 2026-10-14**, from a flashed release-4 card: setup from a
phone, root `ro`, no failed units; a reboot kept the machine ID and
`journalctl -b -1` read the boot before; the power pulled during a recording,
and the same slot booted, `/data` replayed its journal and kept the
credential and Wi-Fi; release-6 installed into B, which came up as porch-2
with the same machine ID, on trial and then good - except that "good" was
the bug above; release-7 into A was marked good with RAUC's own word for it
and `BOOT_A_LEFT=3` to show.

**The hardware watchdog, proven the same day.** `kernel.panic=0`, so a
crashed kernel never reboots itself, then `echo c > /proc/sysrq-trigger`:
the Pi stopped answering at +4 s and answered again at +32 s, reset by the
BCM2835's watchdog because systemd (`RuntimeWatchdogSec=30s`) had stopped
feeding it. Without one, a doorbell that hangs stays hung until someone
pulls its plug.

## U-Boot's environment, twice, in raw sectors

**From release-8 (2026-10-14), which needs a reflash: the card's layout
changed.** The power-cut drill showed the last weak point: `uboot.env` was a
file on the FAT, shared by both slots, rewritten by U-Boot at every boot and
by RAUC at every mark-good. A power cut inside that write could leave no slot
bootable, which neither A/B nor a read-only root protects.

Now U-Boot keeps its environment in raw sectors (`uboot.fragment`:
`ENV_IS_IN_MMC`, `SYS_REDUNDAND_ENVIRONMENT`), at 1 MiB and 2 MiB, and
`fw_env.config` names the same two places for Linux. Each copy is a CRC, a
one-byte counter and the variables; a save goes to the older copy with the
next count, so a torn write leaves a copy whose CRC fails, and the other is
used. The FAT is mounted read-only; only firstboot's unit opens it, and only
while there is a birth certificate to destroy.

There was no room for this: p1 began at sector 1. `genimage.cfg.in` now
aligns every partition to 4 MiB, which also stops them straddling the card's
erase blocks - the old p2 to p4 began at odd sectors.

To look at it: `fw_printenv`, or each copy's counter with
`dd if=/dev/mmcblk0 bs=1 skip=$((0x100000+4)) count=1 | od -An -tu1`.

**Proven on the Pi:** after a reflash both copies were valid, counters 1 and
2. Then the newest was torn on purpose - 64 bytes of its variables
overwritten, as a cut-off write would leave it - and Linux read the other
copy at once. The Pi rebooted normally, with the same machine ID and the log
unbroken; U-Boot's next save went over the torn copy, and the next mark-good
gave back the one attempt the lost save had cost. With `uboot.env` that was a
doorbell that did not boot.

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
`fdt addr 0x03000000; fdt header`. Before release-8, `uboot.env` on the FAT
partition was readable from Windows too, and its counters said how many times
it went round; now `fw_printenv` on the Pi, or `printenv` at the prompt.

## A smaller image

**From release-10 (2026-10-15), a clean build:** the system slot from
296 MiB to 228, the update bundle from 120 MB to 89 - what every device
downloads. Measured first, cut in order of what it cost:

| | Before | After |
|---|---|---|
| Kernel modules | 29 MiB, 1904 files | 2.7 MiB, 72 |
| FFmpeg | 19 MiB | gone: `voaacenc` encodes the AAC |
| udev's hwdb | 22 MiB | gone (`# BR2_PACKAGE_SYSTEMD_HWDB is not set`) |
| Kernel `Image` | 24 MiB | 23 MiB |
| Python, mostly LiveKit's Rust library and numpy | 84 MiB | 84 MiB: the call needs it |

**The kernel** is `linux.config`: Raspberry Pi's `bcm2711_defconfig` cut by
`make localmodconfig` to `kernel-modules.txt`, the modules the Pi loads.
`localmodconfig` turns off modules and leaves built-in drivers alone, which is
why `Image` barely moved; cutting those is the next round, by hand.

The list is the lesson. A normal boot's `lsmod` did not have `dm_mod`,
`dm_verity`, `dm_bufio` or `squashfs`, which load only while RAUC opens a
bundle - a kernel cut to it would have run perfectly and never installed
another update. So the list was taken after a clip, a call and
`rauc mount` of a bundle, and `post-build.sh` refuses an image missing any
module on it. On the first build it did: `libaes` and `snd_compress`, both
only dependencies of things removed on purpose (Bluetooth; compressed audio
offload) - so they left the list, with the reason written in it.

**AAC without FFmpeg.** `voaacenc` is the default now. A device's config is
on `/data`, which no update touches, and porch-2's said `avenc_aac`: on this
image that clip would not have parsed. `record-clip.py` swaps a missing AAC
encoder for one the system has, and logs it - `avenc_aac is not on this
system; recording AAC with voaacenc`.

**Buildroot never removes what a package installed.** Turning FFmpeg and the
hwdb off and rebuilding leaves their files in `output/target`; only a clean
build (`make clean all`, three hours here) shows the real size.

**Proven on the Pi** (release-10 as a bundle): 0 failed units and no driver
errors; Wi-Fi, the sound card and the camera; a chime and a clip with sound,
recorded through the fallback; a live call at 30 fps with the microphone,
ended by itself; and `rauc mount` of a bundle through dm-verity, so the
update path survived the cut. Talk-back was not used in that call; the
speaker it plays through played the chime.

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
package/porchlight-recorder/
                        record-clip.py and upload-clip.py, and the GStreamer
                        and libcamera pieces a clip needs.
package/python-livekit/ The LiveKit SDK from its prebuilt aarch64 wheel: its
                        FFI library is Rust, and the wheel is what upstream
                        ships, checked by hash.
package/porchlight-call/
                        webrtc-video.py, the elements the call adds, and the
                        /opt/porchlight/venv/bin/python3 the daemon's config
                        names, as a link to the system Python.
package/porchlight-ab/  A/B: RAUC's system.conf and keyring, fw_env.config for
                        U-Boot's two raw environment copies, the hardware
                        watchdog, the journal on /data, NetworkManager's
                        profiles on /data, porchlight-data (prepares /data
                        before anything reads it), porchlight-health (the 60 s
                        verdict) and firstboot's drop-in that opens the
                        read-only boot partition while it needs to.
make-bundle.sh          A signed update bundle from the built root filesystem,
                        refused if the host's RAUC rejects its system.conf.
board/porchlight/rpi4/  Copies of Buildroot's Pi 4 files, now ours.
  config.txt            Read by the GPU firmware: U-Boot, 64-bit, overlays.
  cmdline.txt           The firmware's half of the kernel command line;
                        boot.cmd adds root=, rauc.slot= and panic=.
  boot.cmd              U-Boot's script: choose a slot, spend an attempt,
                        boot it, and reset if it does not.
  uboot.fragment        Merged into U-Boot's rpi_4 config: its environment in
                        two raw copies at 1 and 2 MiB, not a FAT file.
  post-build.sh         Runs before the root filesystem is packed: the HDMI
                        login prompt, fstab for / (no fsck), /boot/firmware
                        (read-only) and /data, the
                        links that put /etc/porchlight and dropbear's keys on
                        /data, and permissions a Windows checkout cannot be
                        trusted to carry.
  rootfs-overlay/       Copied over the root filesystem, permissions then set
                        by post-build.sh: root/.ssh/authorized_keys (a public
                        key), the Wi-Fi country, NetworkManager's
                        firewall-backend=none - the setup access point shares
                        nothing, and there is no nft or iptables to share with
                        - and its hostname-mode=none, setup's captive-portal
                        file, systemd-resolved without DNSSEC, mDNS or LLMNR,
                        the mixer state, and the udev rules that give
                        /dev/gpiochip* to gpio and /dev/dma_heap to video.
  post-image.sh         Runs genimage to make sdcard.img, with boot.scr.
  genimage.cfg.in       The SD card: the environment's gap, then boot, slot
                        A, slot B and data, each on a 4 MiB boundary.
  patches/*/*.hash      SHA-256 of the kernel and U-Boot tarballs. Downloads
                        are refused without a matching hash.
  linux.config          The kernel's config: bcm2711_defconfig cut to the
                        modules the doorbell loads, in defconfig form.
  kernel-modules.txt    Those modules, taken on the Pi; post-build.sh refuses
                        an image that lacks one.
  patches/libcamera/    libcamerasrc's sensor-config, the one change we carry
                        on upstream libcamera. Applied when libcamera is
                        unpacked, so a new patch needs libcamera-dirclean.
```
