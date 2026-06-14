/*
 * ============================================================================
 *  wh-repeater - Media Timing Helpers
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Defines deterministic output clock calculations shared by the media
 *    pipeline and regression tests. Output PTS remains the single source of
 *    truth; source timestamps must only be mapped onto this clock.
 * ============================================================================
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace whrepeater {

inline constexpr int defaultOutputAudioSampleRate{48'000};
inline constexpr int defaultOutputAudioChannels{2};

int clampedOutputFrameRate(std::uint32_t configuredFrameRate);
int clampedOutputAudioChannels(std::uint32_t configuredChannels);
std::chrono::microseconds outputFrameInterval(int frameRate);
int outputAudioChannelsForSource(std::uint32_t configuredChannels, int sourceChannels);

void advanceOutputFrameClock(std::chrono::steady_clock::time_point& nextFrame,
                             std::chrono::microseconds frameInterval,
                             std::chrono::steady_clock::time_point now);

std::int64_t nextOutputVideoPts(std::int64_t lastVideoPts);
std::int64_t audioTargetSampleForVideoFrame(std::int64_t nextVideoFrame,
                                            int audioSampleRate,
                                            int frameRate);
bool audioFrameDue(std::int64_t audioSampleIndex,
                   int audioFrameSamples,
                   std::int64_t nextVideoFrame,
                   int audioSampleRate,
                   int frameRate);

} // namespace whrepeater
