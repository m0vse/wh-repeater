/*
 * ============================================================================
 *  wh-repeater - Ident Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the lightweight ident helper retained for service-ident timing and future transport-stream metadata insertion.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/types.hpp"

#include <chrono>
#include <optional>

namespace whrepeater {

class IdentInserter {
public:
    explicit IdentInserter(IdentConfig config);

    void update(std::optional<ActiveInput> input, std::chrono::steady_clock::time_point now);

private:
    IdentConfig config_;
    std::chrono::steady_clock::time_point nextIdent_{};
};

} // namespace whrepeater
