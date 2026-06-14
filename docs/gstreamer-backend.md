<!--
  ============================================================================
   wh-repeater - GStreamer Backend Notes
  ============================================================================
   Copyright (c) 2026 Phil Taylor (M0VSE)

   Purpose:
     Records the current status of the optional GStreamer media backend.
  ============================================================================
-->

# GStreamer Backend

`media.backend` still accepts `gstreamer`, but the backend is experimental and
not the current production path.

The previous GStreamer implementation was built around Raspberry Pi legacy
stateful V4L2 H.264 codec elements such as `v4l2h264enc` and `v4l2h264dec`.
Those codec paths are intentionally retired. The split architecture treats the
Pi as the Winterhill/NIM control and raw MPEG-TS gateway device; it must not
depend on local H.264 encode/decode hardware.

The remaining useful GStreamer role is analogue capture plumbing through
`v4l2src`, if that path proves useful for a PC-side capture card. V4L2 capture
is separate from V4L2 codec support and should not be removed as part of codec
cleanup.

If the GStreamer backend is revived for PC-side media output, it must use a
PC-appropriate encoder path and obey `docs/media-stream-contract.md`, including:

- fixed configured output profile for every source;
- AAC 48 kHz output-channel parity with the FFmpeg backend, including stereo as
  the default. The previous GStreamer appsrc pipelines used `channels=1`; that
  must not be carried forward as the default because stereo fallback/test media
  can cancel to silence when downmixed to mono;
- output muxer PTS as the single timing source;
- no source-driven output PTS or frame-rate pacing;
- no Pi V4L2 H.264 codec dependency.

Until that work is done, the FFmpeg backend is the stable media path.
