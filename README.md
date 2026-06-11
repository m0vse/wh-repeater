# wh-repeater

`wh-repeater` is a fresh C++ daemon for a Winterhill-derived DVB repeater.

The project uses the original Winterhill C code only as hardware reference. The
daemon talks to the existing `whdriver` kernel module for Winterhill board
access and keeps the Serit NIM, STV0910, STV6120, PIC, PlutoPlus, SD1 analogue
input, media pipeline, and management API behind contained C++ interfaces.

## Current State

This is the first working streaming test build. It can:

- control two Serit NIM devices as four fixed logical DVB receivers:
  - RX1: NIM A top antenna
  - RX2: NIM A bottom antenna
  - RX3: NIM B top antenna
  - RX4: NIM B bottom antenna
- scan configured frequency, symbol-rate, DVB-S/S2 mode, and FEC targets;
- pause scanning while a receiver is locked, then resume after a configured hang
  time once the signal is lost;
- support a generic USB/V4L2 analogue capture input, with parked Lintest
  Systems SD1 support retained for future CSI mode work;
- serve a local REST API on `127.0.0.1:8080`;
- serve a web management UI through nginx over HTTPS;
- generate a continuous MPEG-TS media pipeline with blue fallback/status slides;
- require the Pi hardware H.264 encoder for all output video encoding;
- stream the generated output to an optional RTMP server;
- isolate FFmpeg/V4L2 media handling in a supervised child process so the
  control daemon can restart media output after a media-worker failure;
- publish PlutoPlus/F5OEO control over MQTT, including TX mute/PTT state;
- provisionally select the Tezuka DATV MQTT protocol used by 7020/7035
  PlutoSky/Fishball/LibreSDR-class firmware builds;
- optionally drive a hardware GPIO PTT output for an external amplifier or
  sequencer.

The media pipeline has a hard fixed-output contract: every source must be
decoded, scaled, resampled, and transcoded into the configured stream profile so
RTMP/output video is never interrupted just because the source changes. See
[`docs/media-stream-contract.md`](docs/media-stream-contract.md).
Hardware H.264 encode is mandatory for every output path. Software decode may be
used when hardware decode is unavailable or unsuitable, but software H.264 encode
must not be used as an automatic fallback.
The hardware encoder is treated as a protected boundary: only validated
fixed-format frames with monotonic timestamps may be submitted to it. Bad,
missing, or stalled source data must be replaced with generated video and
silence before it reaches the encoder.

Media processing runs in a forked child process supervised by the main daemon.
The child owns FFmpeg, V4L2 codec devices, Pluto UDP output, and RTMP output.
The parent keeps API/config/scanning/PTT state alive, sends control and selected
TS packets over a local Unix socket, and restarts the media child if it exits.

`media.backend` selects the media implementation. `ffmpeg` is the stable default.
`gstreamer` is an experimental complete media backend. When selected, generated
fallback/slides, fallback videos, DVB retransmit, analogue capture, MPEG-TS
output, and RTMP output are handled through GStreamer pipelines instead of the
FFmpeg encode/mux path. FFmpeg remains the stable default.

## Requirements

On the Pi target:

- CMake 3.20 or newer;
- a C++20 compiler;
- FFmpeg development libraries:
  `libavformat`, `libavcodec`, `libavfilter`, `libavutil`, `libswresample`, and
  `libswscale`;
- the Winterhill `whdriver` kernel module and device node, normally
  `/dev/whdriver-4v00`;
- I2C enabled for the NIM hardware and optional SD1 board;
- nginx if using the packaged web UI;
- a self-signed or real certificate at
  `/etc/ssl/wh-repeater/wh-repeater.crt` and
  `/etc/ssl/wh-repeater/wh-repeater.key` if using the supplied nginx config.

Example dependency install on Raspberry Pi OS:

```sh
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config nginx \
  libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev \
  libswresample-dev libswscale-dev
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

For a direct foreground run from the repo:

```sh
./build/wh-repeater wh-repeater.json
```

Without an explicit argument, the daemon looks for `wh-repeater.json` in the
current working directory.

## Installed Service

The installed service expects mutable configuration in
`/etc/wh-repeater/wh-repeater.json` and the binary at
`/usr/local/bin/wh-repeater`.

```sh
sudo install -m 0755 build/wh-repeater /usr/local/bin/wh-repeater
sudo install -d -m 0755 /etc/wh-repeater /var/www/wh-repeater
sudo install -m 0644 wh-repeater.json /etc/wh-repeater/wh-repeater.json
sudo install -m 0644 deploy/systemd/wh-repeater.service /etc/systemd/system/wh-repeater.service
sudo cp web/index.html web/styles.css web/app.js /var/www/wh-repeater/
sudo systemctl daemon-reload
sudo systemctl enable --now wh-repeater.service
```

The unit runs:

```text
/usr/local/bin/wh-repeater /etc/wh-repeater/wh-repeater.json
```

with `WH_REPEATER_NIM=serit`.

Useful service checks:

```sh
sudo systemctl status wh-repeater.service
sudo journalctl -u wh-repeater.service
curl -sS http://127.0.0.1:8080/api/health
curl -sS http://127.0.0.1:8080/api/status
```

## Web UI

The daemon only binds the API to localhost. The supplied nginx site serves the
static UI from `/var/www/wh-repeater`, terminates HTTPS, and proxies `/api/` to
`127.0.0.1:8080`.

Install the nginx site:

```sh
sudo install -m 0644 deploy/nginx/wh-repeater /etc/nginx/sites-available/wh-repeater
sudo ln -sf /etc/nginx/sites-available/wh-repeater /etc/nginx/sites-enabled/wh-repeater
sudo nginx -t
sudo systemctl reload nginx
```

For a self-signed certificate:

```sh
sudo install -d -m 0755 /etc/ssl/wh-repeater
sudo openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout /etc/ssl/wh-repeater/wh-repeater.key \
  -out /etc/ssl/wh-repeater/wh-repeater.crt \
  -subj "/CN=wh-repeater"
sudo chmod 0600 /etc/ssl/wh-repeater/wh-repeater.key
sudo chmod 0644 /etc/ssl/wh-repeater/wh-repeater.crt
```

## Configuration

`wh-repeater.json` is the sample config. The live file is
`/etc/wh-repeater/wh-repeater.json` when running through systemd.

The main sections are:

- `receivers`: RX1-RX4 scan targets, dwell time, and hang time;
- `analogue.capture`: generic USB/V4L2 analogue capture configuration;
- `analogue.sd1`: parked SD1 analogue configuration, disabled by default;
- `pluto`: DVB-S/S2 transmit settings, symbol rate, calculated mux/video rates,
  fixed output width/height/frame rate, MQTT host, MQTT protocol/device id,
  gain, callsign, and watermark text;
- `fallback`: static slide/video fallback settings and slide timing;
- `streaming.rtmp`: optional direct RTMP output URL;
- `media`: selected media backend, currently stable `ffmpeg` or experimental
  complete-path `gstreamer`;
- `hardwarePtt`: optional GPIO PTT output for amplifier/sequencer switching;
- `beaconSchedule`: UTC operating hours for beacon/slideshow TX versus sleep mode;
- `ident`: service name and ident interval;
- `selection`: minimum signal quality thresholds.

Do not commit live RTMP stream keys or site-specific secrets. Keep those in the
installed config file.

## Hardware Notes

The Serit NIM path is selected with:

```sh
WH_REPEATER_NIM=serit ./build/wh-repeater wh-repeater.json
```

The implementation expects the existing whdriver interface exposed by the kernel
module. It does not create the device node itself.

Generic analogue capture is wired for USB/V4L2 devices such as UVC composite
capture dongles. Configure `analogue.capture.captureDevice`, normally
`/dev/video0`, plus PAL defaults of 720x576 at 25 fps. `lockMode` supports
`v4l2-sync`, `manual`, `device-present`, and `gpio`. Use `v4l2-sync` for USB
capture cards that expose input sync state; it only locks when the selected
V4L2 input is not reporting no-signal or no-hsync. The Automation HAT default
for GPIO lock is input 1 on `/dev/gpiochip0` line 26, active high.

SD1 analogue support is parked pending confirmed CSI output details from the
Lintest board. The retained code can read the PiVideo control processor at
`/dev/i2c-0` address `0x40`, and the latest hardware tests found the emulated
OV5647 camera endpoint at `0x36` on camera mux bus `i2c-10`, but no completed
CSI frames were received. Keep `analogue.sd1.enabled` false until the CSI data
type, lane count, timing, and link frequency are confirmed.

Hardware PTT is disabled by default. When enabled, the daemon uses the Linux GPIO
character-device API, normally `/dev/gpiochip0`, and asserts the configured line
whenever the repeater is actively transmitting: beacon/slideshow during operating
hours, hang-time access notices, or live retransmit. The line is forced inactive
on config reload and daemon shutdown. Keep the GPIO output isolated from the RF
amplifier keying circuit with suitable buffering or a sequencer; do not connect a
Pi GPIO directly to an unknown amplifier PTT input.

For Pimoroni Automation HAT, use relay 1 for PTT on GPIO line 13. The HAT's I2C
devices are expected at `0x48` for ADS1015 analogue inputs and `0x54` for the
SN3218 LED driver; these addresses were clear on the initial `/dev/i2c-1`
baseline scan.

Tezuka/F5OEO DATV output is selected with `"mqttProtocol": "tezuka"` in the
`pluto` config object, plus optional `"mqttDeviceId"` when the firmware's MQTT
device id is not the same as the configured callsign. The daemon sends MPEG-TS
over UDP to `pluto.address` and `pluto.port`, configures the Tezuka TS source
with `tx/dvbs2/tssourcemode=0` and `tx/dvbs2/tssourceaddress=<address>:<port>`,
and uses the serial-scoped `cmd/pluto/<device-id>/...` and
`dt/pluto/<device-id>/...` topic layout.

## Source Layout

- `src/nim_controller.cpp`: whdriver, Serit NIM, STV0910/STV6120, PIC access.
- `src/scan_scheduler.cpp`: receiver dwell and hang-time scan policy.
- `src/signal_arbitrator.cpp`: active-input selection.
- `src/media_pipeline.cpp`: generated MPEG-TS, fallback/status slides, H.264
  encoding, and RTMP output.
- `src/hardware_ptt.cpp`: optional GPIO PTT output for amplifier/sequencer
  control.
- `src/pluto_sink.cpp`: Pluto UDP TS output and MQTT transmit control.
- `src/pluto_mqtt_status.cpp`: Pluto MQTT status subscription.
- `src/media_process.cpp`: parent-side media child supervision and IPC.
- `src/media_pipeline.cpp`: child-side FFmpeg/V4L2 media generation,
  encoding, RTMP output, and fallback rendering.
- `src/sd1_controller.cpp`: parked SD1 analogue receiver status over I2C.
- `docs/sd1-parked.md`: SD1 test findings and restoration notes.
- `src/api_server.cpp`: localhost REST API.
- `web/`: static browser management UI.
- `deploy/`: systemd and nginx deployment files.
