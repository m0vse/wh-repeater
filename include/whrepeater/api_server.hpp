/*
 * ============================================================================
 *  wh-repeater - API Server Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the localhost REST API server used by the web management UI
 *    to read live status, fetch configuration, and submit updated repeater
 *    settings.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/pluto_mqtt_status.hpp"
#include "whrepeater/types.hpp"

#include <cstdint>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace whrepeater {

struct ApiServerConfig {
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t port{8080};
};

class ApiServer {
public:
    explicit ApiServer(ApiServerConfig config = {});
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    void start();
    void stop();

    void updateConfig(RepeaterConfig config);
    void updateStatus(std::vector<ReceiverStatus> statuses, std::optional<ActiveInput> active);
    void updateAnalogueStatus(AnalogueStatus status);
    void updatePlutoStatus(PlutoMqttStatus status);
    void updateTsGatewayStatus(TsGatewayStatus status);
    void updateRemoteGatewayStatus(std::optional<std::string> statusJson);
    void updateReceiverTransitions(std::vector<ReceiverTransition> transitions);
    void updateBeaconSchedule(bool active);
    std::optional<RepeaterConfig> takePendingConfig();
    std::optional<std::string> takePendingFallbackVideo();
    bool takePendingFallbackVideoStop();
    std::optional<bool> takePendingPreviewEnabled();

private:
    void serve();
    void handleClient(int clientFd);
    std::string configJson() const;
    std::string statusJson() const;

    ApiServerConfig serverConfig_;
    mutable std::mutex snapshotMutex_;
    RepeaterConfig repeaterConfig_;
    std::vector<ReceiverStatus> statuses_;
    std::optional<AnalogueStatus> analogueStatus_;
    std::optional<PlutoMqttStatus> plutoStatus_;
    std::optional<TsGatewayStatus> tsGatewayStatus_;
    std::optional<std::string> remoteGatewayStatusJson_;
    std::vector<ReceiverTransition> receiverTransitions_;
    bool beaconScheduleActive_{true};
    std::optional<ActiveInput> active_;
    std::optional<RepeaterConfig> pendingConfig_;
    std::optional<std::string> pendingFallbackVideo_;
    bool pendingFallbackVideoStop_{false};
    std::optional<bool> pendingPreviewEnabled_;
    std::atomic_bool running_{false};
    int serverFd_{-1};
    std::thread serverThread_;
};

} // namespace whrepeater
