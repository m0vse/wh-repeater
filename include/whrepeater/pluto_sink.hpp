/*
 * ============================================================================
 *  wh-repeater - Pluto Sink Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares UDP transport-stream output and MQTT control for
 *    PlutoPlus/F5OEO firmware, including transmit mute/PTT and modulation
 *    settings.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/ts_router.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace whrepeater {

class PlutoSink final : public TsSink {
public:
    explicit PlutoSink(PlutoConfig config);
    ~PlutoSink() override;

    PlutoSink(const PlutoSink&) = delete;
    PlutoSink& operator=(const PlutoSink&) = delete;

    PlutoSink(PlutoSink&& other) noexcept;
    PlutoSink& operator=(PlutoSink&& other) noexcept;

    void write(std::span<const std::byte> packet) override;
    void writeMuxData(std::span<const std::byte> data);
    void setTransmitEnabled(bool enabled);
    void setPreviewEnabled(bool enabled);
    void reconfigureTransmitter(bool transmitEnabled);

private:
    void openSocket();
    void flushDatagram();
    void openPreviewSocket();
    void writePreviewData(std::span<const std::byte> data);
    void flushPreviewDatagram();
    void configureTransmitter();
    void publishTransmitState(bool enabled);
    void publishControl(std::string_view suffix, std::string_view payload);

    PlutoConfig config_;
    int socket_{-1};
    int previewSocket_{-1};
    std::vector<std::byte> datagram_;
    std::vector<std::byte> previewDatagram_;
    bool configured_{false};
    bool transmitEnabled_{false};
    bool previewEnabled_{false};
};

} // namespace whrepeater
