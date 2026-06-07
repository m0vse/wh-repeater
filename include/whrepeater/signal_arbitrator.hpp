#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/types.hpp"

#include <optional>
#include <vector>

namespace whrepeater {

class SignalArbitrator {
public:
    explicit SignalArbitrator(const RepeaterConfig& config);

    std::optional<ActiveInput> choose(const std::vector<ReceiverStatus>& statuses) const;

private:
    bool isUsable(const ReceiverStatus& status) const;
    int receiverPriority(ReceiverId receiver) const;

    RepeaterConfig config_;
};

} // namespace whrepeater
