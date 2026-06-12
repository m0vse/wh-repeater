/*
 * ============================================================================
 *  wh-repeater - Pi Gateway Status Client
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the PC-side HTTP client that polls the Pi gateway status API so
 *    the operator dashboard can display the original NIM receiver state.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace whrepeater {

class GatewayStatusClient final {
public:
    explicit GatewayStatusClient(PiStatusConfig config);

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool due(std::chrono::steady_clock::time_point now) const;
    [[nodiscard]] std::optional<std::string> poll(std::chrono::steady_clock::time_point now);
    [[nodiscard]] const std::optional<std::string>& lastError() const;

private:
    [[nodiscard]] std::optional<std::string> fetchStatus();

    PiStatusConfig config_;
    std::chrono::steady_clock::time_point nextPoll_{};
    std::optional<std::string> lastError_;
};

} // namespace whrepeater
