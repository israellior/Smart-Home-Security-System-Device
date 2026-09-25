#!/bin/bash
# The setup flow end to end on a machine with no radio, using --fake-nm.
#
# What this can prove: the state machine, the page, the captive-portal probes,
# that a refused password comes back as a sentence rather than a stack trace,
# and that a good password ends the process with exit 0 - which is the signal
# systemd uses to start porchlightd.
#
# What it cannot prove, and nothing on a development host can: nmcli, the
# access point, the scan-before-AP ordering, the dnsmasq redirect, and the LED.
# Those need a Pi with a radio.
#
#   ./pi/tests/setup-drill.sh
set -u
cd "$(dirname "$0")/../.." || exit 1

PORT=${PORT:-8642}
W=$(mktemp -d)
pass=0; fail=0

ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; }
says() { if grep -q "$2" "$3" 2>/dev/null; then ok "$1"; else bad "$1"; fi; }

cat > "$W/identity.json" <<'JSON'
{
  "deviceId": "porch-1",
  "url": "https://porchlight.example",
  "claimCode": "7K2M9P",
  "setupSsid": "Porchlight-7K2M",
  "setupPassword": "hunter2hunter",
  "name": "Front Door"
}
JSON
printf '%s' 'pl_porch-1_secret' > "$W/credential"

echo "== --check says what it would do, and changes nothing =="
python3 pi/porchlight-setup.py --fake-nm --check \
  --identity "$W/identity.json" --credential-file "$W/credential" 2>"$W/check.log"
[ $? -eq 0 ] && ok "exits 0" || bad "exits 0"
says "reports the device" "porch-1" "$W/check.log"
says "would raise the access point" "would raise the access point" "$W/check.log"

echo
echo "== the whole flow =="
python3 pi/porchlight-setup.py --fake-nm --no-led --port "$PORT" \
  --identity "$W/identity.json" --credential-file "$W/credential" \
  > "$W/out.log" 2>&1 &
SETUP=$!
for _ in $(seq 40); do
  curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null && break
  sleep 0.25
done

curl -fsS "http://127.0.0.1:$PORT/" -o "$W/page.html" && ok "serves the page" || bad "serves the page"
says "offers the networks it scanned" "Wombat" "$W/page.html"
says "shows the signal strength" "&#9608;\|▊\|█\|▆\|▄\|▂" "$W/page.html"
says "shows the claim code" "7K2M9P" "$W/page.html"
says "shows the doorbell's name" "Front Door" "$W/page.html"
says "warns that the network will disappear" "disappear" "$W/page.html"
if grep -q "hunter2hunter\|pl_porch-1_secret" "$W/page.html"; then
  bad "the page leaks a secret"
else
  ok "the page carries no credential and no ap password"
fi

echo
echo "== captive portal probes =="
for probe in /hotspot-detect.html /generate_204 /ncsi.txt /connecttest.txt; do
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT$probe")
  [ "$code" = "302" ] && ok "$probe redirects ($code)" || bad "$probe answered $code, wanted 302"
done
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/anything/else")
[ "$code" = "302" ] && ok "an unknown path redirects too" || bad "unknown path answered $code"

echo
echo "== a password the router refuses =="
curl -fsS -X POST -d 'ssid=Wombat&psk=wrong' "http://127.0.0.1:$PORT/wifi" -o "$W/sent.html" \
  && ok "the POST answers at once, before the radio moves" || bad "the POST answers at once"
says "says which network it is trying" "Wombat" "$W/sent.html"
sleep 2
curl -fsS "http://127.0.0.1:$PORT/" -o "$W/after.html" && ok "the page is back after the failure" \
  || bad "the page is back after the failure"
says "explains the password was refused" "password was not accepted" "$W/after.html"
kill -0 $SETUP 2>/dev/null && ok "still running, so the customer can try again" \
  || bad "the process gave up on a failed attempt"

echo
echo "== the password that works =="
curl -fsS -X POST -d 'ssid=Wombat&psk=correct' "http://127.0.0.1:$PORT/wifi" -o /dev/null \
  && ok "accepted" || bad "accepted"
for _ in $(seq 40); do kill -0 $SETUP 2>/dev/null || break; sleep 0.25; done
wait $SETUP; code=$?
[ "$code" -eq 0 ] && ok "exits 0, which is what starts porchlightd" \
  || bad "exited $code, so systemd would not hand over"
says "says the handover" "porchlightd takes it from here" "$W/out.log"
says "scanned before raising the access point" "network(s) in range" "$W/out.log"
first=$(grep -n "network(s) in range" "$W/out.log" | head -1 | cut -d: -f1)
second=$(grep -n "access point Porchlight-7K2M up" "$W/out.log" | head -1 | cut -d: -f1)
if [ -n "$first" ] && [ -n "$second" ] && [ "$first" -lt "$second" ]; then
  ok "the scan happens before the access point, not after"
else
  bad "the access point came up before the scan"
fi

echo
echo "-- $pass passed, $fail failed --"
kill $SETUP 2>/dev/null
rm -rf "$W"
[ "$fail" -eq 0 ]
