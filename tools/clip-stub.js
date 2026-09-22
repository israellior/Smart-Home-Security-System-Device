#!/usr/bin/env node
// The app server's three clip endpoints, and a bucket, faked well enough to
// exercise the device side of an upload before the real ones exist.
//
//   POST /api/clips/<eventId>/upload-url   -> a signed-looking PUT url
//   PUT  /bucket/<opaque>                  -> the bytes, written to --out
//   POST /api/clips/<eventId>/confirm      -> the metadata, printed in full
//
// It is not a model of the real server and must never become one. What it is
// for is the half of the contract that is hard to provoke against a real
// server: a confirm that fails, a url that has expired, a PUT that succeeds
// and a confirm that then does not. The device is supposed to survive all of
// those, and this is how you watch it try.
//
//   node tools/clip-stub.js --out ./received
//   node tools/clip-stub.js --fail confirm          # never lets a clip finish
//   node tools/clip-stub.js --fail-once confirm     # the retry then works
//   node tools/clip-stub.js --fail-once put --expire-after 1
//
// The credential is not checked against anything - any non-empty bearer token
// passes - but its absence is a 401, because a device that forgets to send it
// should fail here rather than at the real server.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const args = process.argv.slice(2);
const flag = (name, fallback = null) => {
  const at = args.indexOf(name);
  return at === -1 ? fallback : args[at + 1];
};
const has = (name) => args.includes(name);

const PORT = Number(flag('--port', 4100));
const OUT = path.resolve(flag('--out', path.join(__dirname, '..', 'received')));
const FAIL = flag('--fail');            // url | put | confirm
const FAIL_ONCE = flag('--fail-once');  // the same, but only the first time
const EXPIRE_AFTER = Number(flag('--expire-after', 0));  // seconds; 0 = never
// Real object storage signs a fixed set of headers, so an extra one does not
// add metadata - it breaks the signature. Emulated on purpose: it is the
// mistake the brief warns about, and the only way to meet it early is here.
const STRICT_HEADERS = !has('--loose-headers');

const ALLOWED_PUT_HEADERS = new Set([
  'host', 'user-agent', 'accept-encoding', 'accept', 'connection',
  'content-length', 'content-type',
]);

const grants = new Map();   // opaque path -> { eventId, expiresAt, contentType }
const uploaded = new Map(); // eventId -> bytes on disk
const failedOnce = new Set();

fs.mkdirSync(OUT, { recursive: true });

const say = (...parts) => console.log(...parts);

function shouldFail(step) {
  if (FAIL === step) return true;
  if (FAIL_ONCE === step && !failedOnce.has(step)) {
    failedOnce.add(step);
    return true;
  }
  return false;
}

function json(res, status, body) {
  const text = JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(text);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', (chunk) => chunks.push(chunk));
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function bearer(req) {
  const header = req.headers.authorization || '';
  return header.startsWith('Bearer ') ? header.slice(7).trim() : '';
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const parts = url.pathname.split('/').filter(Boolean);

  // Step 1: somewhere to put it.
  if (req.method === 'POST' && parts[0] === 'api' && parts[1] === 'clips' && parts[3] === 'upload-url') {
    const eventId = decodeURIComponent(parts[2]);
    if (!bearer(req)) {
      say(`  upload-url ${eventId}: no credential -> 401`);
      return json(res, 401, { error: 'no credential' });
    }
    await readBody(req);  // it is {} and there is nothing in it to want
    if (shouldFail('url')) {
      say(`  upload-url ${eventId}: failing on purpose -> 503`);
      return json(res, 503, { error: 'pretending to be down' });
    }

    const opaque = crypto.randomUUID();
    const expiresAt = EXPIRE_AFTER > 0 ? Date.now() + EXPIRE_AFTER * 1000 : null;
    grants.set(opaque, { eventId, expiresAt, contentType: 'video/mp4' });
    say(`> upload-url ${eventId} -> /bucket/${opaque}${EXPIRE_AFTER ? ` (expires in ${EXPIRE_AFTER}s)` : ''}`);
    return json(res, 200, {
      url: `http://${req.headers.host}/bucket/${opaque}`,
      method: 'PUT',
      headers: { 'Content-Type': 'video/mp4' },
      expiresAt: expiresAt ? new Date(expiresAt).toISOString() : undefined,
    });
  }

  // Step 2: the bucket. No credential here - the url is the credential.
  if (req.method === 'PUT' && parts[0] === 'bucket') {
    const grant = grants.get(parts[1]);
    if (!grant) {
      say(`  PUT /bucket/${parts[1]}: no such grant -> 403`);
      return json(res, 403, { error: 'unknown or already used signature' });
    }
    if (grant.expiresAt && Date.now() > grant.expiresAt) {
      say(`  PUT ${grant.eventId}: the url expired -> 403 (a retry must ask for a new one)`);
      return json(res, 403, { error: 'signature expired' });
    }
    if (req.headers.authorization) {
      say('  ! the PUT carried an Authorization header; the real bucket would refuse it');
    }
    if (STRICT_HEADERS) {
      const extra = Object.keys(req.headers).filter((h) => !ALLOWED_PUT_HEADERS.has(h.toLowerCase()));
      if (extra.length) {
        say(`  PUT ${grant.eventId}: unsigned header(s) ${extra.join(', ')} -> 403`);
        return json(res, 403, { error: `headers not covered by the signature: ${extra.join(', ')}` });
      }
    }
    if ((req.headers['content-type'] || '') !== grant.contentType) {
      say(`  PUT ${grant.eventId}: Content-Type was ${req.headers['content-type']} -> 403`);
      return json(res, 403, { error: 'Content-Type does not match the signature' });
    }

    const body = await readBody(req);
    if (shouldFail('put')) {
      say(`  PUT ${grant.eventId}: failing on purpose -> 500`);
      return json(res, 500, { error: 'pretending to drop it' });
    }

    fs.writeFileSync(path.join(OUT, `${grant.eventId}.mp4`), body);
    uploaded.set(grant.eventId, body.length);
    say(`> PUT ${grant.eventId}: ${body.length} bytes`);
    return json(res, 200, {});
  }

  // Step 3: the only step that lets the device delete its copy.
  if (req.method === 'POST' && parts[0] === 'api' && parts[1] === 'clips' && parts[3] === 'confirm') {
    const eventId = decodeURIComponent(parts[2]);
    if (!bearer(req)) {
      say(`  confirm ${eventId}: no credential -> 401`);
      return json(res, 401, { error: 'no credential' });
    }
    const body = await readBody(req);
    if (shouldFail('confirm')) {
      say(`  confirm ${eventId}: failing on purpose -> 500 (the clip must stay on the device)`);
      return json(res, 500, { error: 'pretending the database is down' });
    }

    let described;
    try {
      described = JSON.parse(body.toString());
    } catch {
      say(`  confirm ${eventId}: body is not JSON -> 400`);
      return json(res, 400, { error: 'not JSON' });
    }

    fs.writeFileSync(path.join(OUT, `${eventId}.json`), JSON.stringify(described, null, 2) + '\n');
    const onDisk = uploaded.get(eventId);
    // Rule 4 in the brief: a confirm may arrive with no alert, and it still
    // creates the record. Worth seeing rather than assuming.
    say(`> confirm ${eventId}: ${JSON.stringify(described)}`);
    if (onDisk === undefined) {
      say('  ! confirmed a clip this stub never received - the real server creates the row anyway');
    } else if (onDisk !== described.bytes) {
      say(`  ! it says ${described.bytes} bytes and ${onDisk} arrived`);
    }
    return json(res, 200, { ok: true });
  }

  // So server-bridge.py --check has something to talk to as well.
  if (req.method === 'GET' && parts[0] === 'api' && parts[1] === 'devices' && parts[3] === 'self') {
    if (!bearer(req)) return json(res, 401, { error: 'no credential' });
    return json(res, 200, { device: { deviceId: decodeURIComponent(parts[2]), name: 'stub', location: 'a fake bucket' } });
  }

  say(`  ${req.method} ${url.pathname} -> 404`);
  json(res, 404, { error: 'not here' });
});

server.listen(PORT, () => {
  say(`clip stub on http://0.0.0.0:${PORT}, writing to ${OUT}`);
  if (FAIL) say(`failing '${FAIL}' every time`);
  if (FAIL_ONCE) say(`failing '${FAIL_ONCE}' once`);
  if (EXPIRE_AFTER) say(`urls expire after ${EXPIRE_AFTER}s`);
  say('');
});
