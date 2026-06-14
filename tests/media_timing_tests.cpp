#include "whrepeater/media_timing.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace whrepeater;

struct TestCase {
    std::string_view name;
    void (*run)();
};

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void clampsOutputFrameRates()
{
    expect(clampedOutputFrameRate(0) == 1, "output frame rate must clamp low values to 1 fps");
    expect(clampedOutputFrameRate(25) == 25, "output frame rate should preserve normal 25 fps output");
    expect(clampedOutputFrameRate(50) == 50, "output frame rate should preserve normal 50 fps output");
    expect(clampedOutputFrameRate(120) == 50, "output frame rate must clamp high values to 50 fps");
}

void calculatesFrameIntervals()
{
    expect(outputFrameInterval(25) == std::chrono::microseconds{40'000}, "25 fps interval should be 40 ms");
    expect(outputFrameInterval(50) == std::chrono::microseconds{20'000}, "50 fps interval should be 20 ms");
    expect(outputFrameInterval(0) == std::chrono::microseconds{1'000'000}, "invalid fps should fall back to 1 fps interval");
}

void advancesFrameClockWithoutAccumulatingBacklog()
{
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds{10};
    auto next = start;
    advanceOutputFrameClock(next, std::chrono::milliseconds{40}, start + std::chrono::milliseconds{1});
    expect(next == start + std::chrono::milliseconds{40}, "normal frame advance should add one frame interval");

    advanceOutputFrameClock(next, std::chrono::milliseconds{40}, start + std::chrono::seconds{1});
    expect(next == start + std::chrono::seconds{1}, "late frame advance should catch up to now instead of accumulating backlog");
}

void assignsOutputPtsMonotonically()
{
    expect(nextOutputVideoPts(std::numeric_limits<std::int64_t>::min()) == 0, "sentinel last PTS should start at zero");
    expect(nextOutputVideoPts(-1) == 0, "negative last PTS should start at zero");
    expect(nextOutputVideoPts(0) == 1, "video PTS should advance by one output frame");
    expect(nextOutputVideoPts(1234) == 1235, "video PTS should ignore source PTS and advance the output clock");
}

void mapsVideoFramesToAudioSamples()
{
    expect(audioTargetSampleForVideoFrame(0, 48'000, 25) == 0, "frame zero should target audio sample zero");
    expect(audioTargetSampleForVideoFrame(1, 48'000, 25) == 1920, "25 fps frame should map to 1920 audio samples");
    expect(audioTargetSampleForVideoFrame(50, 48'000, 50) == 48'000, "50 fps second should map to exactly one second of audio");
    expect(audioTargetSampleForVideoFrame(25, 44'100, 25) == 44'100, "sample-rate changes should map without hard-coded 48 kHz assumptions");
}

void schedulesAudioFramesAgainstOutputPts()
{
    expect(audioFrameDue(0, 1024, 1, 48'000, 25), "first 1024-sample frame should be due before 25 fps frame 1");
    expect(!audioFrameDue(1024, 1024, 1, 48'000, 25), "second 1024-sample frame should wait until enough video PTS has elapsed");
    expect(audioFrameDue(44'100 - 1024, 1024, 25, 44'100, 25), "last 44.1 kHz frame ending at one second should be due");
    expect(!audioFrameDue(44'100, 1024, 25, 44'100, 25), "audio must not run ahead of the output video PTS");
}

void preservesStereoOutputAudioContract()
{
    expect(defaultOutputAudioSampleRate == 48'000, "output audio sample rate must remain 48 kHz");
    expect(defaultOutputAudioChannels == 2, "default output audio should be stereo to avoid phase-cancelled mono downmixes");
    expect(clampedOutputAudioChannels(0) == 1, "configured audio channels should clamp low values to mono");
    expect(clampedOutputAudioChannels(1) == 1, "configured mono output should be allowed");
    expect(clampedOutputAudioChannels(2) == 2, "configured stereo output should be allowed");
    expect(clampedOutputAudioChannels(6) == 2, "configured output should clamp high values to stereo");
    expect(outputAudioChannelsForSource(defaultOutputAudioChannels, 1) == 2, "mono sources must map to default stereo output");
    expect(outputAudioChannelsForSource(defaultOutputAudioChannels, 2) == 2, "stereo sources must stay stereo by default");
    expect(outputAudioChannelsForSource(defaultOutputAudioChannels, 6) == 2, "5.1 sources must downmix to default stereo output");
    expect(outputAudioChannelsForSource(1, 2) == 1, "explicit mono output should remain possible");
}

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"clamps output frame rates", clampsOutputFrameRates},
        {"calculates frame intervals", calculatesFrameIntervals},
        {"advances frame clock without accumulating backlog", advancesFrameClockWithoutAccumulatingBacklog},
        {"assigns output PTS monotonically", assignsOutputPtsMonotonically},
        {"maps video frames to audio samples", mapsVideoFramesToAudioSamples},
        {"schedules audio frames against output PTS", schedulesAudioFramesAgainstOutputPts},
        {"preserves stereo output audio contract", preservesStereoOutputAudioContract},
    };

    int failures{};
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
        }
    }

    return failures == 0 ? 0 : 1;
}
