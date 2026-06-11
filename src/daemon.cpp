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
#include "whrepeater/media_process.hpp"
#include "whrepeater/nim_controller.hpp"
#include "whrepeater/pluto_mqtt_status.hpp"
#include "whrepeater/scan_scheduler.hpp"
#include "whrepeater/sd1_controller.hpp"
#include "whrepeater/signal_arbitrator.hpp"
#include "whrepeater/ts_router.hpp"

#include <chrono>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <linux/gpio.h>
#include <linux/videodev2.h>
#include <memory>
#include <optional>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

std::atomic_bool shutdownRequested{false};
constexpr bool kSd1SupportEnabled = false;
constexpr auto kAnalogueSyncDebounce = std::chrono::milliseconds{1500};

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
    std::tm utc{};
    gmtime_r(&now, &utc);
    const auto current = utc.tm_hour * 60 + utc.tm_min;
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

std::optional<bool> readGpioInput(std::string_view chip, std::uint32_t line, bool activeHigh, std::string& error)
{
    const int chipFd = ::open(std::string{chip}.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0) {
        error = "open " + std::string{chip} + ": " + std::strerror(errno);
        return std::nullopt;
    }

    gpio_v2_line_request request{};
    request.offsets[0] = line;
    request.num_lines = 1;
    std::snprintf(request.consumer, sizeof(request.consumer), "wh-repeater-analogue-lock");
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT;

    if (::ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        error = "request GPIO line " + std::to_string(line) + ": " + std::strerror(errno);
        ::close(chipFd);
        return std::nullopt;
    }

    gpio_v2_line_values values{};
    values.mask = 1;
    if (::ioctl(request.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        error = "read GPIO line " + std::to_string(line) + ": " + std::strerror(errno);
        ::close(request.fd);
        ::close(chipFd);
        return std::nullopt;
    }

    const bool physicalHigh = (values.bits & 1U) != 0;
    ::close(request.fd);
    ::close(chipFd);
    return activeHigh ? physicalHigh : !physicalHigh;
}

std::optional<bool> readV4l2Sync(std::string_view device, std::string& detail, std::uint32_t& rawStatus)
{
    const int fd = ::open(std::string{device}.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        detail = "open " + std::string{device} + ": " + std::strerror(errno);
        return std::nullopt;
    }

    int inputIndex{};
    if (::ioctl(fd, VIDIOC_G_INPUT, &inputIndex) < 0) {
        detail = "read V4L2 input: " + std::string{std::strerror(errno)};
        ::close(fd);
        return std::nullopt;
    }

    v4l2_input input{};
    input.index = static_cast<std::uint32_t>(inputIndex);
    if (::ioctl(fd, VIDIOC_ENUMINPUT, &input) < 0) {
        detail = "read V4L2 input status: " + std::string{std::strerror(errno)};
        ::close(fd);
        return std::nullopt;
    }

    ::close(fd);
    rawStatus = input.status;

    const auto name = reinterpret_cast<const char*>(input.name);
    const bool noSignal = (input.status & V4L2_IN_ST_NO_SIGNAL) != 0;
    const bool noHSync = (input.status & V4L2_IN_ST_NO_H_LOCK) != 0;
    const bool locked = !noSignal && !noHSync;

    std::ostringstream message;
    message << "input " << input.index << " " << name << " status=0x"
            << std::hex << input.status << std::dec;
    if (noSignal) {
        message << " no-signal";
    }
    if (noHSync) {
        message << " no-hsync";
    }
    detail = message.str();
    return locked;
}

std::optional<ActiveInput> analogueActiveInput(const RepeaterConfig& config, const AnalogueStatus& analogue)
{
    if (!config.analogue.capture.enabled
        || !analogue.enabled
        || !analogue.present
        || !analogue.ready
        || !analogue.locked
        || !std::filesystem::exists(config.analogue.capture.captureDevice)) {
        return std::nullopt;
    }

    const auto sourceLabel = config.analogue.capture.label.empty() ? "Analogue" : config.analogue.capture.label;
    ScanTarget target{
        .frequencyKhz = 0,
        .symbolRateKs = 0,
        .localOscillatorKhz = 0,
        .antenna = Antenna::top,
        .system = DvbSystem::unknown,
        .fec = "auto",
        .label = sourceLabel,
    };
    ReceiverStatus status{
        .receiver = config.analogue.capture.receiver,
        .state = ReceiverState::lockedDvbs,
        .target = target,
        .merDb = std::nullopt,
        .dNumberDb = std::nullopt,
        .serviceName = sourceLabel,
        .modulation = "SD analogue",
        .transportPackets = 0,
        .continuityErrors = 0,
        .updatedAt = analogue.updatedAt,
    };
    return ActiveInput{
        .receiver = config.analogue.capture.receiver,
        .target = std::move(target),
        .status = std::move(status),
    };
}

AnalogueStatus captureAnalogueStatus(const RepeaterConfig& config)
{
    AnalogueStatus status;
    status.enabled = config.analogue.capture.enabled;
    status.updatedAt = std::chrono::steady_clock::now();
    if (!config.analogue.capture.enabled) {
        status.error = "Analogue USB capture disabled";
        return status;
    }

    status.model = "USB capture";
    status.selectedSource = config.analogue.capture.captureDevice;
    status.activeSource = config.analogue.capture.captureDevice;
    status.present = std::filesystem::exists(config.analogue.capture.captureDevice);
    status.ready = status.present;
    status.cameraRunning = status.present;

    if (config.analogue.capture.lockMode == "manual") {
        status.locked = status.present;
    } else if (config.analogue.capture.lockMode == "device-present") {
        status.locked = status.present;
    } else if (config.analogue.capture.lockMode == "v4l2-sync") {
        std::string syncDetail;
        std::uint32_t rawStatus{};
        if (const auto locked = readV4l2Sync(config.analogue.capture.captureDevice, syncDetail, rawStatus)) {
            status.locked = *locked;
            status.rawLock = *locked ? 1 : 0;
            if (!*locked) {
                status.error = "Analogue V4L2 sync unlocked: " + syncDetail;
            }
        } else {
            status.locked = false;
            status.error = "Analogue V4L2 sync read failed: " + syncDetail;
        }
    } else {
        std::string gpioError;
        if (const auto locked = readGpioInput(config.analogue.capture.gpioChip,
                config.analogue.capture.gpioLine,
                config.analogue.capture.gpioActiveHigh,
                gpioError)) {
            status.locked = *locked;
            status.rawLock = *locked ? 1 : 0;
        } else {
            status.locked = false;
            status.error = "Analogue GPIO lock read failed: " + gpioError;
        }
    }

    if (!status.present) {
        status.error = "Analogue capture device not found: " + config.analogue.capture.captureDevice;
    }
    return status;
}

AnalogueStatus parkedAnalogueStatus()
{
    AnalogueStatus status;
    status.enabled = false;
    status.updatedAt = std::chrono::steady_clock::now();
    status.error = "Analogue capture disabled; SD1 support parked pending confirmed CSI output mode";
    return status;
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
    auto media = std::make_unique<MediaProcess>(config_);
    auto plutoStatus = std::make_unique<PlutoMqttStatusWorker>(config_.pluto);
    std::unique_ptr<Sd1StatusWorker> sd1;
    if (kSd1SupportEnabled && config_.analogue.sd1.enabled) {
        sd1 = std::make_unique<Sd1StatusWorker>(config_.analogue.sd1);
    }
    HardwarePtt hardwarePtt{config_.hardwarePtt};
    IdentInserter ident{config_.ident};
    ApiServer api;
    std::optional<std::string> accessNotice;
    std::chrono::steady_clock::time_point accessNoticeUntil{};
    std::optional<std::chrono::steady_clock::time_point> analogueLockedSince;
    std::optional<ActiveInput> heldAnalogueActive;
    std::chrono::steady_clock::time_point heldAnalogueUntil{};

    nim->initialise();
    api.updateConfig(config_);
    api.start();

    std::cout << "wh-repeater daemon starting with " << config_.receivers.size() << " receivers\n";
    std::cout << "API listening on http://127.0.0.1:8080/api/status\n";

    while (!shutdownRequested.load()) {
        if (auto pendingConfig = api.takePendingConfig(); pendingConfig.has_value()) {
            auto nextConfig = std::move(*pendingConfig);
            api.updateConfig(nextConfig);
            if (!configPath_.empty()) {
                saveConfig(configPath_, nextConfig);
            }
            std::cout << "configuration accepted via API\n";

            config_ = std::move(nextConfig);
            scanner = ScanScheduler{config_.receivers};
            arbitrator = SignalArbitrator{config_};
            hardwarePtt = HardwarePtt{config_.hardwarePtt};
            ident = IdentInserter{config_.ident};
            analogueLockedSince.reset();
            heldAnalogueActive.reset();
            api.updateConfig(config_);
            std::cout << "configuration updated via API; media and hardware worker changes apply on service restart\n";
        }

        if (auto fallbackVideo = api.takePendingFallbackVideo(); fallbackVideo.has_value()) {
            auto path = std::move(*fallbackVideo);
            if (path.empty() && !config_.fallback.videoPaths.empty()) {
                path = config_.fallback.videoPaths.front();
            }
            if (path.empty()) {
                std::cerr << "wh-repeater: fallback video play requested but no fallback video path is configured\n";
            } else {
                media->playFallbackVideo(std::move(path));
            }
        }

        if (api.takePendingFallbackVideoStop()) {
            media->stopFallbackVideo();
        }

        const auto now = std::chrono::steady_clock::now();
        const auto beaconAllowed = beaconScheduleActive(config_.beaconSchedule);
        api.updateBeaconSchedule(beaconAllowed);
        auto statuses = nim->pollStatus();
        scanner.tick(*nim, statuses, now);
        statuses = nim->pollStatus();
        const auto analogueStatus = config_.analogue.capture.enabled
            ? captureAnalogueStatus(config_)
            : sd1 ? sd1->snapshot() : parkedAnalogueStatus();
        auto gatedAnalogueStatus = analogueStatus;
        if (gatedAnalogueStatus.locked) {
            if (!analogueLockedSince.has_value()) {
                analogueLockedSince = now;
            }
            if (now - *analogueLockedSince < kAnalogueSyncDebounce) {
                gatedAnalogueStatus.locked = false;
                gatedAnalogueStatus.error = "Analogue sync waiting for stable lock";
            }
        } else {
            analogueLockedSince.reset();
        }
        api.updateAnalogueStatus(gatedAnalogueStatus);
        api.updatePlutoStatus(plutoStatus->snapshot());
        auto active = arbitrator.choose(statuses);
        if (!active.has_value()) {
            if (auto analogueActive = analogueActiveInput(config_, gatedAnalogueStatus); analogueActive.has_value()) {
                active = analogueActive;
                heldAnalogueActive = analogueActive;
                heldAnalogueUntil = now + hangTimeForReceiver(config_, analogueActive->receiver);
            } else if (heldAnalogueActive.has_value() && now < heldAnalogueUntil) {
                active = heldAnalogueActive;
            } else {
                heldAnalogueActive.reset();
            }
        } else if (active->receiver != config_.analogue.capture.receiver
            && active->receiver != config_.analogue.sd1.receiver) {
            heldAnalogueActive.reset();
        }
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
        const auto mediaForcedTransmit = media->mode() == MediaPipelineMode::fallbackVideo;
        hardwarePtt.setTransmitEnabled(mediaForcedTransmit
            || active.has_value()
            || (config_.fallback.enabled && (beaconAllowed || accessNotice.has_value())));
        const auto activeForRouter = active.has_value()
                && active->receiver != config_.analogue.sd1.receiver
                && active->receiver != config_.analogue.capture.receiver
            ? active
            : std::nullopt;
        router.select(activeForRouter);
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
