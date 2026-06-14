/*
 * ============================================================================
 *  wh-repeater - Signal Arbitrator
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Chooses the active receiver from current status snapshots using lock
 *    state, MER, D-number, configured thresholds, and receiver ordering.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/signal_arbitrator.hpp"

#include <algorithm>
#include <iterator>

namespace whrepeater {

SignalArbitrator::SignalArbitrator(const RepeaterConfig& config)
    : config_{config}
{
}

std::optional<ActiveInput> SignalArbitrator::choose(const std::vector<ReceiverStatus>& statuses)
{
    if (activeReceiver_.has_value()) {
        if (auto active = activeForReceiver(statuses, *activeReceiver_); active.has_value()) {
            return active;
        }
        activeReceiver_.reset();
    }

    std::optional<ReceiverStatus> best;

    for (const auto& status : statuses) {
        if (!isUsable(status) || !status.target.has_value()) {
            continue;
        }

        if (!best.has_value()) {
            best = status;
            continue;
        }

        const auto priority = receiverPriority(status.receiver);
        const auto bestPriority = receiverPriority(best->receiver);
        const auto mer = status.merDb.value_or(0.0);
        const auto bestMer = best->merDb.value_or(0.0);

        if (priority < bestPriority || (priority == bestPriority && mer > bestMer)) {
            best = status;
        }
    }

    if (!best.has_value()) {
        return std::nullopt;
    }

    activeReceiver_ = best->receiver;
    return ActiveInput{
        .receiver = best->receiver,
        .target = *best->target,
        .status = *best,
    };
}

bool SignalArbitrator::isUsable(const ReceiverStatus& status) const
{
    const auto locked = status.state == ReceiverState::lockedDvbs || status.state == ReceiverState::lockedDvbs2;
    if (!locked) {
        return false;
    }

    if (status.merDb.has_value() && *status.merDb < config_.minimumMerDb) {
        return false;
    }

    if (status.dNumberDb.has_value() && *status.dNumberDb < config_.minimumDNumberDb) {
        return false;
    }

    return true;
}

int SignalArbitrator::receiverPriority(ReceiverId receiver) const
{
    const auto it = std::ranges::find_if(config_.receivers, [receiver](const ReceiverConfig& config) {
        return config.receiver == receiver;
    });

    if (it == config_.receivers.end()) {
        return static_cast<int>(config_.receivers.size());
    }

    return static_cast<int>(std::distance(config_.receivers.begin(), it));
}

std::optional<ActiveInput> SignalArbitrator::activeForReceiver(const std::vector<ReceiverStatus>& statuses,
                                                               ReceiverId receiver) const
{
    const auto it = std::ranges::find_if(statuses, [receiver](const ReceiverStatus& status) {
        return status.receiver == receiver;
    });
    if (it == statuses.end() || !isUsable(*it) || !it->target.has_value()) {
        return std::nullopt;
    }

    return ActiveInput{
        .receiver = it->receiver,
        .target = *it->target,
        .status = *it,
    };
}

} // namespace whrepeater
