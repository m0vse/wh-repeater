/*
 * ============================================================================
 *  wh-repeater - Media Pipeline Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the generated MPEG-TS media pipeline, fallback/status slate
 *    generation, optional RTMP forwarding, and video state notifications.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/pluto_sink.hpp"
#include "whrepeater/ts_router.hpp"

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace whrepeater {

enum class MediaPipelineMode {
    idle,
    retransmit,
    analogue,
    fallbackVideo,
    fallback,
};

class MediaPipeline final : public TsSink {
public:
    explicit MediaPipeline(RepeaterConfig config);
    ~MediaPipeline() override;

    MediaPipeline(const MediaPipeline&) = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;

    void select(std::optional<ActiveInput> input);
    void setBeaconAllowed(bool allowed);
    void setAccessNotice(std::optional<std::string> notice);
    void playFallbackVideo(std::string path);
    void stopFallbackVideo();
    void seekFallbackVideo(std::chrono::milliseconds position);
    void setPreviewEnabled(bool enabled);
    void tick(std::chrono::steady_clock::time_point now);
    void write(std::span<const std::byte> packet) override;

    [[nodiscard]] MediaPipelineMode mode() const;
    std::optional<std::string> takeStreamInfoUpdate();
    std::optional<std::string> takeFallbackVideoStatusUpdate();
    [[nodiscard]] std::optional<std::string> fallbackVideoStatus() const;

private:
    void ensureLibavReady();
    void enterRetransmit(std::chrono::steady_clock::time_point now);
    void enterAnalogue(std::chrono::steady_clock::time_point now);
    void enterFallbackVideo(std::string path);
    void enterFallback(std::chrono::steady_clock::time_point now, bool transmitEnabled);
    void enterIdle();
    void workerLoop();
    void stopWorker();
    void queueInput(std::span<const std::byte> packet);
    void setSessionStreamInfo(std::optional<std::string> streamInfo);
    void setFallbackVideoStatus(std::optional<std::string> statusJson);

    RepeaterConfig config_;
    PlutoSink output_;
    std::optional<ActiveInput> active_;
    std::chrono::steady_clock::time_point lastInput_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point liveRetryAfter_{};
    std::chrono::steady_clock::time_point analogueRetryAfter_{};
    MediaPipelineMode mode_{MediaPipelineMode::idle};
    bool beaconAllowed_{true};
    bool transmitEnabled_{false};
    std::optional<std::string> accessNotice_;
    std::optional<std::string> streamIndicator_;
    std::optional<std::string> sessionStreamInfo_;
    std::optional<std::string> pendingSessionStreamInfo_;
    std::optional<std::string> fallbackVideoPath_;
    std::optional<std::chrono::milliseconds> pendingFallbackVideoSeek_;
    std::optional<std::string> fallbackVideoStatus_;
    std::optional<std::string> pendingFallbackVideoStatus_;
    mutable std::mutex mutex_;
    std::condition_variable inputReady_;
    std::deque<std::byte> inputQueue_;
    bool stopping_{false};
    std::thread worker_;
};

} // namespace whrepeater
