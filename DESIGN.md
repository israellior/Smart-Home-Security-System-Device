# DESIGN.md

How this system is meant to work when it is finished.

`CLAUDE.md` describes what exists today. This describes what is being built
toward. The decisions here are settled — treat them as given.

Last updated 2026-09-19.

## What it is

A private intercom. A Raspberry Pi with a camera, microphone and speaker, watched
and talked to through a browser, from anywhere in the world.

## Users and access

- Users have accounts in the app.
- A user belongs to one or more **device groups**. A group grants access to
  particular Pis.
- Adding a user to a device group is the only step needed to give them access.
  Removing them revokes it immediately.
- Access is carried by **tokens the app issues**: scoped to a single device, and
  short-lived. The user installs nothing and configures nothing.
- Each Pi authenticates with its own device credential, set when it is
  provisioned. A Pi cannot be impersonated.
- A user with a valid token for one Pi has no access to any other.

## The parts

```
   ┌────────────┐        ┌──────────────────────┐        ┌───────────┐
   │     Pi     │        │   app server (VPS)   │        │  browser  │
   │  at a home │◄──────►│  accounts, groups,   │◄──────►│  anywhere │
   │            │  wss   │  tokens, signaling   │  wss   │           │
   └─────┬──────┘        └──────────────────────┘        └─────┬─────┘
         │                                                     │
         │               ┌──────────────────┐                  │
         └──────────────►│       SFU        │◄─────────────────┘
            one copy up  │  copies it out   │   one copy each
                         └──────────────────┘
```

**The app server** is public, on a rented host, behind HTTPS with a real
certificate. It holds accounts, device groups and device credentials, issues
tokens, and runs signaling. It never carries media.

**The SFU** carries the media. The Pi publishes one stream to it; the SFU sends a
copy to each viewer. Both the Pi and the browsers connect *outward* to it, so no
home router needs to accept an incoming connection and no separate TURN relay is
required.

**Decided 2026-09-22: the SFU is LiveKit Cloud.** Managed, so there is no SFU
to run, and its access tokens already express the per-participant permissions
this needs. Every mint goes through one interface on the app server, so moving
to self-hosted LiveKit or to mediasoup later changes one module and no callers.
The device holds two tokens rather than one — see `docs/server-brief.md` for
why the split is load-bearing.

**The Pi** publishes one stream, whatever the number of viewers. It never fans
out, and its upload cost does not grow with the audience.

**The browser** is a plain web page. No plugin, no install, no VPN.

## How a session happens

1. The user signs in to the app and opens a device they have access to.
2. The app server checks group membership and issues a short-lived token scoped
   to that Pi.
3. The browser connects to the SFU with that token and subscribes to the Pi's
   streams.
4. The Pi, already authenticated with its device credential, is publishing.
5. Media flows through the SFU. The app server is not in the media path.

## Media

| Direction | Content | Codec |
|---|---|---|
| Pi → viewers | camera | H.264 |
| Pi → viewers | room microphone | Opus |
| viewers → Pi | a viewer's microphone | Opus |

- All of a peer's tracks share one connection and one transport.
- Sound and picture stay in sync because they share one clock and one set of
  timing reports.
- Several viewers may talk at once. The Pi mixes the incoming streams into its
  single speaker.
- A viewer who is not talking sends nothing at all.
- Echo cancellation runs on the Pi, because its microphone and speaker are the
  same sound card.

## Capacity

- **1–5 viewers per Pi**, typically one of them talking.
- Video is sized for an intercom, not for broadcast: 640×480 is the target, at a
  bitrate chosen for cost rather than quality.

## Still to decide

- The final video bitrate and resolution.
- Whether this repository remains a learning project (`CLAUDE.md` Phase 2:
  hand-written RTP, jitter buffer, direct V4L2) or becomes the product. The two
  goals do not conflict, but "finished" means something different for each.
