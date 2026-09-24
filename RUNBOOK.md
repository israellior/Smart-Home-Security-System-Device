# Running the whole thing

Cold start, everything closed. Nothing here installs or configures — it assumes
the Pi already has `porchlightd`, the venv and a valid credential.

Machines: **Pi** `192.168.0.129` (user `israellior1`), **server host**
`192.168.0.219` (this Windows box). MongoDB is Atlas and R2 and LiveKit are
cloud — there is nothing local to start for any of them.

---

## 1. Windows — two PowerShell terminals

**Terminal 1 — the app server (:4000).** Alerts, clips, LiveKit tokens, and the
device WebSocket all live here.

```powershell
cd "C:\Users\israe\Documents\Smart Camera Doorbell\smart-doorbell-app\porchlight-backend"
npm run dev
```

Wait for `MongoDB connected` then `Porchlight API listening on http://localhost:4000`.
If it dies on a TLS error, just run it again — that Atlas cluster drops the
first handshake regularly.

**Terminal 2 — the viewer app (:5173).**

```powershell
cd "C:\Users\israe\Documents\Smart Camera Doorbell\smart-doorbell-app"
npm run dev
```

---

## 2. Pi — one SSH session

```bash
ssh israellior1@192.168.0.129
```

The config gets left in debug states. Put it back to the real one first — this
line is safe to run every time:

```bash
sudo sed -i 's/"uploader": "log"/"uploader": "script"/; s/"audio_source": "test"/"audio_source": "alsa"/; s/"input": "stdin"/"input": "gpio"/; s/"led": "console"/"led": "gpio"/' /etc/porchlight/porchlightd.json
```

Then start it in the foreground so you can read the log:

```bash
/usr/local/bin/porchlightd /etc/porchlight/porchlightd.json
```

Healthy startup is four lines:

```
led       line 25 on /dev/gpiochip0
input     button on line 24, motion on line 23
server    bridge started, connecting to http://192.168.0.219:4000
server    connected
```

---

## 3. Browser

`http://localhost:5173` — log in as **cara-1788679585@x.com**, who owns
`porch-1`. To use a different account, register and join with share code
**PORCH-EFTDM6**; you come in as a member, which is enough to watch.

---

## Check it worked

**Alerts and clips.** Wave at the PIR or press the button. On the Pi:
`input: motion` (or `button pressed` + `chime: ding`), then `rec recording`,
then 15 s later `rec finished ok=true`, then `upload confirmed`. The clip
appears under Activity in the app.

**Live view.** Click Watch in the app. The Pi should log `viewer-requested`,
stop any recording, and start `webrtc-video.py`.

---

## Optional — two more Windows terminals

Worth having when something is silent, because an unacknowledged alert and a
clip that uploaded but was never confirmed both look like "nothing happened".
Both run from `porchlight-backend`:

```powershell
node scripts/watch.mjs --device porch-1        # events, alerts, clips
node scripts/watch-room.mjs --device porch-1   # who is in the LiveKit room
```

`watch.mjs` should flip `porch-1` to `ONLINE` the moment the daemon connects.

The dev file server in this repo (`npm start`, :3000) is **not** part of
running the system. It exists only so the Pi can `curl` scripts and the
`porchlightd` tarball, and is needed only when you change one of those.

---

## Worth knowing

- **Live view has never been exercised end to end.** Everything above alerts
  and clips is confirmed on hardware; the Watch path is not.
- **The camera and the sound card go to one process at a time.** The daemon
  stops a recording before starting a call for that reason. A stray
  `rpicam-hello` or a second `porchlightd` will take either, and the failure
  reads as "cannot open" rather than "something else has it".
- **A doorbell press during a live view will not chime** — the call holds the
  card. Known gap, not a fault.
- **Clip sizes vary from ~400 KB to ~3.8 MB.** That is x264 spending its
  bitrate on a moving person and almost nothing on an empty room. Normal.
- **Open: some clips report a nonsense duration in the player** (`204:38:09`)
  while `gst-discoverer-1.0` on the Pi reads them as a correct ~15 s. Not yet
  established whether the file or the playback path is at fault.
