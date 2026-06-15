/*
 * ============================================================================
 *  wh-repeater - Media Stream Contract
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Records the fixed output-stream invariants that every media source and
 *    fallback path must obey.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

# Media Stream Contract

This contract applies to media-producing modes such as `pc-gateway` and the
legacy `local-transcode` path. In `ts-gateway` mode the Pi bypasses the media
worker entirely and forwards selected raw 188-byte MPEG-TS packets over UDP;
FFmpeg, GStreamer, and H.264 codec availability must not be required for that
gateway path.

The output stream is continuous and fixed-format. Source changes must never
change the advertised output parameters or interrupt the RTMP/output stream.

Required invariants:

- The encoded video parameters are owned by the repeater output configuration,
  not by the active input.
- PC-side H.264 output should use the configured hardware encoder when
  available, currently VAAPI on Intel test hardware. Software H.264 encode is
  acceptable only as a diagnostic/development fallback and must not be re-added
  as a Pi-side requirement.
- Legacy Raspberry Pi V4L2 H.264 codec paths are intentionally retired. The Pi
  `ts-gateway` role must not depend on local H.264 encode/decode devices.
- Hardware decode is preferred for received or file inputs when it is stable for
  that source and target hardware. Software decode fallback is allowed when
  hardware decode is unavailable, unsuitable for a specific stream, or less
  stable.
- The configured output width, height, and frame rate define the single video
  profile used by generated fallback, live inputs, Pluto TS output, and RTMP.
- The configured H.264 profile and level are part of that same output contract.
  Profile may be `baseline`, `main`, or `high`; level may be explicit or `auto`.
  Automatic level selection uses 3.1 for outputs up to 1280x720 and 4 above
  1280x720.
- Fallback slides, fallback videos, DVB receive, analogue capture, access
  notices, sleep slates, and testcards must all be transcoded into the same
  configured output video format.
- Audio must always be encoded into the same configured output audio format.
  The FFmpeg output path currently uses AAC at 48 kHz with configurable output
  channels for both Pluto TS and RTMP. The default is stereo. With stereo output,
  stereo sources must remain stereo, mono sources must be duplicated or
  otherwise mapped into valid stereo, and multichannel sources must be downmixed
  to stereo. Do not collapse stereo to mono unless the operator explicitly
  configures mono output: phase-opposed diagnostic audio can cancel to silence.
- A source change must not recreate the RTMP stream with different codec
  parameters, frame dimensions, frame rate, time base, or audio parameters.
- Invalid, missing, or stalled input must be replaced with valid generated
  frames and valid audio/silence so encoder time continues monotonically.
- The output muxer PTS is the single source of truth for stream timing. Input
  PTS/DTS, file timestamps, decoder delivery rate, demux read speed, queue
  depth, wall clock, and API commands may be used only to map source media onto
  the output timeline; they must never overwrite, advance, or independently
  pace the output PTS.
- The H.264 encoder boundary must validate every submitted frame. A frame is
  valid only if it matches the configured encoder pixel format, dimensions, and
  timing, has populated data planes, and has a monotonic presentation timestamp.
- Invalid video must be replaced before encode with a generated frame in the
  configured output format. Invalid audio must be replaced before encode with
  silence in the configured output audio format.
- Input demux/decode errors are source errors, not encoder input. They must not
  be forwarded as malformed frames or used to renegotiate output parameters.
- RTMP viewers may need to reconnect after a daemon or network failure, but not
  because the daemon switched media source.
- RTMP output is a mirror of the fixed encoded output stream. Network writes
  must not be allowed to stall the media loop for multiple seconds; RTMP errors
  should close that mirror and reconnect while the local encoder/Pluto output
  continues.
- During fallback-video file playback, stability and uninterrupted output take
  priority over lowest possible CPU use. Using one hot CPU thread for software
  decode, timestamp pacing, scaling, and audio sync is acceptable if it keeps the
  encoder and RTMP/output stream stable.
- Hard rule: fallback-video file playback must be paced against the common
  output muxer PTS for both video and audio. The file reader/decoder may buffer a
  small amount ahead of the output clock to absorb decode jitter, but it must
  block when decoded video or decoded audio is more than the permitted PTS lead
  ahead of `nextVideoPts()` / `nextAudioPts()`. Do not pace fallback videos by
  queue depth alone, decoder throughput, source frame rate sleeps, or source
  timestamps. Those approaches have previously caused real playback-speed and
  A/V timing regressions.
- Fallback videos should be prepared to match the configured output width,
  height, and frame rate whenever practical. Mismatched files are still accepted
  and transcoded into the fixed output profile, but software scaling, frame-rate
  conversion, and hardware-decode ineligibility can increase CPU load and cause
  startup frame holds.
- Exact-match fallback videos may bypass software scaling/copying, but only when
  decoded dimensions, pixel format, and sample aspect already match the output
  contract. All other fallback videos must use the normal scale/pillarbox path.

Implementation implication: fallback video playback is an input source, not a
new output profile. It must decode and resample into the existing stream profile
instead of configuring a separate encoder/header for the file.

Output path:

- Every media source must feed decoded frames/audio into the common output
  encoder path.
- The media path runs inside the supervised media child process. The parent
  daemon sends commands and selected TS packets, but raw decoded video frames
  stay inside the child process to avoid IPC copies.
- The FFmpeg backend is the stable implementation. The optional GStreamer
  backend is intended as a complete media replacement and must obey the same
  fixed-output contract, including mandatory hardware H.264 encode.
- The encoded transport stream is written to the Pluto UDP sink.
- The same encoded packets are mirrored to the persistent RTMP muxer.
- Once RTMP has accepted its first video/audio profile, later source changes
  must match that profile. Mismatches are output-contract violations and must
  not renegotiate the live stream.

Future option: editable HTML/CSS slates

- Sleep, access, signal-received, diagnostic, and testcard slates may later move from
  hardcoded C++ strings/layout into operator-editable local HTML/CSS templates.
- Templates should be local files with local assets and a small set of repeater
  variables substituted by the daemon, for example callsign, receiver name,
  service name, video format, access duration, and sleep schedule times.
- The preferred architecture is render-on-change into a cached bitmap at the
  configured output size, then repeat that bitmap through the existing generated
  frame path. Slate rendering must not run in the per-frame critical path.
- The selected first renderer is packaged headless Chromium, called through the
  `render-slate-html` wrapper. It is heavier than `litehtml`, but it is
  available on Debian 13, accurately renders normal HTML/CSS, supports fixed
  size local-file screenshots, and can run outside the per-frame media loop.
  `litehtml` remains a possible later embedded renderer if Chromium packaging
  becomes too heavy. Embedded CEF should be treated as a heavier option due to
  Chromium runtime, packaging, helper processes, and operational complexity.
- JavaScript and remote network assets should be avoided for first
  implementation. Static HTML/CSS with local images is enough for the expected
  slate use case and is easier to make deterministic.
- Rendering failure must not interrupt output. The media path should reuse the
  last good rendered slate, or fall back to a built-in emergency slate if no
  rendered slate is available.
- The repository now contains starter templates under `slates/default/`.
  Deployments install them below the service config directory so operators can
  edit local copies without rebuilding the C++ binaries.
