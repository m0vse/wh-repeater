/*
 * ============================================================================
 *  wh-repeater - Pluto MQTT Status Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the MQTT status client used to monitor Pluto/F5OEO transmit
 *    state and expose it to the management API and web UI.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace whrepeater {

struct PlutoMqttStatus {
    bool enabled{false};
    bool connected{false};
    std::string host;
    std::uint16_t port{};
    std::string protocol;
    std::string deviceId;
    std::string callsign;
    std::map<std::string, std::string> values;
    std::optional<std::string> error;
    std::chrono::steady_clock::time_point updatedAt{std::chrono::steady_clock::now()};
};

class PlutoMqttStatusWorker final {
public:
    explicit PlutoMqttStatusWorker(PlutoConfig config);
    ~PlutoMqttStatusWorker();

    PlutoMqttStatusWorker(const PlutoMqttStatusWorker&) = delete;
    PlutoMqttStatusWorker& operator=(const PlutoMqttStatusWorker&) = delete;

    [[nodiscard]] PlutoMqttStatus snapshot() const;

private:
    void workerLoop();
    void setDisconnected(std::string error);
    void setConnected(bool connected);
    void recordStatus(std::string topic, std::string payload);

    PlutoConfig config_;
    mutable std::mutex mutex_;
    PlutoMqttStatus status_;
    std::atomic_bool stopping_{false};
    std::thread worker_;
};

} // namespace whrepeater
