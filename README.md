# wh-repeater

`wh-repeater` is a fresh C++ daemon for a Winterhill-derived DVB repeater.

The aim is to reuse the Winterhill hardware knowledge, not the monolithic C
application structure. The daemon owns scanning, signal arbitration, transport
stream routing, PlutoPlus output, and ident insertion behind small interfaces.

## Direction

- Control the two Serit NIM modules as four logical receivers.
- Scan configured frequency, symbol-rate, antenna, and mode targets.
- Detect usable DVB-S/S2 signals from lock state, MER, D-number, and TS health.
- Select one input according to configured receiver priority and signal quality.
- Route the selected transport stream to PlutoPlus firmware such as
  `pluto-ori-ps`.
- Add ident first through TS metadata/EIT/service information; reserve full video
  overlay for a later decode/overlay/re-encode path.

## Current State

This is the first scaffold. It builds the daemon shape with a stub NIM
controller, so hardware driver code can be ported into a contained module rather
than spread through the application.

```sh
cmake -S . -B build
cmake --build build
```

Running without a config uses a small default scan plan:

```sh
./build/wh-repeater
```

For the installed daemon, keep the mutable JSON configuration in
`/etc/wh-repeater/wh-repeater.json` and run it through systemd:

```sh
sudo install -m 0755 build/wh-repeater /usr/local/bin/wh-repeater
sudo install -d -m 0755 /etc/wh-repeater /var/www/wh-repeater
sudo install -m 0644 wh-repeater.json /etc/wh-repeater/wh-repeater.json
sudo install -m 0644 deploy/systemd/wh-repeater.service /etc/systemd/system/wh-repeater.service
sudo cp web/index.html web/styles.css web/app.js /var/www/wh-repeater/
sudo systemctl daemon-reload
sudo systemctl enable --now wh-repeater.service
```

The unit runs `/usr/local/bin/wh-repeater /etc/wh-repeater/wh-repeater.json`
with `WH_REPEATER_NIM=serit`, binds the local REST API on `127.0.0.1:8080`,
and logs to journald:

```sh
sudo systemctl status wh-repeater.service
sudo journalctl -u wh-repeater.service
```

## Planned Modules

- `NimController`: hardware access for Serit NIMs, STV0910 demods, STV6120 tuners,
  LNA control, and PIC/SPI transport stream input.
- `ScanScheduler`: dwell and retune policy for per-receiver scan lists.
- `SignalArbitrator`: decides which locked receiver is allowed to key the
  transmitter.
- `TsRouter`: routes 188-byte transport stream packets from the active input.
- `PlutoSink`: sends the selected TS to PlutoPlus.
- `IdentInserter`: injects repeater ident metadata, and later controls an
  optional video overlay pipeline.
