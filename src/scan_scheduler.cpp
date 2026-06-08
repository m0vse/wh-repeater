/*
 * ============================================================================
 *  wh-repeater - Receiver Scan Scheduler
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements scan target rotation, retune dwell timing, lock hold
 *    behaviour, and hang-time state after a received signal is lost.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/scan_scheduler.hpp"

#include <algorithm>
#include <utility>

namespace whrepeater {
namespace {

bool isLocked(ReceiverState state)
{
    return state == ReceiverState::lockedDvbs || state == ReceiverState::lockedDvbs2;
}

} // namespace

ScanScheduler::ScanScheduler(std::vector<ReceiverConfig> receivers)
    : receivers_{std::move(receivers)}
{
}

void ScanScheduler::tick(NimController& nim, const std::vector<ReceiverStatus>& statuses, std::chrono::steady_clock::time_point now)
{
    for (const auto& receiver : receivers_) {
        if (!receiver.enabled || receiver.targets.empty()) {
            continue;
        }

        auto& cursor = cursors_[receiver.receiver.value];
        const auto statusIt = std::find_if(statuses.begin(), statuses.end(), [&](const ReceiverStatus& status) {
            return status.receiver == receiver.receiver;
        });
        const auto locked = statusIt != statuses.end() && isLocked(statusIt->state);
        if (locked) {
            cursor.wasLocked = true;
            cursor.hangUntil = {};
            cursor.nextRetune = now + receiver.dwellTime;
            continue;
        }
        if (cursor.wasLocked) {
            cursor.wasLocked = false;
            cursor.hangUntil = now + receiver.hangTime;
        }
        if (cursor.hangUntil > now) {
            continue;
        }

        if (!cursor.tuned || now >= cursor.nextRetune) {
            const auto& target = receiver.targets[cursor.index % receiver.targets.size()];
            nim.tune(receiver.receiver, target);
            cursor.index = (cursor.index + 1) % receiver.targets.size();
            cursor.nextRetune = now + receiver.dwellTime;
            cursor.tuned = true;
        }
    }
}

} // namespace whrepeater
