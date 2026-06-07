#pragma once

#include "whrepeater/types.hpp"

#include <vector>

namespace whrepeater {

class NimController {
public:
    virtual ~NimController() = default;

    virtual void initialise() = 0;
    virtual void tune(ReceiverId receiver, const ScanTarget& target) = 0;
    virtual void stop(ReceiverId receiver) = 0;
    virtual std::vector<ReceiverStatus> pollStatus() = 0;
};

class StubNimController final : public NimController {
public:
    void initialise() override;
    void tune(ReceiverId receiver, const ScanTarget& target) override;
    void stop(ReceiverId receiver) override;
    std::vector<ReceiverStatus> pollStatus() override;

private:
    std::vector<ReceiverStatus> statuses_;
};

} // namespace whrepeater
