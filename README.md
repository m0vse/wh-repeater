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

A future parser will load TOML in the shape shown in
`config/wh-repeater.example.toml`.

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
