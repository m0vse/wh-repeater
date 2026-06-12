/*
 * ============================================================================
 *  wh-repeater - Transport Stream Gateway Sink Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the raw MPEG-TS UDP gateway sink used when the Pi acts as a
 *    Winterhill receiver/gateway instead of locally transcoding video.
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
#include "whrepeater/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace whrepeater {

class TsGatewaySink final : public TsSink {
public:
    explicit TsGatewaySink(TsGatewayConfig config);
    ~TsGatewaySink() override;

    TsGatewaySink(const TsGatewaySink&) = delete;
    TsGatewaySink& operator=(const TsGatewaySink&) = delete;

    void setActive(std::optional<ActiveInput> active);
    void write(std::span<const std::byte> packet) override;
    void flush();
    TsGatewayStatus status() const;

private:
    void openSocket();
    void flushDatagram();

    TsGatewayConfig config_;
    int socket_{-1};
    std::vector<std::byte> datagram_;
    TsGatewayStatus status_;
};

} // namespace whrepeater
