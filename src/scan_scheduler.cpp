#include "whrepeater/scan_scheduler.hpp"

#include <utility>

namespace whrepeater {

ScanScheduler::ScanScheduler(std::vector<ReceiverConfig> receivers)
    : receivers_{std::move(receivers)}
{
}

void ScanScheduler::tick(NimController& nim, std::chrono::steady_clock::time_point now)
{
    for (const auto& receiver : receivers_) {
        if (receiver.targets.empty()) {
            continue;
        }

        auto& cursor = cursors_[receiver.receiver.value];
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
