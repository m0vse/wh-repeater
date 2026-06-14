#include "whrepeater/media_timing.hpp"

#include <algorithm>
#include <limits>

namespace whrepeater {

int clampedOutputFrameRate(std::uint32_t configuredFrameRate)
{
    return std::clamp(static_cast<int>(configuredFrameRate), 1, 50);
}

int clampedOutputAudioChannels(std::uint32_t configuredChannels)
{
    return std::clamp(static_cast<int>(configuredChannels), 1, 2);
}

std::chrono::microseconds outputFrameInterval(int frameRate)
{
    return std::chrono::microseconds{1'000'000 / std::max(1, frameRate)};
}

int outputAudioChannelsForSource(std::uint32_t configuredChannels, int sourceChannels)
{
    (void)sourceChannels;
    return clampedOutputAudioChannels(configuredChannels);
}

void advanceOutputFrameClock(std::chrono::steady_clock::time_point& nextFrame,
                             std::chrono::microseconds frameInterval,
                             std::chrono::steady_clock::time_point now)
{
    nextFrame += frameInterval;
    if (nextFrame < now) {
        nextFrame = now;
    }
}

std::int64_t nextOutputVideoPts(std::int64_t lastVideoPts)
{
    if (lastVideoPts == std::numeric_limits<std::int64_t>::min()) {
        return 0;
    }
    if (lastVideoPts < 0) {
        return 0;
    }
    return lastVideoPts + 1;
}

std::int64_t audioTargetSampleForVideoFrame(std::int64_t nextVideoFrame,
                                            int audioSampleRate,
                                            int frameRate)
{
    return nextVideoFrame * std::max(1, audioSampleRate) / std::max(1, frameRate);
}

bool audioFrameDue(std::int64_t audioSampleIndex,
                   int audioFrameSamples,
                   std::int64_t nextVideoFrame,
                   int audioSampleRate,
                   int frameRate)
{
    return audioSampleIndex + std::max(1, audioFrameSamples)
        <= audioTargetSampleForVideoFrame(nextVideoFrame, audioSampleRate, frameRate);
}

} // namespace whrepeater
