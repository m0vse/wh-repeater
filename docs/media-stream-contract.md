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

The output stream is continuous and fixed-format. Source changes must never
change the advertised output parameters or interrupt the RTMP/output stream.

Required invariants:

- The encoded video parameters are owned by the repeater output configuration,
  not by the active input.
- H.264 output encoding must always use the Raspberry Pi hardware encoder. The
  daemon must not silently fall back to `libx264`, OpenH264, or any other
  software H.264 encoder for generated fallback, fallback video playback, live
  DVB retransmit, analogue capture, Pluto TS output, or RTMP output.
- If the hardware encoder cannot be opened, the media path must fail loudly and
  remain supervised/restartable. High-CPU software encode is not an acceptable
  degraded mode for the installed repeater.
- Hardware decode is preferred for received or file inputs, but software decode
  fallback is allowed when hardware decode is unavailable, unsuitable for a
  specific stream, or less stable on the current Pi stack.
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
- A source change must not recreate the RTMP stream with different codec
  parameters, frame dimensions, frame rate, time base, or audio parameters.
- Invalid, missing, or stalled input must be replaced with valid generated
  frames and valid audio/silence so encoder time continues monotonically.
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
  hardware encoder and RTMP/output stream stable.
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
