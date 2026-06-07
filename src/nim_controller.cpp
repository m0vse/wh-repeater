#include "whrepeater/nim_controller.hpp"

#include <algorithm>
#include <chrono>

namespace whrepeater {

void StubNimController::initialise()
{
    statuses_.clear();
    for (int receiver = 1; receiver <= 4; ++receiver) {
        statuses_.push_back(ReceiverStatus{.receiver = ReceiverId{receiver}});
    }
}

void StubNimController::tune(ReceiverId receiver, const ScanTarget& target)
{
    auto it = std::ranges::find_if(statuses_, [receiver](const ReceiverStatus& status) {
        return status.receiver == receiver;
    });

    if (it == statuses_.end()) {
        statuses_.push_back(ReceiverStatus{.receiver = receiver});
        it = statuses_.end() - 1;
    }

    it->state = ReceiverState::searching;
    it->target = target;
    it->updatedAt = std::chrono::steady_clock::now();
}

void StubNimController::stop(ReceiverId receiver)
{
    auto it = std::ranges::find_if(statuses_, [receiver](const ReceiverStatus& status) {
        return status.receiver == receiver;
    });

    if (it != statuses_.end()) {
        it->state = ReceiverState::idle;
        it->target.reset();
        it->updatedAt = std::chrono::steady_clock::now();
    }
}

std::vector<ReceiverStatus> StubNimController::pollStatus()
{
    return statuses_;
}

} // namespace whrepeater
