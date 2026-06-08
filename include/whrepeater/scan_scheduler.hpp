/*
 * ============================================================================
 *  wh-repeater - Scan Scheduler Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the receiver scan scheduler responsible for dwell timing,
 *    scan target rotation, lock hold, and post-loss hang timing.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/nim_controller.hpp"

#include <chrono>
#include <vector>
#include <unordered_map>

namespace whrepeater {

class ScanScheduler {
public:
    explicit ScanScheduler(std::vector<ReceiverConfig> receivers);

    void tick(NimController& nim, const std::vector<ReceiverStatus>& statuses, std::chrono::steady_clock::time_point now);

private:
    struct Cursor {
        std::size_t index{};
        std::chrono::steady_clock::time_point nextRetune{};
        std::chrono::steady_clock::time_point hangUntil{};
        bool tuned{};
        bool wasLocked{};
    };

    std::vector<ReceiverConfig> receivers_;
    std::unordered_map<int, Cursor> cursors_;
};

} // namespace whrepeater
