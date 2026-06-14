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

In `ts-gateway` mode the Pi API binds to `0.0.0.0:8080` so the PC gateway can
poll status and apply hardware configuration. This is a machine-to-machine API;
do not serve the operator web UI or nginx from the Pi/Winterhill hardware.

In `pc-gateway` and `local-transcode` modes the API binds to `127.0.0.1:8080`
and is normally reached through the PC-side nginx/web UI.

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

When the PC web UI saves configuration, `wh-pc-gateway` compares only the
Pi-owned hardware gateway fields:

- `statusIntervalMs`;
- `selection.minimumMerDb`;
- `selection.minimumDNumberDb`;
- `tsGateway.address`;
- `tsGateway.port`;
- `receivers[]` scan plans, dwell times, hang times, and enabled states.

If those fields changed and `piStatus.enabled` is true, the PC fetches the Pi
config from `/api/config`, merges those fields into it, forces `mode` to
`ts-gateway`, and sends the result back with `PUT /api/config`. Changes to PC
media, Pluto, fallback, RTMP, preview, ident, or other output settings do not
trigger a Pi config update.

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

## Target Mini-PC Network Layout

The intended PC-side media processor is a fanless Intel Core Ultra 5 125U
mini-PC with three 2.5GbE LAN interfaces. It has not arrived yet, so do not
assume this hardware is available for current testing.

Planned interface roles:

- LAN 1: management, internet access, and BATC RTMP output;
- LAN 2: dedicated point-to-point link to the Raspberry Pi CM4/Winterhill
  receiver;
- LAN 3: dedicated point-to-point link to the Pluto.

Likely addressing:

- mini-PC Pi-facing NIC: `10.10.20.1/24`;
- Raspberry Pi: `10.10.20.2/24`;
- mini-PC Pluto-facing NIC: `10.10.30.1/24`;
- Pluto: `10.10.30.2/24`.

Only the management/BATC interface should have a default route. The Pi-facing
and Pluto-facing links should be directly connected subnets without default
routes.

The Pi remains the Winterhill/NIM control and raw TS gateway device. In the
target layout, the Pi should forward selected MPEG-TS to the mini-PC over the
dedicated Pi link:

```json
{
  "mode": "ts-gateway",
  "tsGateway": {
    "address": "10.10.20.1",
    "port": 5000
  }
}
```

The mini-PC should eventually own:

- raw TS receive from the Pi;
- decode/transcode and final H.264 encode;
- fallback/status/slideshow video generation;
- BATC RTMP output;
- final MPEG-TS output to the Pluto over the Pluto-facing link;
- logging, status, monitoring, and the operator web UI.

Keep current development modular: continue improving Pi-side `ts-gateway` mode
and the PC-side `pc-gateway` media/control boundary without baking in the
temporary test-PC addresses or assuming the Core Ultra hardware is present.

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

## Next Development Stage After UDP Validation

After the UDP MPEG-TS path is proven end to end, continue moving from the
current mode-based binary toward two clearly demarcated services. The installed
service and config split is now active; the deeper link-time/code split can
follow once the runtime boundary is stable.

### Pi Headless Gateway Service

The Pi service should become a headless Winterhill hardware appliance.

Responsibilities:

- control the Winterhill NIM hardware;
- scan configured RX1-RX4 frequency/symbol-rate plans;
- select the active locked receiver;
- expose a small hardware/status API on `0.0.0.0:8080`;
- forward selected raw MPEG-TS to the PC over UDP;
- continue scanning and reporting status when the PC/media path is unavailable.

The Pi service should not own:

- nginx;
- the operator web UI;
- decode/transcode/fallback/status-video generation;
- RTMP/BATC output;
- Pluto/media output;
- broad operator-facing configuration.

All operator commands should originate from the PC side. The Pi API should be
treated as a machine-to-machine hardware API, not as a user interface.

### PC Gateway And Control Service

The PC service should become the control plane and media/output appliance.

Responsibilities:

- receive raw UDP MPEG-TS from the Pi;
- poll/mirror Pi NIM status for the dashboard;
- host the web UI through nginx;
- own all operator-facing API routes;
- decode/transcode/watermark/fallback/status generation;
- own RTMP/BATC output;
- own Pluto/modulator output;
- own recording/diagnostics features;
- send validated Pi hardware configuration updates to the Pi API.

Browsers should talk only to the PC. The PC service should talk to the Pi API on
behalf of the operator.

### Service Split

Installed service names:

- `wh-pi-gateway`: Pi-only NIM scanner and UDP TS sender.
- `wh-pc-gateway`: PC-side UDP receiver, media/output service, and control API.

Config locations:

- `/etc/wh-pi-gateway/config.json`;
- `/etc/wh-pc-gateway/config.json`.

Deployment commands:

```sh
sudo DEPLOY_TARGET=pi-gateway ./deploy/install.sh
sudo DEPLOY_TARGET=pc-gateway ./deploy/install.sh
```

`DEPLOY_TARGET=pi-gateway` installs no web UI and no nginx site. The Pi remains
headless apart from SSH and the port-8080 hardware API used by the PC service.

The current single binary can be kept while validating the architecture, but the
post-validation split should remove runtime mode branching where practical:

- the Pi executable should not link or require FFmpeg/GStreamer/media output
  components;
- the PC executable should not link or require whdriver/NIM/I2C hardware
  components.

Shared code can remain in common libraries for types, config parsing, API
helpers, and JSON utilities.

### PC-Managed Pi Configuration

The PC web UI should include a Pi Gateway configuration section. Those controls
should update the Pi through the PC backend, not directly from browser
JavaScript.

PC API routes can be added as a stable facade:

```text
GET /api/pi/status
GET /api/pi/config
PUT /api/pi/config
```

The PC backend should proxy these to:

```text
GET http://<piStatus.address>:8080/api/status
GET http://<piStatus.address>:8080/api/config
PUT http://<piStatus.address>:8080/api/config
```

Only Pi-owned fields should be accepted and forwarded:

- `mode`, fixed to `ts-gateway`;
- `statusIntervalMs`;
- `selection.minimumMerDb`;
- `selection.minimumDNumberDb`;
- `tsGateway.address`;
- `tsGateway.port`;
- `receivers[]`:
  - enabled state;
  - dwell time;
  - hang time;
  - scan targets;
  - frequency;
  - symbol rate;
  - local oscillator;
  - DVB system;
  - FEC;
  - label.

The Pi should reject media/output fields in gateway mode, including:

- `pluto`;
- `streaming`;
- `media`;
- `fallback`;
- `analogue` media capture;
- RTMP/BATC/recording/output controls.

This keeps the PC as the single operator console while preserving a narrow,
testable Pi hardware boundary.
