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
- The configured output width, height, and frame rate define the single video
  profile used by generated fallback, live inputs, Pluto TS output, and RTMP.
- Fallback slides, fallback videos, DVB receive, analogue capture, access
  notices, sleep slates, and testcards must all be transcoded into the same
  configured output video format.
- Audio must always be encoded into the same configured output audio format.
- A source change must not recreate the RTMP stream with different codec
  parameters, frame dimensions, frame rate, time base, or audio parameters.
- Invalid, missing, or stalled input must be replaced with valid generated
  frames and valid audio/silence so encoder time continues monotonically.
- RTMP viewers may need to reconnect after a daemon or network failure, but not
  because the daemon switched media source.
- During fallback-video file playback, stability and uninterrupted output take
  priority over lowest possible CPU use. Using one hot CPU thread for software
  decode, timestamp pacing, scaling, and audio sync is acceptable if it keeps the
  hardware encoder and RTMP/output stream stable.

Implementation implication: fallback video playback is an input source, not a
new output profile. It must decode and resample into the existing stream profile
instead of configuring a separate encoder/header for the file.

Output path:

- Every media source must feed decoded frames/audio into the common output
  encoder path.
- The media path runs inside the supervised media child process. The parent
  daemon sends commands and selected TS packets, but raw decoded video frames
  stay inside the child process to avoid IPC copies.
- The encoded transport stream is written to the Pluto UDP sink.
- The same encoded packets are mirrored to the persistent RTMP muxer.
- Once RTMP has accepted its first video/audio profile, later source changes
  must match that profile. Mismatches are output-contract violations and must
  not renegotiate the live stream.
