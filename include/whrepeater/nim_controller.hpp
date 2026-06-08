#pragma once

#include "whrepeater/types.hpp"

#include <filesystem>
#include <memory>
#include <cstddef>
#include <vector>

namespace whrepeater {

class NimController {
public:
    virtual ~NimController() = default;

    virtual void initialise() = 0;
    virtual void tune(ReceiverId receiver, const ScanTarget& target) = 0;
    virtual void stop(ReceiverId receiver) = 0;
    virtual void shutdown() = 0;
    virtual std::vector<ReceiverStatus> pollStatus() = 0;
    virtual std::vector<TransportPacket> drainTransportPackets(ReceiverId receiver, std::size_t maxPackets) = 0;
};

class StubNimController final : public NimController {
public:
    void initialise() override;
    void tune(ReceiverId receiver, const ScanTarget& target) override;
    void stop(ReceiverId receiver) override;
    void shutdown() override;
    std::vector<ReceiverStatus> pollStatus() override;
    std::vector<TransportPacket> drainTransportPackets(ReceiverId receiver, std::size_t maxPackets) override;

private:
    std::vector<ReceiverStatus> statuses_;
};

struct SeritNimControllerConfig {
    std::filesystem::path i2cDevice{"/dev/i2c-1"};
    std::filesystem::path whDriverDevice{"/dev/whdriver-4v00"};
    std::filesystem::path interruptsFile{"/proc/interrupts"};
};

class SeritNimController final : public NimController {
public:
    explicit SeritNimController(SeritNimControllerConfig config = {});
    ~SeritNimController() override;

    void initialise() override;
    void tune(ReceiverId receiver, const ScanTarget& target) override;
    void stop(ReceiverId receiver) override;
    void shutdown() override;
    std::vector<ReceiverStatus> pollStatus() override;
    std::vector<TransportPacket> drainTransportPackets(ReceiverId receiver, std::size_t maxPackets) override;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace whrepeater
