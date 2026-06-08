/*
 * ============================================================================
 *  wh-repeater - Shared Domain Types
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Defines core repeater domain types, receiver identifiers, scan targets, receiver status snapshots, and active-input selection records.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace whrepeater {

using TransportPacket = std::array<std::byte, 188>;

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
    std::string fec{"auto"};
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

struct AnalogueStatus {
    bool enabled{};
    bool present{};
    bool ready{};
    bool locked{};
    bool cameraRunning{};
    std::string model;
    std::string selectedSource;
    std::string activeSource;
    std::string firmwareVersion;
    std::string longVersion;
    std::string hardwareId;
    std::uint8_t rawLock{};
    std::chrono::steady_clock::time_point updatedAt{std::chrono::steady_clock::now()};
    std::optional<std::string> error;
};

struct ActiveInput {
    ReceiverId receiver;
    ScanTarget target;
    ReceiverStatus status;
};

} // namespace whrepeater
