'use strict';

// Watches the WebRTC handshake from one side, so a failure can be pinned on the
// right machine instead of guessed at. It speaks the signaling protocol and
// nothing else: no media, no WebRTC stack.
//
//   node tools/signal-test.js                   pretend to be a browser
//   node tools/signal-test.js --role pi         pretend to be the Pi
//   node tools/signal-test.js HOST:PORT
//
// As a browser it asks the Pi for an offer and prints the SDP the Pi produced.
// If the SDP arrives here, the Pi and the server are both fine and the problem
// is in the page. If it never arrives, the Pi never got as far as an offer.
//
// As a Pi it just sits there, so you can watch a real browser ask for one. The
// browser will wait forever for an offer that never comes; that is the point.

// Only our own arguments: argv[0] is the node binary, whose Windows path
// ("C:\Program Files\...") also contains a colon.
const args = process.argv.slice(2);
const HOST = args.find((a) => a.includes(':') && !a.startsWith('--')) || 'localhost:3000';
const ROLE = args.includes('--role') ? args[args.indexOf('--role') + 1] : 'browser';

if (ROLE !== 'browser' && ROLE !== 'pi') {
  console.error('--role must be browser or pi');
  process.exit(1);
}

let myId = null;
let iceCount = 0;
let requested = false; // the server broadcasts status on every peer change; ask once

const ws = new WebSocket(`ws://${HOST}/ws`);

ws.onerror = () => {
  console.error(`could not connect to ws://${HOST}/ws - is server.js running?`);
  process.exit(1);
};

ws.onopen = () => {
  console.log(`connected to ${HOST}, saying hello as ${ROLE}`);
  ws.send(JSON.stringify({ type: 'hello', role: ROLE }));
};

ws.onmessage = (e) => {
  const msg = JSON.parse(e.data);

  switch (msg.type) {
    case 'welcome':
      myId = msg.id;
      console.log(`signed in as ${ROLE} #${myId}`);
      break;

    case 'status':
      if (ROLE === 'browser') {
        if (msg.pi === null) {
          console.log('no Pi is signed in yet - start pi/webrtc-video.py');
        } else if (!requested) {
          requested = true;
          console.log(`Pi is #${msg.pi}, asking it for an offer`);
          ws.send(JSON.stringify({ type: 'request-offer', to: msg.pi }));
        }
      }
      break;

    case 'request-offer':
      console.log(`browser #${msg.from} asked for an offer (a real Pi would build a pipeline now)`);
      break;

    case 'offer':
      console.log(`\n=== offer from #${msg.from}, ${msg.sdp.length} bytes ===`);
      console.log(msg.sdp);
      console.log('=== end of offer ===\n');
      console.log('the Pi is producing valid signaling. Watching for ICE candidates…');
      break;

    case 'answer':
      console.log(`\n=== answer from #${msg.from}, ${msg.sdp.length} bytes ===`);
      console.log(msg.sdp);
      break;

    case 'ice':
      iceCount++;
      // Host candidates are LAN addresses. Seeing only these is correct here:
      // nothing needs STUN when both machines are on the same network.
      console.log(`  ice #${iceCount} from #${msg.from}: ${msg.candidate.candidate || '(end of candidates)'}`);
      break;

    case 'peer-left':
      console.log(`${msg.role} #${msg.id} left`);
      break;

    case 'signal-error':
      console.log(`server could not deliver ${msg.about} to #${msg.to}`);
      break;
  }
};

ws.onclose = () => {
  console.log(`connection closed after ${iceCount} ICE candidate(s)`);
  process.exit(0);
};

process.on('SIGINT', () => {
  console.log(`\nstopping after ${iceCount} ICE candidate(s)`);
  ws.close();
});
