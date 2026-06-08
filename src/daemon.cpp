/*
 * ============================================================================
 *  wh-repeater - Daemon Orchestrator
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Runs the main service loop, owns hardware and media subsystems,
 *    handles API config reloads, signal shutdown, scheduling, arbitration,
 *    and transmit state.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/daemon.hpp"

#include "whrepeater/api_server.hpp"
#include "whrepeater/hardware_ptt.hpp"
#include "whrepeater/ident.hpp"
#include "whrepeater/media_pipeline.hpp"
#include "whrepeater/nim_controller.hpp"
#include "whrepeater/pluto_mqtt_status.hpp"
#include "whrepeater/scan_scheduler.hpp"
#include "whrepeater/sd1_controller.hpp"
#include "whrepeater/signal_arbitrator.hpp"
#include "whrepeater/ts_router.hpp"

#include <chrono>
#include <atomic>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace whrepeater {
namespace {

std::atomic_bool shutdownRequested{false};

void requestShutdown(int)
{
    shutdownRequested.store(true);
}

void installSignalHandlers()
{
    struct sigaction action {};
    action.sa_handler = requestShutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

int minutesSinceMidnight(std::string_view clock)
{
    return ((clock[0] - '0') * 10 + (clock[1] - '0')) * 60
        + ((clock[3] - '0') * 10 + (clock[4] - '0'));
}

bool beaconScheduleActive(const BeaconScheduleConfig& schedule)
{
    if (!schedule.enabled) {
        return true;
    }

    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const auto current = local.tm_hour * 60 + local.tm_min;
    const auto start = minutesSinceMidnight(schedule.startTime);
    const auto end = minutesSinceMidnight(schedule.endTime);

    if (start == end) {
        return true;
    }
    if (start < end) {
        return current >= start && current < end;
    }
    return current >= start || current < end;
}

std::chrono::milliseconds hangTimeForReceiver(const RepeaterConfig& config, ReceiverId receiver)
{
    for (const auto& entry : config.receivers) {
        if (entry.receiver == receiver) {
            return entry.hangTime;
        }
    }
    return std::chrono::seconds{5};
}

std::string repeaterName(const RepeaterConfig& config)
{
    if (!config.pluto.callsign.empty()) {
        return config.pluto.callsign;
    }
    if (!config.pluto.watermarkText.empty()) {
        return config.pluto.watermarkText;
    }
    return "WH Repeater";
}

std::string accessMessage(const RepeaterConfig& config)
{
    return repeaterName(config) + "\nhas just been accessed";
}

} // namespace

Daemon::Daemon(RepeaterConfig config, std::filesystem::path configPath)
    : config_{std::move(config)}
    , configPath_{std::move(configPath)}
{
}

int Daemon::run()
{
    installSignalHandlers();

    std::unique_ptr<NimController> nim;
    if (const auto* mode = std::getenv("WH_REPEATER_NIM"); mode != nullptr && std::string_view{mode} == "serit") {
        nim = std::make_unique<SeritNimController>();
    } else {
        nim = std::make_unique<StubNimController>();
    }

    ScanScheduler scanner{config_.receivers};
    SignalArbitrator arbitrator{config_};
    TsRouter router;
    auto media = std::make_unique<MediaPipeline>(config_);
    auto plutoStatus = std::make_unique<PlutoMqttStatusWorker>(config_.pluto);
    auto sd1 = std::make_unique<Sd1StatusWorker>(config_.analogue.sd1);
    HardwarePtt hardwarePtt{config_.hardwarePtt};
    IdentInserter ident{config_.ident};
    ApiServer api;
    std::optional<std::string> accessNotice;
    std::chrono::steady_clock::time_point accessNoticeUntil{};

    nim->initialise();
    api.updateConfig(config_);
    api.start();

    std::cout << "wh-repeater daemon starting with " << config_.receivers.size() << " receivers\n";
    std::cout << "API listening on http://127.0.0.1:8080/api/status\n";

    while (!shutdownRequested.load()) {
        if (auto pendingConfig = api.takePendingConfig(); pendingConfig.has_value()) {
            hardwarePtt.setTransmitEnabled(false);
            sd1.reset();
            for (const auto& receiver : config_.receivers) {
                try {
                    nim->stop(receiver.receiver);
                } catch (const std::exception& ex) {
                    std::cerr << "wh-repeater: stop RX" << receiver.receiver.value
                              << " during config reload failed: " << ex.what() << '\n';
                }
            }
            config_ = std::move(*pendingConfig);
            scanner = ScanScheduler{config_.receivers};
            arbitrator = SignalArbitrator{config_};
            media = std::make_unique<MediaPipeline>(config_);
            plutoStatus = std::make_unique<PlutoMqttStatusWorker>(config_.pluto);
            sd1 = std::make_unique<Sd1StatusWorker>(config_.analogue.sd1);
            hardwarePtt = HardwarePtt{config_.hardwarePtt};
            ident = IdentInserter{config_.ident};
            api.updateConfig(config_);
            if (!configPath_.empty()) {
                saveConfig(configPath_, config_);
            }
            std::cout << "configuration updated via API\n";
        }

        const auto now = std::chrono::steady_clock::now();
        const auto beaconAllowed = beaconScheduleActive(config_.beaconSchedule);
        api.updateBeaconSchedule(beaconAllowed);
        auto statuses = nim->pollStatus();
        scanner.tick(*nim, statuses, now);
        statuses = nim->pollStatus();
        api.updateAnalogueStatus(sd1->snapshot());
        api.updatePlutoStatus(plutoStatus->snapshot());
        auto active = arbitrator.choose(statuses);
        api.updateStatus(statuses, active);

        if (active.has_value()) {
            accessNotice = accessMessage(config_);
            accessNoticeUntil = now + hangTimeForReceiver(config_, active->receiver);
        } else if (accessNotice.has_value() && now >= accessNoticeUntil) {
            accessNotice.reset();
        }

        if (active.has_value()) {
            std::cout << "active RX" << active->receiver.value << " "
                      << active->target.frequencyKhz << " kHz SR"
                      << active->target.symbolRateKs << '\n';
        }

        ident.update(active, now);
        media->select(active);
        media->setBeaconAllowed(beaconAllowed);
        media->setAccessNotice(accessNotice);
        hardwarePtt.setTransmitEnabled(active.has_value() || (config_.fallback.enabled && (beaconAllowed || accessNotice.has_value())));
        router.select(std::move(active));
        router.pump(*nim, *media);
        media->tick(now);

        std::this_thread::sleep_for(config_.statusInterval);
    }

    std::cout << "wh-repeater daemon shutting down\n";
    media.reset();
    hardwarePtt.setTransmitEnabled(false);
    plutoStatus.reset();
    sd1.reset();
    for (const auto& receiver : config_.receivers) {
        try {
            nim->stop(receiver.receiver);
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: stop RX" << receiver.receiver.value << " during shutdown failed: " << ex.what() << '\n';
        }
    }
    nim->shutdown();
    api.stop();
    return 0;
}

} // namespace whrepeater
