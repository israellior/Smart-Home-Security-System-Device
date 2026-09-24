'use strict';

// The development server: static files, and the Pi's scripts by name.
//
// Its other half is a signaling switchboard - /ws, an offer, an answer and a
// handful of ICE candidates - and **nothing uses it any more**. The live call
// moved to LiveKit, so webrtc-video.py exchanges no SDP with anybody: it asks
// the app server for a token over HTTPS and negotiates with LiveKit directly.
// public/webrtc.html is the only remaining client, and it has no Pi to answer
// it. Both are kept because tools/signal-test.js still exercises the protocol
// and because the shape of it is the thing the app server replaced.
//
// What is still load-bearing is the list below: it is how the Pi gets its
// scripts, since there is no git clone on the Pi.

const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const express = require('express');
const { WebSocketServer, WebSocket } = require('ws');

const PORT = Number(process.env.PORT) || 3000;
const PING_INTERVAL_MS = 30000;

const app = express();
app.use(express.static(path.join(__dirname, 'public')));
// So the Pi can fetch its scripts: curl -fO http://SERVER_IP:3000/webrtc-video.py
// Add any new Pi script to this list or it cannot be fetched.
for (const script of [
  'check-audio.sh', 'check-camera.sh', 'check-livekit.sh', 'fix-wm8960.sh',
  'webrtc-video.py', 'server-bridge.py', 'upload-clip.py',
]) {
  app.get(`/${script}`, (req, res) =>
    res.type('text/plain').sendFile(path.join(__dirname, 'pi', script))
  );
}

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

// The server never parses SDP or ICE candidates. It gives each socket an id,
// remembers which one is the Pi, and forwards these message types verbatim to
// whichever peer `to` names, stamping `from` so the other side can answer.
const SIGNAL_TYPES = new Set(['request-offer', 'offer', 'answer', 'ice', 'bye']);
const peers = new Map(); // ws -> { id, role: 'pi' | 'browser' }
let nextPeerId = 1;

function piSocket() {
  for (const [ws, peer] of peers) if (peer.role === 'pi') return ws;
  return null;
}

function socketById(id) {
  for (const [ws, peer] of peers) if (peer.id === id) return ws;
  return null;
}

function statusMessage() {
  const pi = piSocket();
  // The Pi's peer id, or null when no Pi is signed in. A browser needs this
  // before it can ask for an offer.
  return JSON.stringify({ type: 'status', pi: pi ? peers.get(pi).id : null });
}

function broadcastStatus() {
  const message = statusMessage();
  for (const ws of wss.clients) {
    if (ws.readyState === WebSocket.OPEN) ws.send(message);
  }
}

wss.on('connection', (ws, req) => {
  ws.isAlive = true;
  ws.on('pong', () => {
    ws.isAlive = true;
  });
  ws.on('error', (err) => console.log(`peer error: ${err.message}`));

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      return;
    }

    if (msg.type === 'hello') {
      // Both sides of a WebRTC session sign in here, so the server can address them.
      const role = msg.role === 'pi' ? 'pi' : 'browser';
      if (role === 'pi') {
        // A reconnecting Pi replaces the old socket, so its own half-open
        // connection never locks it out.
        const old = piSocket();
        if (old && old !== ws) {
          console.log('signaling: a newer Pi replaced the previous one');
          old.close(4000, 'replaced by a newer Pi');
          peers.delete(old);
        }
      }
      const peer = { id: nextPeerId++, role };
      peers.set(ws, peer);
      console.log(`signaling: ${role} #${peer.id} said hello from ${req.socket.remoteAddress}`);
      ws.send(JSON.stringify({ type: 'welcome', id: peer.id }));
      broadcastStatus();
      return;
    }

    if (SIGNAL_TYPES.has(msg.type)) {
      const me = peers.get(ws);
      if (!me) return; // say hello first
      const target = socketById(msg.to);
      if (!target || target.readyState !== WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'signal-error', about: msg.type, to: msg.to }));
        console.log(`signaling: #${me.id} sent ${msg.type} to #${msg.to}, who isn't here`);
        return;
      }
      // ICE candidates arrive in the dozens; logging each one buries everything else.
      if (msg.type !== 'ice') console.log(`signaling: ${msg.type} #${me.id} -> #${msg.to}`);
      target.send(JSON.stringify({ ...msg, from: me.id }));
    }
  });

  ws.on('close', () => {
    const peer = peers.get(ws);
    if (!peer) return;
    peers.delete(ws);
    console.log(`signaling: ${peer.role} #${peer.id} disconnected`);
    // The other side tears down its peer connection on this, rather than
    // waiting for ICE to time out.
    for (const other of peers.keys()) {
      if (other.readyState === WebSocket.OPEN) {
        other.send(JSON.stringify({ type: 'peer-left', id: peer.id, role: peer.role }));
      }
    }
    broadcastStatus();
  });

  ws.send(statusMessage());
});

// Drop peers that vanished without closing their socket.
setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) {
      ws.terminate();
      continue;
    }
    ws.isAlive = false;
    ws.ping();
  }
}, PING_INTERVAL_MS);

// ws re-emits the HTTP server's errors (e.g. port already in use) here.
wss.on('error', (err) => {
  console.error(`server error: ${err.message}`);
  process.exit(1);
});

server.listen(PORT, () => {
  const lanHosts = Object.values(os.networkInterfaces())
    .flat()
    .filter((a) => a && a.family === 'IPv4' && !a.internal)
    .map((a) => a.address);

  console.log(`Pi intercom dev server on port ${PORT}`);
  // The signaling half has no Pi on the other end any more - see the top of
  // this file - so what this is actually for is the line below it.
  console.log(`  browser:  http://localhost:${PORT}/webrtc.html   (no Pi answers this now)`);
  for (const host of lanHosts) console.log(`  pi side:  curl -fO http://${host}:${PORT}/webrtc-video.py`);
});
