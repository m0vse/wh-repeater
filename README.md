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
- detect the Lintest Systems SD1 analogue board over I2C as RX5;
- serve a local REST API on `127.0.0.1:8080`;
- serve a web management UI through nginx over HTTPS;
- generate a continuous MPEG-TS media pipeline with blue fallback/status slides;
- use the Pi hardware H.264 encoder when `h264_v4l2m2m` is available;
- stream the generated output to an optional RTMP server;
- publish PlutoPlus/F5OEO control over MQTT, including TX mute/PTT state;
- provisionally select the Tezuka DATV MQTT protocol used by 7020/7035
  PlutoSky/Fishball/LibreSDR-class firmware builds;
- optionally drive a hardware GPIO PTT output for an external amplifier or
  sequencer.

The daemon still has known gaps. Received DVB transport streams are not yet
decoded, watermarked, and re-encoded into the generated output path. The current
media pipeline is suitable for fallback/status/RTMP testing and for validating
Pluto and service control, but live received-video retranscoding is still to be
completed.

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
- `analogue.sd1`: optional SD1 analogue receiver detection over I2C;
- `pluto`: DVB-S/S2 transmit settings, symbol rate, calculated mux/video rates,
  MQTT host, MQTT protocol/device id, gain, callsign, and watermark text;
- `fallback`: static slide/video fallback settings and static-frame rate;
- `streaming.rtmp`: optional direct RTMP output URL;
- `hardwarePtt`: optional GPIO PTT output for amplifier/sequencer switching;
- `beaconSchedule`: operating hours for beacon/slideshow TX versus sleep mode;
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

SD1 access is direct C++ I2C using the PiVideo register protocol. The default SD1
device is `/dev/i2c-0` at address `0x40`, keeping it separate from the NIM bus on
`/dev/i2c-1`. NIM and SD1 I2C access still share a process mutex so service
restarts and config reloads do not intentionally overlap bus transactions.

Hardware PTT is disabled by default. When enabled, the daemon uses the Linux GPIO
character-device API, normally `/dev/gpiochip0`, and asserts the configured line
whenever the repeater is actively transmitting: beacon/slideshow during operating
hours, hang-time access notices, or live retransmit. The line is forced inactive
on config reload and daemon shutdown. Keep the GPIO output isolated from the RF
amplifier keying circuit with suitable buffering or a sequencer; do not connect a
Pi GPIO directly to an unknown amplifier PTT input.

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
- `src/sd1_controller.cpp`: SD1 analogue receiver status over I2C.
- `src/api_server.cpp`: localhost REST API.
- `web/`: static browser management UI.
- `deploy/`: systemd and nginx deployment files.
