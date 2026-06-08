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
    void tick(std::chrono::steady_clock::time_point now);
    void write(std::span<const std::byte> packet) override;

    [[nodiscard]] MediaPipelineMode mode() const;

private:
    void ensureLibavReady();
    void enterRetransmit(std::chrono::steady_clock::time_point now);
    void enterFallback(std::chrono::steady_clock::time_point now, bool transmitEnabled);
    void enterIdle();
    void workerLoop();
    void stopWorker();
    void queueInput(std::span<const std::byte> packet);

    RepeaterConfig config_;
    PlutoSink output_;
    std::optional<ActiveInput> active_;
    std::chrono::steady_clock::time_point lastInput_{std::chrono::steady_clock::now()};
    MediaPipelineMode mode_{MediaPipelineMode::idle};
    bool beaconAllowed_{true};
    bool transmitEnabled_{false};
    std::optional<std::string> accessNotice_;
    mutable std::mutex mutex_;
    std::condition_variable inputReady_;
    std::deque<std::byte> inputQueue_;
    bool stopping_{false};
    std::thread worker_;
};

} // namespace whrepeater
