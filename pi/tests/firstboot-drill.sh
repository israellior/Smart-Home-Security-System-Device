#!/bin/bash
# Every path through porchlight-firstboot.py, on a machine that is not a Pi.
#
# There is no Python test framework in this repository and this is not the
# place to introduce one: what firstboot does is write four files and delete a
# fifth, so a shell script that looks at the files is a more direct test than
# anything that mocks os.open. Nothing here needs root or a Pi - --etc points
# at a temp directory and --user is whoever is running it.
#
#   ./pi/tests/firstboot-drill.sh          # from the top of the tree
set -u
cd "$(dirname "$0")/../.." || exit 1
FB="python3 pi/porchlight-firstboot.py --no-hostname --user $(id -un)"

pass=0; fail=0
check() { # check <label> <expected-exit> <actual-exit>
  if [ "$2" -eq "$3" ]; then pass=$((pass+1)); printf '  ok   %s\n' "$1"
  else fail=$((fail+1)); printf '  FAIL %s (wanted exit %s, got %s)\n' "$1" "$2" "$3"; fi
}
says() { # says <label> <file> <needle>
  if grep -q "$3" "$2" 2>/dev/null; then pass=$((pass+1)); printf '  ok   %s\n' "$1"
  else fail=$((fail+1)); printf '  FAIL %s (%s does not contain %s)\n' "$1" "$2" "$3"; fi
}
absent() {
  if [ ! -e "$2" ]; then pass=$((pass+1)); printf '  ok   %s\n' "$1"
  else fail=$((fail+1)); printf '  FAIL %s (%s still exists)\n' "$1" "$2"; fi
}

new_card() { # new_card <dir> <json>
  rm -rf "$1"; mkdir -p "$1/boot" "$1/etc"
  cp porchlightd/porchlightd.example.json "$1/etc/porchlightd.json"
  printf '%s' "$2" > "$1/boot/porchlight.json"
}

W=$(mktemp -d)

echo "== a card that was never minted =="
rm -rf "$W/a"; mkdir -p "$W/a/boot" "$W/a/etc"
$FB --boot "$W/a/boot" --etc "$W/a/etc" 2>"$W/a.log"; check "exits 2" 2 $?
says "says the card was never minted" "$W/a.log" "never minted"

echo
echo "== a good card =="
new_card "$W/b" '{"deviceId":"porch-1","credential":"pl_porch-1_abc123","url":"https://porchlight.example","claimCode":"7K2M9P","setupSsid":"Porchlight-7K2M","setupPassword":"hunter2hunter"}'
$FB --boot "$W/b/boot" --etc "$W/b/etc" 2>"$W/b.log"; check "exits 0" 0 $?
says "credential is written verbatim" "$W/b/etc/credential" "pl_porch-1_abc123"
printf '  ..   credential mode %s, bytes %s\n' \
  "$(stat -c '%a' "$W/b/etc/credential")" "$(stat -c '%s' "$W/b/etc/credential")"
[ "$(stat -c '%a' "$W/b/etc/credential")" = "600" ] && pass=$((pass+1)) ||
  { fail=$((fail+1)); echo "  FAIL credential is not 0600"; }
[ "$(stat -c '%s' "$W/b/etc/credential")" = "17" ] && pass=$((pass+1)) ||
  { fail=$((fail+1)); echo "  FAIL credential has a trailing newline"; }
says "identity carries the claim code" "$W/b/etc/identity.json" "7K2M9P"
says "identity carries the ap password" "$W/b/etc/identity.json" "hunter2hunter"
python3 - "$W/b/etc/identity.json" <<'PY'
import json,sys
i=json.load(open(sys.argv[1]))
assert "credential" not in i, "identity.json must not carry the credential"
print("  ok   identity.json holds no credential")
PY
says "config takes the device id" "$W/b/etc/porchlightd.json" '"device_id": "porch-1"'
says "config takes the url" "$W/b/etc/porchlightd.json" "porchlight.example"
python3 - "$W/b/etc/porchlightd.json" <<'PY'
import json,sys
c=json.load(open(sys.argv[1]))
assert c["media"]["python"], "the rest of the config survived"
assert c["gpio"]["button_line"] == 24
print("  ok   the rest of the shipped config is untouched")
PY
absent "the boot partition is clean" "$W/b/boot/porchlight.json"

echo
echo "== running it again, as every later boot does =="
$FB --boot "$W/b/boot" --etc "$W/b/etc" 2>"$W/b2.log"; check "exits 0" 0 $?
says "says it is already provisioned" "$W/b2.log" "already provisioned"

echo
echo "== a card whose credential belongs to another device =="
new_card "$W/c" '{"deviceId":"porch-1","credential":"pl_porch-2_abc123","url":"https://porchlight.example"}'
$FB --boot "$W/c/boot" --etc "$W/c/etc" 2>"$W/c.log"; check "exits 1" 1 $?
says "names the mismatch" "$W/c.log" "not this device"
absent "no credential was written" "$W/c/etc/credential"
if [ -e "$W/c/boot/porchlight.json" ]; then echo "  ok   the bad certificate is left for inspection"; pass=$((pass+1));
else echo "  FAIL the bad certificate was destroyed"; fail=$((fail+1)); fi

echo
echo "== a card with a nonsense device id =="
new_card "$W/d" '{"deviceId":"Porch 1","credential":"pl_Porch 1_abc","url":"https://x.example"}'
$FB --boot "$W/d/boot" --etc "$W/d/etc" 2>"$W/d.log"; check "exits 1" 1 $?
says "names the slug" "$W/d.log" "valid slug"

echo
echo "== a card with a url that is not http =="
new_card "$W/e" '{"deviceId":"porch-1","credential":"pl_porch-1_abc","url":"porchlight.example"}'
$FB --boot "$W/e/boot" --etc "$W/e/etc" 2>"$W/e.log"; check "exits 1" 1 $?
says "names the url" "$W/e.log" "is not http"

echo
echo "== truncated json, the classic half-written card =="
new_card "$W/f" '{"deviceId":"porch-1","cred'
$FB --boot "$W/f/boot" --etc "$W/f/etc" 2>"$W/f.log"; check "exits 1" 1 $?
says "says it cannot read it" "$W/f.log" "cannot read"

echo
echo "== --dry-run changes nothing =="
new_card "$W/g" '{"deviceId":"porch-9","credential":"pl_porch-9_abc","url":"https://x.example"}'
$FB --boot "$W/g/boot" --etc "$W/g/etc" --dry-run 2>"$W/g.log"; check "exits 0" 0 $?
absent "wrote no credential" "$W/g/etc/credential"
if [ -e "$W/g/boot/porchlight.json" ]; then echo "  ok   left the certificate alone"; pass=$((pass+1));
else echo "  FAIL destroyed the certificate in a dry run"; fail=$((fail+1)); fi
says "said what it would do" "$W/g.log" "would write"

echo
echo "-- $pass passed, $fail failed --"
rm -rf "$W"
[ "$fail" -eq 0 ]
