/*
 * ============================================================================
 *  wh-repeater - Parked SD1 Analogue Capture Notes
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Records the Lintest Systems SD1 investigation so the temporary removal of
 *    runtime support can be reversed without repeating the same hardware tests.
 *
 *  Project notes:
 *    SD1 support is intentionally parked until the exact CSI-2 output mode is
 *    known. The daemon keeps the controller and media code in-tree, but it does
 *    not start the SD1 worker or select SD1 as an active receiver.
 * ============================================================================
 */

# SD1 Analogue Capture Parked State

SD1 runtime support is disabled for now. The code remains in-tree for a future
restore, but the daemon does not poll the SD1 controller, list RX5, or switch
the media pipeline into analogue capture mode.

## Confirmed Findings

- The PiVideo control processor responds at `0x40` on `/dev/i2c-0`.
- The SD1 OV5647-emulated camera endpoint responds at `0x36` on camera mux bus
  `i2c-10`.
- With the ribbon on CSI0, a shim sensor on `i2c-10` linked to CSI0 creates
  `/dev/video0`.
- Starting `/dev/video0` makes PiVideo report `Raspberry Pi camera port is
  active`.
- The stock `ov5647` driver is not usable as-is because it tries to program and
  read real OV5647 registers. The SD1 emulation NACKed stream-start register
  access around `0x0100`.
- The local `wh_sd1_sensor` shim can bind and `VIDIOC_STREAMON` succeeds, but
  no completed frames arrive from Unicam.

## Current Runtime State

- `analogue.sd1.enabled` defaults to `false`.
- `src/daemon.cpp` has `kSd1SupportEnabled = false`, which prevents the SD1
  status worker from starting even if config is changed.
- The web SD1 configuration section is hidden, but the HTML controls are still
  present for easy restoration.

## Parked Files

- `src/sd1_controller.cpp` and `include/whrepeater/sd1_controller.hpp`: PiVideo
  register polling.

The experimental local kernel module, device-tree overlays, and one-off
activation diagnostic have been removed from the active tree. They were test
artifacts rather than confirmed SD1 support. Recreate those pieces from the
findings below only if the CSI format is confirmed and SD1 work resumes.

## Restore Checklist

1. Confirm the SD1 CSI-2 output data type, resolution, frame rate, lane count,
   clock mode, and link frequency.
2. Create a minimal sensor driver or overlay that matches the confirmed mode.
3. Install the overlay/module if that route is still selected, then enable their
   boot-time loading.
4. Set `kSd1SupportEnabled = true` in `src/daemon.cpp`.
5. Remove `hidden` from the SD1 section in `web/index.html`.
6. Set `analogue.sd1.enabled` true in the live config only after direct V4L2
   capture produces frames.
