/*
 * ============================================================================
 *  wh-repeater - PC Gateway UDP Input
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the PC-side UDP MPEG-TS input used when a Raspberry Pi
 *    Winterhill gateway forwards selected raw transport stream packets.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/ts_router.hpp"
#include "whrepeater/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace whrepeater {

struct PcGatewayInputStatus {
    bool enabled{};
    std::string listenAddress;
    std::uint16_t listenPort{};
    std::uint64_t transportPackets{};
    std::uint64_t datagrams{};
    std::uint64_t bytes{};
    std::uint64_t alignmentErrors{};
    std::uint64_t syncErrors{};
    std::chrono::steady_clock::time_point updatedAt{std::chrono::steady_clock::now()};
    std::optional<std::string> lastError;
};

class PcGatewayInput final {
public:
    explicit PcGatewayInput(GatewayInputConfig config);
    ~PcGatewayInput();

    PcGatewayInput(const PcGatewayInput&) = delete;
    PcGatewayInput& operator=(const PcGatewayInput&) = delete;

    void pump(TsSink& sink);

    [[nodiscard]] bool active(std::chrono::steady_clock::time_point now,
                              std::chrono::milliseconds timeout) const;
    [[nodiscard]] ReceiverStatus receiverStatus() const;
    [[nodiscard]] ActiveInput activeInput() const;
    [[nodiscard]] PcGatewayInputStatus status() const;

private:
    void openSocket();
    void recordError(std::string error);

    GatewayInputConfig config_;
    int socket_{-1};
    std::vector<std::byte> buffer_;
    PcGatewayInputStatus status_;
};

} // namespace whrepeater
