/*
 * ============================================================================
 *  wh-repeater - Pi Gateway API Contract
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Defines the Pi-side control/status and UDP MPEG-TS contract used when
 *    wh-repeater runs as a Winterhill hardware gateway for a future PC server.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    In gateway mode the Pi owns hardware access only; media processing,
 *    retransmit output, fallback generation, and operator UI can move to a PC.
 * ============================================================================
 */

# Pi Gateway API Contract

This document defines the boundary between the Raspberry Pi Winterhill hardware
gateway and a future PC Linux server.

The Pi gateway remains responsible for deterministic local hardware work:

- controlling the Winterhill NIM hardware through the existing `whdriver`
  kernel-module interface;
- scanning configured RX1-RX4 targets;
- reporting lock, signal quality, service metadata, packet counters, and
  receiver transitions;
- selecting the best active NIM input;
- forwarding selected raw MPEG-TS packets over UDP.

The PC server should run wh-repeater in `pc-gateway` mode and own media-heavy
and operator-facing work:

- TS validation, decode, transcode, watermark, ident, fallback, slideshow, and
  generated status video;
- RTMP/BATC/Pluto/Tezuka output;
- the primary web interface and operator API;
- high-level operating policy, schedules, and output routing.

## Operating Mode

Set the Pi config top-level `mode` to `ts-gateway`.

```json
{
  "mode": "ts-gateway",
  "tsGateway": {
    "address": "192.168.1.50",
    "port": 5000
  }
}
```

In `ts-gateway` mode:

- the daemon does not construct the media worker;
- FFmpeg, GStreamer, and H.264 codec checks are not required for runtime;
- Pluto, BATC, RTMP, fallback, slideshow, and analogue media paths are not moved
  or reinterpreted;
- only the selected Winterhill NIM transport stream is forwarded.

`local-transcode` remains the existing single-Pi mode and keeps the fixed media
stream contract in [`media-stream-contract.md`](media-stream-contract.md).

Set the PC config top-level `mode` to `pc-gateway`.

```json
{
  "mode": "pc-gateway",
  "gatewayInput": {
    "listenAddress": "0.0.0.0",
    "listenPort": 5000,
    "packetSize": 1316
  }
}
```

In `pc-gateway` mode:

- the daemon does not initialise NIM hardware or scan receivers;
- UDP MPEG-TS received from the Pi is treated as the live DVB input;
- fallback/status generation, decode/transcode, RTMP, Pluto, MQTT status, and
  hardware PTT remain on the PC;
- if no UDP packets arrive within `fallback.inputTimeoutMs`, the media path can
  fall back to the configured beacon/status output.

## HTTP API

The Pi API binds to localhost by default and is normally exposed through nginx
if remote access is required.

### Health

```text
GET /api/health
```

Returns a basic daemon health response.

### Configuration

```text
GET /api/config
PUT /api/config
```

The PC server may use these to read or update Pi hardware gateway settings. For
gateway operation, the important fields are:

- `mode`: must be `ts-gateway`;
- `statusIntervalMs`: control/status loop interval;
- `selection.minimumMerDb` and `selection.minimumDNumberDb`: quality threshold
  for active input selection;
- `receivers`: RX1-RX4 scan targets, dwell time, and hang time;
- `tsGateway.address` and `tsGateway.port`: PC UDP destination.

Config changes are accepted immediately, but hardware/media worker changes that
are process-lifetime scoped still require a service restart. The PC server
should treat `mode` changes as restart-required.

### Status

```text
GET /api/status
```

Important top-level fields:

- `mode`: current daemon mode;
- `activeReceiver`: currently selected receiver id, or `null`;
- `receivers`: RX status array;
- `tsGateway`: UDP gateway counters/status;
- `receiverTransitions`: recent receiver and active-selection transitions;
- `beaconSchedule`, `analogue`, and `pluto`: retained for local-transcode
  compatibility and legacy UI visibility.

Receiver objects include:

- `id`: logical receiver number, normally 1-4 for NIM receivers;
- `name`: display name such as `RX1`;
- `type`: `nim`;
- `nim`, `tuner`, `antenna`: fixed physical mapping;
- `state`: `idle`, `searching`, `foundHeader`, `lockedDvbs`, `lockedDvbs2`,
  `lost`, `timeout`, or `fault`;
- `target`: current scan target with frequency, symbol rate, system, FEC, and
  label;
- `merDb`, `dNumberDb`, `modulation`, `serviceName`;
- `transportPackets`, `continuityErrors`;
- `updatedMsAgo`.

`tsGateway` includes:

- `enabled`: true when gateway sink is active;
- `address`, `port`: configured UDP destination;
- `activeReceiver`: receiver currently being forwarded, or `null`;
- `transportPackets`: TS packets successfully sent by the gateway sink;
- `datagrams`: UDP datagrams sent;
- `bytes`: payload bytes sent;
- `sendErrors`: UDP/socket/alignment errors;
- `updatedMsAgo`;
- `lastError`.

`receiverTransitions` entries include:

- `receiver`: receiver id or `null` for active-selection changes;
- `from`, `to`: previous and new state labels;
- `detail`: scan target label, service name, or active-input detail;
- `updatedMsAgo`.

The PC server should poll `/api/status` while a push/event API does not yet
exist. Start with the configured `statusIntervalMs`; reduce polling only if the
Pi loop and network remain stable.

## PC Status Mirroring

When the PC runs in `pc-gateway` mode, it can poll the Pi API and mirror the
Pi's receiver status into the PC dashboard. This keeps the operator UI looking
like the original four-NIM dashboard even though the PC owns media/output and
the Pi owns only Winterhill hardware.

PC config:

```json
{
  "mode": "pc-gateway",
  "gatewayInput": {
    "listenAddress": "0.0.0.0",
    "listenPort": 5000,
    "packetSize": 1316
  },
  "piStatus": {
    "enabled": true,
    "address": "192.168.99.181",
    "port": 8080,
    "pollIntervalMs": 500
  }
}
```

The PC polls:

```text
GET http://<piStatus.address>:<piStatus.port>/api/status
```

The PC currently reuses these Pi response fields directly:

- `activeReceiver`;
- `receivers`;
- `receiverTransitions`.

The Pi must therefore keep the existing `ts-gateway` `/api/status` shape stable.
In particular, every NIM receiver object should include the fields already used
by the web UI:

- `id`, `name`, `type`, `deviceId`, `nim`, `tuner`, `antenna`;
- `state`, `target`, `merDb`, `dNumberDb`, `serviceName`, `modulation`;
- `transportPackets`, `continuityErrors`, `updatedMsAgo`.

If the Pi is unavailable, the PC dashboard falls back to showing its local UDP
gateway input as a single receiver. The PC-side poller uses short nonblocking
network timeouts so an offline Pi does not stall media, fallback, Pluto, RTMP,
or API handling.

For the current GB3GV test PC, use:

- PC address: `192.168.99.113`;
- Pi API/status address: `192.168.99.181`;
- UDP MPEG-TS from Pi to PC: `192.168.99.113:5000`.

The Pi-side config should therefore include:

```json
{
  "mode": "ts-gateway",
  "tsGateway": {
    "address": "192.168.99.113",
    "port": 5000
  }
}
```

## UDP MPEG-TS Contract

The Pi sends raw MPEG-TS packets over UDP to `tsGateway.address:tsGateway.port`.

Packet rules:

- every TS packet is 188 bytes;
- normal datagrams contain seven TS packets, i.e. 1316 bytes;
- a short datagram may occur when the selected receiver is cleared and the sink
  flushes a partial buffer;
- packet order is the order received from the Winterhill transport stream;
- no transcoding, remuxing, PCR correction, null stuffing, or service filtering
  is performed on the Pi;
- the PC server must validate sync bytes, continuity counters, PAT/PMT/SDT, PCR,
  service/video/audio content, and bitrate before using the stream.

Receiver loss behavior:

- when there is no active receiver, the Pi stops forwarding TS;
- gateway counters remain visible in `/api/status`;
- receiver and active-input transitions indicate lock, loss, and selection
  changes;
- the future PC server should generate any fallback/status output itself.

## Validation Tool

Build the diagnostic receiver:

```sh
cmake --build build --target ts-gateway-inspect
```

Run it on the PC destination, or on the Pi while testing locally:

```sh
./build/ts-gateway-inspect --bind 0.0.0.0 --port 5000 --seconds 30
```

It reports:

- datagram sizes, including 1316-byte grouping;
- total packets, bytes, and bitrate;
- sync and continuity errors;
- discovered services from PAT/PMT/SDT;
- top PIDs by packet count.

This tool is only an inspector. It does not replace the future PC server and it
does not generate fallback output.
