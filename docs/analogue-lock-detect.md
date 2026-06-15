# Analogue Lock Detect Options

The PC-side analogue capture path can receive usable video and audio from a USB
HDMI capture device, but UVC capture hardware does not reliably expose true
input lock. Some devices keep streaming black or vendor status frames even when
the analogue receiver has no valid signal.

For production, prefer a real lock-detect signal from the analogue receiver over
image heuristics.

## Current Test Setup

The current test setup uses:

- video: `/dev/video0`, `mjpeg`, `1280x720`, `50 fps`
- audio: `hw:U25854,0`, `48000 Hz`, stereo
- audio delay: `120 ms`
- lock mode: `manual`

`manual` is suitable for bench testing only. It treats the analogue input as
locked whenever enabled, so it can take over even if the external source is
blank.

## Production Options

### USB GPIO On The PC

Add a small USB GPIO device to the PC and connect the analogue receiver's
lock-detect output to it.

This keeps all analogue input decisions local to the PC-side media processor.
It is the cleanest match if the analogue receiver and capture card are installed
near the PC.

Candidate hardware:

- FT232H-style USB GPIO adapters
- MCP2221 USB GPIO/I2C adapters
- RP2040/Arduino-class board with a tiny serial/HID protocol
- industrial USB digital input modules

The daemon would need a new analogue lock mode such as `usb-gpio` or
`serial-line`, depending on the adapter interface.

### Pi As GPIO Bridge

Connect the analogue receiver lock-detect output to the Raspberry Pi, then have
`ts-gateway` report that state to the PC API poller.

This is a good fit if the receiver hardware is physically near the Winterhill/Pi
stack. It preserves the split architecture: the Pi reports hardware state, while
the PC remains responsible for capture, arbitration, encoding, and output.

The daemon would need a new analogue lock mode such as `pi-status`, using a
field reported by the Pi service.

### USB Serial Control Line

Use a USB serial adapter and wire the lock-detect output to a modem status line
such as CTS, DSR, or DCD.

This is electrically and mechanically simple, and many adapters expose these
lines reliably. It would need a small reader in the PC daemon and a lock mode
such as `serial-line`.

### Image Or Audio Heuristics

Use frame brightness, frame changes, known no-input images, or audio RMS to
guess whether a useful signal is present.

This should remain a fallback or diagnostic option only. It cannot distinguish
all valid programme material from black/static/status output, and it depends on
capture-card behaviour.

## Electrical Notes

Do not connect a receiver lock output directly to an unknown GPIO input.

- Confirm the lock output voltage and polarity.
- Level shift or isolate if the output is not safe for the chosen input.
- Use `gpioActiveHigh` or the equivalent polarity setting after measuring the
actual signal.
- Keep a debounce period before allowing analogue takeover.

The existing `gpio` lock mode is useful only where the media processor has a
Linux GPIO line available. A normal PC usually does not, so production PC builds
will likely need one of the bridge/adapter approaches above.
