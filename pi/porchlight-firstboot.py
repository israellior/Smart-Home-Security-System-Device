#!/usr/bin/env python3
"""Turns one identical card into this particular doorbell, once, at first boot.

Every unit ships the same image. What makes a unit itself is a single file on
the FAT boot partition, written by the factory after flashing:

    /boot/firmware/porchlight.json
    {
      "deviceId": "porch-1",
      "credential": "pl_porch-1_...",
      "url": "https://porchlight.example",
      "claimCode": "7K2M9P",
      "setupSsid": "Porchlight-7K2M",
      "setupPassword": "...",
      "name": "Front Door"
    }

FAT is the only partition a Windows or a Mac laptop can write without help, so
that is where the production line puts it. It is also readable by anyone who
takes the card out, which is why the first thing this does is move the secret
off it.

What it does, in order, and all of it idempotent:

  1. reads the birth certificate
  2. writes the credential to /etc/porchlight/credential, 0600, owned by the
     daemon's user
  3. writes /etc/porchlight/identity.json - the non-secret half, plus the
     setup access point's own password, for porchlight-setup.py to serve
  4. sets device_id and server.base_url in porchlightd.json
  5. sets the hostname from the device id
  6. overwrites and removes the file from the boot partition
  7. leaves a marker so it never runs again

Run by porchlight-firstboot.service, before everything else. It acts by
default, unlike provision.sh, because systemd is what runs it and there is
nobody there to type --apply; --dry-run is for looking.

    ./porchlight-firstboot.py --dry-run
    ./porchlight-firstboot.py --boot /mnt/boot --etc /tmp/etc --dry-run

Exit codes matter to the unit: 0 done or already done, 2 no birth certificate
(the card was never minted - a factory fault, and porchlightd will light the
fault pattern on its own), 1 something went wrong.
"""

import argparse
import json
import os
import pwd
import re
import subprocess
import sys

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_UNMINTED = 2

# Matches the server's own DEVICE_ID_PATTERN. It ends up in a hostname, in
# URLs, in the LiveKit room name and inside the credential, so it is checked
# here rather than trusted - a birth certificate is written by a machine, but
# it is written by a machine somebody wrote.
DEVICE_ID = re.compile(r"^[a-z0-9][a-z0-9-]{1,31}$")

# The non-secret half of the birth certificate, plus the access point's own
# password. Kept 0600 and root-owned: it is no more secret than the sticker on
# the back of the case, but nothing except the setup service needs to read it.
IDENTITY_FIELDS = ("deviceId", "name", "url", "claimCode", "setupSsid", "setupPassword")


def note(text):
    sys.stderr.write(f"firstboot: {text}\n")
    sys.stderr.flush()


class Firstboot:
    def __init__(self, options):
        self.boot = options.boot
        self.etc = options.etc
        self.config = options.config or os.path.join(self.etc, "porchlightd.json")
        self.user = options.user
        self.dry_run = options.dry_run
        self.set_hostname = not options.no_hostname

    # --- the steps --------------------------------------------------------

    def run(self):
        certificate_path = os.path.join(self.boot, "porchlight.json")
        marker = os.path.join(self.etc, "provisioned")

        if os.path.exists(marker) and not os.path.exists(certificate_path):
            note("already provisioned; nothing to do")
            return EXIT_OK

        if not os.path.exists(certificate_path):
            note(f"no birth certificate at {certificate_path}.")
            note("This card was never minted. Mint the device on the server and")
            note("write the file onto the boot partition; porchlightd will show")
            note("the fault pattern until then.")
            return EXIT_UNMINTED

        try:
            with open(certificate_path) as handle:
                certificate = json.load(handle)
        except (OSError, ValueError) as error:
            note(f"cannot read {certificate_path}: {error}")
            return EXIT_FAILED

        problem = self.check(certificate)
        if problem:
            # Deliberately not removed. A malformed certificate is a factory
            # mistake, and leaving it on a partition any laptop can read is how
            # somebody works out what was wrong with it.
            note(f"{certificate_path} is not usable: {problem}")
            return EXIT_FAILED

        device_id = certificate["deviceId"]
        note(f"provisioning as {device_id}")

        try:
            self.write_credential(certificate["credential"])
            self.write_identity(certificate)
            self.patch_config(device_id, certificate["url"])
            if self.set_hostname:
                self.write_hostname(device_id)
        except Exception as error:  # any failure here leaves the card as it was
            note(f"failed: {error}")
            return EXIT_FAILED

        # Last, and only once everything above worked. A card that lost power
        # halfway through has to be able to run this again, and it can only do
        # that while the certificate is still there.
        self.destroy(certificate_path)
        self.touch(marker)
        note("done")
        return EXIT_OK

    def check(self, certificate):
        for field in ("deviceId", "credential", "url"):
            if not certificate.get(field):
                return f"no {field!r}"
        device_id = certificate["deviceId"]
        if not DEVICE_ID.match(device_id):
            return f"deviceId {device_id!r} is not a valid slug"
        # The credential carries the device id, so the two can disagree - and
        # if they do, every request this device ever makes is refused with a
        # message about the credential rather than about the mint that made it.
        if not certificate["credential"].startswith(f"pl_{device_id}_"):
            return "the credential is not this device's"
        if not certificate["url"].startswith(("http://", "https://")):
            return f"url {certificate['url']!r} is not http(s)"
        return None

    def write_credential(self, credential):
        path = os.path.join(self.etc, "credential")
        # printf-style: no trailing newline. The server compares it verbatim,
        # and nothing should depend on the reader stripping whitespace.
        self.write(path, credential, mode=0o600, owner=self.user)

    def write_identity(self, certificate):
        identity = {field: certificate.get(field) for field in IDENTITY_FIELDS}
        path = os.path.join(self.etc, "identity.json")
        self.write(path, json.dumps(identity, indent=2) + "\n", mode=0o600, owner=None)

    def patch_config(self, device_id, url):
        """Two fields, in place, leaving everything else the image shipped."""
        if not os.path.exists(self.config):
            note(f"no {self.config} to patch - the image should ship one")
            return
        with open(self.config) as handle:
            config = json.load(handle)
        config["device_id"] = device_id
        config.setdefault("server", {})["base_url"] = url
        self.write(self.config, json.dumps(config, indent=2) + "\n", mode=0o644, owner=None)

    def write_hostname(self, device_id):
        """So a router's client list says which doorbell, not raspberrypi."""
        if self.dry_run:
            note(f"would set the hostname to {device_id}")
            return
        try:
            subprocess.run(["hostnamectl", "set-hostname", device_id], check=True,
                           capture_output=True)
        except (OSError, subprocess.CalledProcessError) as error:
            # Not fatal. A doorbell with the wrong hostname is a doorbell.
            note(f"could not set the hostname: {error}")

    def destroy(self, path):
        """Overwrite, then unlink. Best effort, and worth being honest about.

        On a wear-levelled SD card an overwrite may land on a different block
        and leave the original readable to anyone with the right tools. What
        actually protects this credential is that it is per-device and can be
        re-minted, which revokes it. This raises the cost of a casual look at
        the card, which is the threat that exists.
        """
        if self.dry_run:
            note(f"would overwrite and remove {path}")
            return
        try:
            size = os.path.getsize(path)
            with open(path, "r+b", buffering=0) as handle:
                handle.write(os.urandom(max(size, 1)))
                handle.flush()
                os.fsync(handle.fileno())
            os.remove(path)
            note(f"removed {path} from the boot partition")
        except OSError as error:
            note(f"could not remove {path}: {error}")

    def touch(self, path):
        self.write(path, "", mode=0o644, owner=None)

    # --- writing ----------------------------------------------------------

    def write(self, path, content, mode, owner):
        if self.dry_run:
            note(f"would write {path} ({mode:#o}"
                 f"{', owned by ' + owner if owner else ''})")
            return
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # Written with the final mode from the start rather than chmod'd after:
        # between creat() and chmod() a 0644 credential is a readable one.
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
        with os.fdopen(descriptor, "w") as handle:
            handle.write(content)
        os.chmod(path, mode)
        if owner:
            self.chown(path, owner)

    def chown(self, path, owner):
        try:
            entry = pwd.getpwnam(owner)
        except KeyError:
            note(f"no user {owner!r}; leaving {path} owned by root")
            return
        try:
            os.chown(path, entry.pw_uid, entry.pw_gid)
        except OSError as error:
            note(f"could not give {path} to {owner}: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--boot", default="/boot/firmware",
                        help="where the birth certificate is (default /boot/firmware)")
    parser.add_argument("--etc", default="/etc/porchlight")
    parser.add_argument("--config", default=None,
                        help="porchlightd.json (default <etc>/porchlightd.json)")
    parser.add_argument("--user", default="porchlight",
                        help="who the credential belongs to (default porchlight)")
    parser.add_argument("--no-hostname", action="store_true")
    parser.add_argument("--dry-run", action="store_true",
                        help="say what would happen and change nothing")
    options = parser.parse_args()

    if not options.dry_run and os.geteuid() != 0 and options.etc.startswith("/etc"):
        note("this writes /etc/porchlight and needs root")
        return EXIT_FAILED

    return Firstboot(options).run()


if __name__ == "__main__":
    sys.exit(main())
