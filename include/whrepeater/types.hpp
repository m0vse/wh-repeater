#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace whrepeater {

enum class Antenna {
    top,
    bottom,
};

enum class DvbSystem {
    dvbs,
    dvbs2,
    unknown,
};

enum class ReceiverState {
    idle,
    searching,
    foundHeader,
    lockedDvbs,
    lockedDvbs2,
    lost,
    timeout,
    fault,
};

struct ScanTarget {
    std::uint32_t frequencyKhz{};
    std::uint32_t symbolRateKs{};
    std::uint32_t localOscillatorKhz{};
    Antenna antenna{Antenna::top};
    DvbSystem system{DvbSystem::unknown};
    std::string label;
};

struct ReceiverId {
    int value{};

    friend bool operator==(ReceiverId lhs, ReceiverId rhs) = default;
};

struct ReceiverStatus {
    ReceiverId receiver;
    ReceiverState state{ReceiverState::idle};
    std::optional<ScanTarget> target;
    std::optional<double> merDb;
    std::optional<double> dNumberDb;
    std::optional<std::string> serviceName;
    std::optional<std::string> modulation;
    std::uint64_t transportPackets{};
    std::uint64_t continuityErrors{};
    std::chrono::steady_clock::time_point updatedAt{std::chrono::steady_clock::now()};
};

struct ActiveInput {
    ReceiverId receiver;
    ScanTarget target;
    ReceiverStatus status;
};

} // namespace whrepeater
