/*
 * ============================================================================
 *  wh-repeater - Signal Arbitrator Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the active-input selector that chooses the receiver allowed
 *    to drive the output path based on lock state and quality thresholds.
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

#include <optional>
#include <vector>

namespace whrepeater {

class SignalArbitrator {
public:
    explicit SignalArbitrator(const RepeaterConfig& config);

    std::optional<ActiveInput> choose(const std::vector<ReceiverStatus>& statuses);

private:
    bool isUsable(const ReceiverStatus& status) const;
    int receiverPriority(ReceiverId receiver) const;
    std::optional<ActiveInput> activeForReceiver(const std::vector<ReceiverStatus>& statuses,
                                                 ReceiverId receiver) const;

    RepeaterConfig config_;
    std::optional<ReceiverId> activeReceiver_;
};

} // namespace whrepeater
