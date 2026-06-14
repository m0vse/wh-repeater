/*
 * ============================================================================
 *  wh-repeater - Media Process Supervisor Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the parent-side supervisor that isolates the FFmpeg/V4L2 media
 *    pipeline in a child process while preserving the TsSink control surface.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/media_pipeline.hpp"
#include "whrepeater/ts_router.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whrepeater {

class MediaProcess final : public TsSink {
public:
    explicit MediaProcess(RepeaterConfig config);
    ~MediaProcess() override;

    MediaProcess(const MediaProcess&) = delete;
    MediaProcess& operator=(const MediaProcess&) = delete;

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
    [[nodiscard]] std::optional<std::string> streamInfo() const;
    [[nodiscard]] std::optional<std::string> fallbackVideoStatus() const;

private:
    void ensureRunning();
    void startChild();
    void stopChild();
    void reapChild(bool restart);
    bool sendMessage(std::uint32_t type, std::span<const std::byte> payload, bool essential);
    void drainChildMessages();
    void flushTransportBuffer(bool essential);
    void replayState();

    RepeaterConfig config_;
    std::optional<ActiveInput> active_;
    bool beaconAllowed_{true};
    bool previewEnabled_{false};
    std::optional<std::string> accessNotice_;
    std::optional<std::string> fallbackVideoPath_;
    std::optional<std::chrono::milliseconds> fallbackVideoSeek_;
    MediaPipelineMode mode_{MediaPipelineMode::idle};
    int socket_{-1};
    int childPid_{-1};
    std::vector<std::byte> transportBuffer_;
    std::optional<std::vector<std::byte>> lastSelectPayload_;
    std::optional<std::vector<std::byte>> lastBeaconPayload_;
    std::optional<std::vector<std::byte>> lastNoticePayload_;
    std::optional<std::vector<std::byte>> lastPreviewPayload_;
    std::optional<std::string> streamInfo_;
    std::optional<std::string> fallbackVideoStatus_;
};

} // namespace whrepeater
