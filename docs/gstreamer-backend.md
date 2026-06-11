<!--
  ============================================================================
   wh-repeater - GStreamer Backend Notes
  ============================================================================
   Copyright (c) 2026 Phil Taylor (M0VSE)

   Purpose:
     Records the staged plan for adding an optional GStreamer media backend
     alongside the current FFmpeg implementation.

   Project notes:
     wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
     This note keeps the backend split aligned with the fixed-output stream
     contract and supervised media-process design.
  ============================================================================
-->

# GStreamer Backend

`media.backend` supports `ffmpeg` and `gstreamer`. FFmpeg remains the stable
default. The GStreamer option is an experimental complete media backend. When
selected, generated fallback/slides, fallback videos, DVB retransmit, analogue
capture, MPEG-TS output, and RTMP output are handled by GStreamer pipelines in
the supervised media child.

The FFmpeg persistent RTMP muxer is not constructed in GStreamer mode. RTMP is
owned by the active GStreamer source pipeline so there is only one writer.

The GStreamer implementation must stay inside the supervised media child. Raw
decoded frames must not cross the parent/child IPC boundary.

Current Pi runtime findings:

- GStreamer 1.26 is available after installing the development/runtime packages.
- `appsink`, `mpegtsmux`, `flvmux`, `rtmp2sink`, and `rtmpsink` are available.
- `GST_V4L2_ENABLE_PROBE=1` is required for the legacy stateful V4L2 codec
  elements to register on this Pi.
- The service must be able to open `/dev/video11` read/write. The current
  systemd unit runs as root; if a dedicated service user is introduced it must
  be granted codec device access, normally through udev/group policy.
- The service uses a dedicated `GST_REGISTRY` path and clears it on startup so
  GStreamer reprobes V4L2 codec features after permission, environment, kernel,
  or plugin changes.
- `v4l2h264dec` registers and successfully decodes through `/dev/video10`.
- `v4l2h264enc` registers against `/dev/video11` and works when the pipeline is
  fully constrained. GStreamer can otherwise infer H.264 level 1.0 from missing
  caps, which is too small for HD frames and fails during frame processing.
- Every hardware encode path must feed an explicit raw capsfilter before
  `v4l2h264enc`. Use `I420` first; `NV12` is the preferred fallback if I420
  becomes incompatible on a later stack. Do not rely on `videoconvert` alone to
  negotiate the encoder input.
- Every hardware encode path must put explicit H.264 level caps immediately
  after `v4l2h264enc`: `video/x-h264,level=(string)<configured-level>`. On this
  Pi stack, adding `profile=(string)high` to that capsfilter makes frame
  processing fail, even though High is exposed by the V4L2 driver. Request the
  configured profile through V4L2 `extra-controls` instead: Baseline
  `h264_profile=0`, Main `h264_profile=2`, or High `h264_profile=4`.
- `h264parse` must follow the encoder. Streaming paths must emit repeated
  headers, using `repeat_sequence_header=1` on the encoder and/or
  `h264parse config-interval=1`.
- The daemon enables V4L2 probing before `gst_init()`, self-tests
  `v4l2h264enc`, and uses it only if it really encodes. Software OpenH264 is not
  an acceptable fallback for output encoding; if GStreamer hardware encode is
  unavailable, the GStreamer output path must fail loudly.

Implementation status:

1. Generated fallback/slideshow uses GStreamer `appsrc` raw video/audio into
   H.264/AAC/MPEG-TS, with optional FLV/RTMP branch.
2. Fallback video playback uses GStreamer decode/scale/resample/transcode.
3. DVB retransmit uses GStreamer TS appsrc, demux/decode, ident overlay,
   transcode, and fixed-output muxing.
4. Analogue capture uses GStreamer `v4l2src` with generated silent audio.
5. GStreamer hardware H.264 decode is available. Hardware H.264 encode is
   probed and self-tested at runtime, then used only if it passes.
6. Wider production adoption should wait until the GStreamer V4L2 encoder path
   is proven stable. Software encode performance is intentionally irrelevant for
   production output because output H.264 encode must be hardware.
