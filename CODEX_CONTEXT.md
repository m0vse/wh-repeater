# Codex project context

This project is an amateur TV repeater system based on BATC Winterhill hardware and a Raspberry Pi CM4.

Related repos:

- https://github.com/m0vse/winterhill
- https://github.com/m0vse/wh-repeater

The current preferred architecture is changing.

## Previous architecture

The Raspberry Pi / Winterhill system was intended to do most of the repeater work locally:

- NIM control
- scanning
- transport stream receive
- video/audio decode
- scaling
- H.264 encode
- fallback/status generation
- BATC RTMP streaming
- Pluto/modulator output

This is now considered too fragile because the Pi hardware H.264 decode path appears unreliable with imperfect incoming DATV transport streams. CPU usage and media-pipeline resilience are major concerns.

## New preferred direction

The Raspberry Pi CM4 and Winterhill hardware should become a receiver/front-end appliance.

The Pi should:

- control the Winterhill NIMs
- scan configured frequencies/symbol rates
- select the active locked receiver
- receive the selected MPEG-TS
- forward raw MPEG-TS over Ethernet to a separate PC
- expose status/counters/API
- continue operating even if the PC/gateway target is unavailable

The Pi should not be required to decode, scale, encode, or stream video in the new mode.

The PC will later handle:

- TS receive
- robust software decode if needed
- hardware H.264 encode
- fallback/status video generation
- BATC RTMP
- Pluto/modulator TS output
- recording/diagnostics/web UI

The PC hardware is not final yet. Current test hardware is a Dell OptiPlex 3020M.

## Confirmed PC media test result

On the OptiPlex 3020M / Haswell, VAAPI works with:

- `LIBVA_DRIVER_NAME=i965`
- `/dev/dri/card0`
- `h264_vaapi`

The machine successfully encoded:

- 1280x720p25 H.264 Main at about 4.4x realtime
- 1920x1080p25 H.264 Main at about 3.8x realtime
- 720p25 software decode + VAAPI re-encode at about 4.35x realtime

So the PC-side split architecture is credible.

## Required wh-repeater refactor

Add two operating modes:

### local-transcode

Existing behaviour. Preserve it for now.

### ts-gateway

New preferred mode.

In `ts-gateway` mode:

- FFmpeg must not be required at runtime.
- GStreamer must not be required at runtime.
- Pi hardware H.264 encode/decode must not be required.
- No local decode.
- No local encode.
- No scaling.
- No RTMP.
- No fallback video generation on the Pi.
- Forward raw selected MPEG-TS to a configured network destination.
- Start with UDP unicast.
- Send 188-byte aligned TS packets.
- Prefer 7 TS packets per UDP datagram, i.e. 1316-byte payloads.
- UDP send failures must not block the NIM/control/scanning loop.
- If the target PC is unreachable, the Pi should continue scanning and reporting status.

Current implemented Pi config:

```json
{
  "mode": "ts-gateway",
  "tsGateway": {
    "address": "10.10.20.1",
    "port": 10000
  }
}
```

Current implemented Pi status fields:

- `GET /api/status`
- top-level `mode`
- top-level `activeReceiver`
- `receivers[]` with lock state, scan target, MER, D-number, modulation,
  service name, TS packet counters, continuity errors, and update age
- `tsGateway` with enabled state, destination address/port, active receiver,
  sent TS packet count, UDP datagram count, byte count, send error count, update
  age, and last error
- `receiverTransitions[]` with recent receiver state and active-selection
  changes

The richer shape below is not the current Pi config. Treat it as possible future
PC/server configuration if useful:

```json
{
  "gatewayInput": {
    "protocol": "udp",
    "listenAddress": "0.0.0.0",
    "listenPort": 10000,
    "packetSize": 1316,
    "expectNullPackets": true
  }
}
```

Current validation helper:

```sh
./build/ts-gateway-inspect --bind 0.0.0.0 --port 10000 --seconds 30
```

This receives UDP MPEG-TS from wh-repeater and reports datagram grouping,
bitrate, sync errors, continuity errors, discovered services, stream types, and
top PIDs.

Important implementation note: in the current wh-repeater build, `ts-gateway`
mode does not construct the media worker, FFmpeg/GStreamer path, or H.264 codec
path at runtime. The single binary still links the local-transcode media code for
now, so a future packaging split may be useful if the Pi gateway install should
avoid FFmpeg libraries entirely.
