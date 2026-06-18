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
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
#include "whrepeater/gateway_status_client.hpp"
#include "whrepeater/media_process.hpp"
#include "whrepeater/pc_gateway_input.hpp"
#include "whrepeater/pluto_mqtt_status.hpp"
#endif
#if !defined(WH_REPEATER_PC_GATEWAY_ONLY)
#include "whrepeater/nim_controller.hpp"
#include "whrepeater/scan_scheduler.hpp"
#include "whrepeater/signal_arbitrator.hpp"
#include "whrepeater/ts_gateway_sink.hpp"
#include "whrepeater/ts_router.hpp"
#endif

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <linux/gpio.h>
#include <linux/videodev2.h>
#include <map>
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
constexpr auto kAnalogueSyncDebounce = std::chrono::milliseconds{1500};
constexpr auto kPcAccessNoticeHold = std::chrono::seconds{10};
constexpr auto kPcGatewayLoopInterval = std::chrono::milliseconds{10};
constexpr std::uint16_t kPreviewUdpPort = 15000;

enum class ActiveSourceFamily {
    nimOrGateway,
    analogue,
};

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
class DiscardTsSink final : public TsSink {
public:
    void write(std::span<const std::byte>) override {}
};
#endif

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

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
std::string udpPortToken(std::uint16_t port)
{
    std::ostringstream token;
    token << ':' << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << port;
    return token.str();
}

bool procNetUdpHasLocalPort(const std::filesystem::path& path, std::uint16_t port)
{
    std::ifstream input{path};
    if (!input) {
        return false;
    }

    const auto token = udpPortToken(port);
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        std::istringstream fields{line};
        std::string slot;
        std::string localAddress;
        fields >> slot >> localAddress;
        if (localAddress.size() >= token.size()
            && localAddress.compare(localAddress.size() - token.size(), token.size(), token) == 0) {
            return true;
        }
    }
    return false;
}

bool previewReceiverRunning()
{
    return procNetUdpHasLocalPort("/proc/net/udp", kPreviewUdpPort)
        || procNetUdpHasLocalPort("/proc/net/udp6", kPreviewUdpPort);
}
#endif

#if !defined(WH_REPEATER_PC_GATEWAY_ONLY)
std::chrono::milliseconds hangTimeForReceiver(const RepeaterConfig& config, ReceiverId receiver)
{
    for (const auto& entry : config.receivers) {
        if (entry.receiver == receiver) {
            return entry.hangTime;
        }
    }
    return std::chrono::seconds{5};
}
#endif

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

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
std::optional<std::string_view> jsonMember(std::string_view object, std::string_view name)
{
    const auto key = "\"" + std::string{name} + "\"";
    const auto keyPos = object.find(key);
    if (keyPos == std::string_view::npos) {
        return std::nullopt;
    }
    auto pos = object.find(':', keyPos + key.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) {
        ++pos;
    }
    if (pos >= object.size()) {
        return std::nullopt;
    }

    const auto start = pos;
    const auto opening = object[pos];
    if (opening == '{' || opening == '[') {
        const auto closing = opening == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; pos < object.size(); ++pos) {
            const auto ch = object[pos];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    inString = false;
                }
                continue;
            }
            if (ch == '"') {
                inString = true;
            } else if (ch == opening) {
                ++depth;
            } else if (ch == closing) {
                --depth;
                if (depth == 0) {
                    return object.substr(start, pos - start + 1);
                }
            }
        }
        return std::nullopt;
    }

    if (opening == '"') {
        bool escaped = false;
        for (++pos; pos < object.size(); ++pos) {
            const auto ch = object[pos];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return object.substr(start, pos - start + 1);
            }
        }
        return std::nullopt;
    }

    while (pos < object.size() && object[pos] != ',' && object[pos] != '}') {
        ++pos;
    }
    auto end = pos;
    while (end > start && std::isspace(static_cast<unsigned char>(object[end - 1]))) {
        --end;
    }
    return object.substr(start, end - start);
}

std::optional<std::string> jsonStringValue(std::string_view value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return std::nullopt;
    }
    std::string output;
    output.reserve(value.size() - 2);
    bool escaped = false;
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        const auto ch = value[index];
        if (escaped) {
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                output.push_back(ch);
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 't':
                output.push_back('\t');
                break;
            default:
                output.push_back(ch);
                break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

std::optional<int> jsonIntValue(std::string_view value)
{
    try {
        return std::stoi(std::string{value});
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string_view> jsonObjectAt(std::string_view text, std::size_t start)
{
    if (start >= text.size() || text[start] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (auto pos = start; pos < text.size(); ++pos) {
        const auto ch = text[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> activeRemoteReceiverObject(std::string_view remoteStatus)
{
    const auto activeValue = jsonMember(remoteStatus, "activeReceiver");
    if (!activeValue.has_value() || *activeValue == "null") {
        return std::nullopt;
    }
    const auto activeId = jsonIntValue(*activeValue);
    const auto receivers = jsonMember(remoteStatus, "receivers");
    if (!activeId.has_value() || !receivers.has_value()) {
        return std::nullopt;
    }

    std::size_t pos = 0;
    while ((pos = receivers->find("\"id\":", pos)) != std::string_view::npos) {
        const auto objectStart = receivers->rfind('{', pos);
        if (objectStart == std::string_view::npos) {
            break;
        }
        const auto object = jsonObjectAt(*receivers, objectStart);
        if (!object.has_value()) {
            break;
        }
        const auto idValue = jsonMember(*object, "id");
        const auto id = idValue.has_value() ? jsonIntValue(*idValue) : std::optional<int>{};
        if (id.has_value() && *id == *activeId) {
            return *object;
        }
        pos += 5;
    }
    return std::nullopt;
}
#endif

std::string compactNoticeLine(std::string text, std::size_t maxLength = 42)
{
    if (text.size() <= maxLength) {
        return text;
    }
    if (maxLength <= 3) {
        text.resize(maxLength);
        return text;
    }
    text.resize(maxLength - 3);
    text += "...";
    return text;
}

std::string compactDuration(std::chrono::steady_clock::duration duration)
{
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (seconds < 0) {
        seconds = 0;
    }
    const auto hours = seconds / 3600;
    seconds %= 3600;
    const auto minutes = seconds / 60;
    seconds %= 60;

    std::ostringstream text;
    if (hours > 0) {
        text << hours << "h " << minutes << "m " << seconds << "s";
    } else if (minutes > 0) {
        text << minutes << "m " << seconds << "s";
    } else {
        text << seconds << "s";
    }
    return text.str();
}

bool isGenericDisplayInput(const ActiveInput& active)
{
    return active.receiver.value == 0
        && active.target.frequencyKhz == 0
        && active.target.symbolRateKs == 0
        && active.target.label == "Received stream"
        && !active.status.serviceName.has_value()
        && !active.status.modulation.has_value()
        && !active.status.merDb.has_value();
}

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
void applyRemoteReceiverDetails(ActiveInput& active, std::string_view remoteStatus)
{
    if (remoteStatus.empty()) {
        return;
    }
    const auto remoteReceiver = activeRemoteReceiverObject(remoteStatus);
    if (!remoteReceiver.has_value()) {
        return;
    }

    if (const auto idValue = jsonMember(*remoteReceiver, "id"); idValue.has_value()) {
        if (const auto id = jsonIntValue(*idValue); id.has_value()) {
            active.receiver = ReceiverId{*id};
            active.status.receiver = active.receiver;
        }
    }
    if (const auto target = jsonMember(*remoteReceiver, "target"); target.has_value() && *target != "null") {
        if (const auto raw = jsonMember(*target, "label"); raw.has_value()) {
            if (const auto value = jsonStringValue(*raw); value.has_value()) {
                active.target.label = *value;
            }
        }
        if (const auto raw = jsonMember(*target, "frequencyKhz"); raw.has_value()) {
            if (const auto value = jsonIntValue(*raw); value.has_value()) {
                active.target.frequencyKhz = static_cast<std::uint32_t>(*value);
            }
        }
        if (const auto raw = jsonMember(*target, "symbolRateKs"); raw.has_value()) {
            if (const auto value = jsonIntValue(*raw); value.has_value()) {
                active.target.symbolRateKs = static_cast<std::uint32_t>(*value);
            }
        }
    }
    if (const auto raw = jsonMember(*remoteReceiver, "serviceName"); raw.has_value()) {
        active.status.serviceName = jsonStringValue(*raw);
    }
    if (const auto raw = jsonMember(*remoteReceiver, "modulation"); raw.has_value()) {
        active.status.modulation = jsonStringValue(*raw);
    }
    if (const auto value = jsonMember(*remoteReceiver, "merDb"); value.has_value() && *value != "null") {
        try {
            active.status.merDb = std::stod(std::string{*value});
        } catch (...) {
        }
    }
}

ActiveInput mediaDisplayInput(ActiveInput active, std::string_view remoteStatus)
{
    applyRemoteReceiverDetails(active, remoteStatus);
    if (active.status.serviceName.value_or("") == "Pi UDP gateway"
        || active.target.label == "Pi UDP gateway") {
        active.receiver = ReceiverId{0};
        active.status.receiver = active.receiver;
        active.target = ScanTarget{
            .frequencyKhz = 0,
            .symbolRateKs = 0,
            .localOscillatorKhz = 0,
            .antenna = Antenna::top,
            .system = DvbSystem::unknown,
            .fec = "auto",
            .label = "Received stream",
        };
        active.status.target = active.target;
        active.status.serviceName.reset();
        active.status.modulation.reset();
        active.status.merDb.reset();
        active.status.dNumberDb.reset();
    }
    return active;
}

bool hasSpecificDisplayInput(const ActiveInput& active)
{
    return !isGenericDisplayInput(active);
}

std::vector<std::string> streamInfoLines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream input{std::string{text}};
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::optional<std::string> serviceFromStreamInfo(std::string_view text)
{
    const auto lines = streamInfoLines(text);
    if (lines.size() < 3) {
        return std::nullopt;
    }
    auto service = lines[2];
    if (const auto separator = service.find(" | "); separator != std::string::npos) {
        service.resize(separator);
    }
    return service.empty() ? std::nullopt : std::optional<std::string>{service};
}

std::optional<std::string> videoFormatFromStreamInfo(std::string_view text)
{
    const auto lines = streamInfoLines(text);
    if (lines.size() < 2) {
        return std::nullopt;
    }
    const auto separator = lines[1].find(" | ");
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    auto video = lines[1].substr(separator + 3);
    return video.empty() ? std::nullopt : std::optional<std::string>{video};
}
#endif

std::string accessMessage(const RepeaterConfig& config,
                          const ActiveInput& active,
                          std::string_view remoteStatus = {},
                          bool postAccess = false,
                          std::optional<std::chrono::steady_clock::duration> activeDuration = std::nullopt,
                          std::optional<std::string> serviceOverride = std::nullopt,
                          std::optional<std::string> videoFormat = std::nullopt)
{
    (void)remoteStatus;
    std::ostringstream text;
    text << repeaterName(config);

    auto receiver = active.receiver.value;
    auto label = active.target.label;
    auto frequencyKhz = active.target.frequencyKhz;
    auto symbolRateKs = active.target.symbolRateKs;
    auto serviceName = serviceOverride.has_value() && !serviceOverride->empty()
        ? serviceOverride
        : active.status.serviceName;
    auto modulation = active.status.modulation;
    auto merDb = active.status.merDb;

    if (postAccess) {
        text << "\nhas just been accessed";
        std::ostringstream sourceLine;
        if (receiver > 0) {
            sourceLine << "RX" << receiver;
            if (!label.empty()) {
                sourceLine << ' ' << label;
            }
        } else if (!label.empty() && !isGenericDisplayInput(active)) {
            sourceLine << label;
        }
        if (activeDuration.has_value()) {
            if (sourceLine.tellp() > 0) {
                sourceLine << " | ";
            }
            sourceLine << "Active " << compactDuration(*activeDuration);
        }
        const auto source = sourceLine.str();
        if (!source.empty()) {
            text << "\n" << compactNoticeLine(source, 44);
        }

        std::ostringstream rfLine;
        if (frequencyKhz != 0 || symbolRateKs != 0) {
            rfLine << frequencyKhz << " kHz / " << symbolRateKs << " kS";
        }
        if (merDb.has_value()) {
            if (rfLine.tellp() > 0) {
                rfLine << " | ";
            }
            rfLine << "MER " << std::fixed << std::setprecision(1) << *merDb << " dB";
        }
        const auto rf = rfLine.str();
        if (!rf.empty()) {
            text << "\n" << compactNoticeLine(rf, 44);
        }

        std::ostringstream serviceLine;
        if (serviceName.has_value() && !serviceName->empty()) {
            serviceLine << "Svc " << *serviceName;
        }
        if (videoFormat.has_value() && !videoFormat->empty()) {
            if (serviceLine.tellp() > 0) {
                serviceLine << " | ";
            }
            serviceLine << *videoFormat;
        }
        const auto service = serviceLine.str();
        if (!service.empty()) {
            text << "\n" << compactNoticeLine(service, 44);
        }
        if (modulation.has_value() && !modulation->empty()) {
            text << "\n" << compactNoticeLine(*modulation, 44);
        }
        return text.str();
    } else if (receiver > 0) {
        text << "\nSignal received on RX" << receiver;
    } else {
        text << "\nSignal received";
    }
    if (!label.empty() && !(postAccess && isGenericDisplayInput(active))) {
        text << "\n" << compactNoticeLine(label, 38);
    }
    if (frequencyKhz != 0 || symbolRateKs != 0) {
        text << "\n" << frequencyKhz << " kHz / " << symbolRateKs << " kS";
    }
    if (serviceName.has_value() && !serviceName->empty()) {
        text << "\nService/callsign: " << compactNoticeLine(*serviceName);
    }
    if (videoFormat.has_value() && !videoFormat->empty()) {
        text << "\nVideo: " << compactNoticeLine(*videoFormat);
    }
    if (modulation.has_value() && !modulation->empty()) {
        text << "\n" << compactNoticeLine(*modulation);
    }
    if (merDb.has_value()) {
        text << "\nMER: " << std::fixed << std::setprecision(1) << *merDb << " dB";
    }
    return text.str();
}

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
RepeaterConfig piManagedProjection(const RepeaterConfig& config)
{
    auto projection = defaultConfig();
    projection.mode = "ts-gateway";
    projection.statusInterval = config.statusInterval;
    projection.minimumMerDb = config.minimumMerDb;
    projection.minimumDNumberDb = config.minimumDNumberDb;
    projection.tsGateway = config.tsGateway;
    projection.receivers = config.receivers;
    return projection;
}

bool piManagedConfigChanged(const RepeaterConfig& before, const RepeaterConfig& after)
{
    return configToJson(piManagedProjection(before)) != configToJson(piManagedProjection(after));
}

RepeaterConfig mergePiManagedConfig(const RepeaterConfig& submitted, RepeaterConfig piConfig)
{
    piConfig.mode = "ts-gateway";
    piConfig.statusInterval = submitted.statusInterval;
    piConfig.minimumMerDb = submitted.minimumMerDb;
    piConfig.minimumDNumberDb = submitted.minimumDNumberDb;
    piConfig.tsGateway = submitted.tsGateway;
    piConfig.receivers = submitted.receivers;
    return piConfig;
}

void setPiHardwarePtt(GatewayStatusClient& client,
                      const HardwarePttConfig& config,
                      bool enabled,
                      std::optional<bool>& lastState)
{
    if (!config.enabled || config.mode != "pi-gpio") {
        lastState.reset();
        return;
    }
    if (lastState.has_value() && *lastState == enabled) {
        return;
    }
    if (!client.enabled()) {
        if (!lastState.has_value()) {
            std::cerr << "wh-repeater: Pi GPIO PTT unavailable: Pi API is disabled\n";
        }
        return;
    }
    if (client.writeGpio(config.chip, config.line, config.activeHigh, enabled)) {
        lastState = enabled;
    } else {
        std::cerr << "wh-repeater: Pi GPIO PTT update failed: "
                  << client.lastError().value_or("unknown error") << '\n';
    }
}
#endif

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

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
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
#endif

#if !defined(WH_REPEATER_PC_GATEWAY_ONLY)
std::string receiverStateName(ReceiverState state)
{
    switch (state) {
    case ReceiverState::idle:
        return "idle";
    case ReceiverState::searching:
        return "searching";
    case ReceiverState::foundHeader:
        return "foundHeader";
    case ReceiverState::lockedDvbs:
        return "lockedDvbs";
    case ReceiverState::lockedDvbs2:
        return "lockedDvbs2";
    case ReceiverState::lost:
        return "lost";
    case ReceiverState::timeout:
        return "timeout";
    case ReceiverState::fault:
        return "fault";
    }
    return "unknown";
}

std::string activeInputDetail(const std::optional<ActiveInput>& active)
{
    if (!active.has_value()) {
        return "none";
    }
    if (!active->status.serviceName.value_or("").empty()) {
        return active->status.serviceName.value();
    }
    if (!active->target.label.empty()) {
        return active->target.label;
    }
    return std::to_string(active->target.frequencyKhz) + " kHz SR" + std::to_string(active->target.symbolRateKs);
}

void appendTransition(std::deque<ReceiverTransition>& transitions,
                      std::optional<ReceiverId> receiver,
                      std::string from,
                      std::string to,
                      std::string detail)
{
    transitions.push_front(ReceiverTransition{
        .receiver = receiver,
        .from = std::move(from),
        .to = std::move(to),
        .detail = std::move(detail),
        .updatedAt = std::chrono::steady_clock::now(),
    });
    while (transitions.size() > 32) {
        transitions.pop_back();
    }
}

std::vector<ReceiverTransition> transitionSnapshot(const std::deque<ReceiverTransition>& transitions)
{
    return {transitions.begin(), transitions.end()};
}
#endif

AnalogueStatus captureAnalogueStatus(
    const RepeaterConfig& config
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    , GatewayStatusClient* piGpioClient = nullptr
#endif
)
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
    } else if (config.analogue.capture.lockMode == "gpio") {
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
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    } else if (config.analogue.capture.lockMode == "pi-gpio") {
        if (piGpioClient == nullptr || !piGpioClient->enabled()) {
            status.locked = false;
            status.error = "Analogue Pi GPIO lock read failed: Pi API is disabled";
        } else if (const auto locked = piGpioClient->readGpio(config.analogue.capture.gpioChip,
                       config.analogue.capture.gpioLine,
                       config.analogue.capture.gpioActiveHigh)) {
            status.locked = *locked;
            status.rawLock = *locked ? 1 : 0;
        } else {
            status.locked = false;
            status.error = "Analogue Pi GPIO lock read failed: "
                + piGpioClient->lastError().value_or("unknown error");
        }
#endif
    } else {
        status.locked = false;
        status.error = "Unsupported analogue lock mode: " + config.analogue.capture.lockMode;
    }

    if (!status.present) {
        status.error = "Analogue capture device not found: " + config.analogue.capture.captureDevice;
    }
    return status;
}

AnalogueStatus disabledAnalogueStatus(std::chrono::steady_clock::time_point now)
{
    AnalogueStatus status;
    status.enabled = false;
    status.updatedAt = now;
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

#if defined(WH_REPEATER_PI_GATEWAY_ONLY)
    if (config_.mode != "ts-gateway") {
        throw std::runtime_error{"wh-pi-gateway requires mode=ts-gateway"};
    }
    return runPiGatewayOrLocal();
#elif defined(WH_REPEATER_PC_GATEWAY_ONLY)
    if (config_.mode != "pc-gateway") {
        throw std::runtime_error{"wh-pc-gateway requires mode=pc-gateway"};
    }
    return runPcGateway();
#else
    if (config_.mode == "pc-gateway") {
        return runPcGateway();
    }

    return runPiGatewayOrLocal();
#endif
}

#if !defined(WH_REPEATER_PC_GATEWAY_ONLY)
int Daemon::runPiGatewayOrLocal()
{
    std::unique_ptr<NimController> nim;
    if (const auto* mode = std::getenv("WH_REPEATER_NIM"); mode != nullptr && std::string_view{mode} == "serit") {
        nim = std::make_unique<SeritNimController>();
    } else {
        nim = std::make_unique<StubNimController>();
    }

    ScanScheduler scanner{config_.receivers};
    SignalArbitrator arbitrator{config_};
    TsRouter router;
    const auto gatewayMode = config_.mode == "ts-gateway";
#if defined(WH_REPEATER_PI_GATEWAY_ONLY)
    if (!gatewayMode) {
        throw std::runtime_error{"wh-pi-gateway requires mode=ts-gateway"};
    }
#else
    std::unique_ptr<MediaProcess> media;
#endif
    std::unique_ptr<TsGatewaySink> gateway;
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    std::unique_ptr<PlutoMqttStatusWorker> plutoStatus;
#endif
    if (gatewayMode) {
        gateway = std::make_unique<TsGatewaySink>(config_.tsGateway);
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    } else {
        media = std::make_unique<MediaProcess>(config_);
        plutoStatus = std::make_unique<PlutoMqttStatusWorker>(config_.pluto);
#endif
    }
    HardwarePtt hardwarePtt{config_.hardwarePtt};
    IdentInserter ident{config_.ident};
    ApiServer api{gatewayMode ? ApiServerConfig{.bindAddress = "0.0.0.0", .port = 8080} : ApiServerConfig{}};
    std::optional<std::string> accessNotice;
    [[maybe_unused]] bool accessNoticeEndTone{};
    std::chrono::steady_clock::time_point accessNoticeUntil{};
    std::optional<std::chrono::steady_clock::time_point> analogueLockedSince;
    std::optional<ActiveInput> heldAnalogueActive;
    std::chrono::steady_clock::time_point heldAnalogueUntil{};
    std::optional<ActiveSourceFamily> activeSourceFamily;
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    bool plutoWasConnected{};
#endif
    std::map<int, ReceiverState> previousReceiverStates;
    std::optional<ReceiverId> previousActiveReceiver;
    std::deque<ReceiverTransition> receiverTransitions;

    nim->initialise();
    api.updateConfig(config_);
    if (gateway) {
        api.updateTsGatewayStatus(gateway->status());
    }
    api.start();

    std::cout << "wh-repeater daemon starting with " << config_.receivers.size() << " receivers\n";
    if (gatewayMode) {
        std::cout << "TS gateway mode forwarding to " << config_.tsGateway.address << ':' << config_.tsGateway.port << '\n';
    }
    std::cout << "API listening on http://" << (gatewayMode ? "0.0.0.0" : "127.0.0.1")
              << ":8080/api/status\n";

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
            activeSourceFamily.reset();
            api.updateConfig(config_);
            std::cout << "configuration updated via API; media and hardware worker changes apply on service restart\n";
        }

        if (auto fallbackVideo = api.takePendingFallbackVideo(); fallbackVideo.has_value()) {
            auto path = std::move(*fallbackVideo);
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
            if (media) {
                if (path.empty() && !config_.fallback.videoPaths.empty()) {
                    path = config_.fallback.videoPaths.front();
                }
                if (path.empty()) {
                    std::cerr << "wh-repeater: fallback video play requested but no fallback video path is configured\n";
                } else {
                    media->playFallbackVideo(std::move(path));
                }
            } else {
                std::cerr << "wh-repeater: fallback video play ignored in TS gateway mode: " << path << '\n';
            }
#else
            std::cerr << "wh-repeater: fallback video play ignored in TS gateway mode: " << path << '\n';
#endif
        }

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        if (api.takePendingFallbackVideoStop() && media) {
            media->stopFallbackVideo();
        }
        if (auto fallbackVideoSeek = api.takePendingFallbackVideoSeek(); fallbackVideoSeek.has_value() && media) {
            media->seekFallbackVideo(*fallbackVideoSeek);
        }
        api.updateFallbackVideoStatus(media ? media->fallbackVideoStatus() : std::nullopt);
#else
        (void)api.takePendingFallbackVideoStop();
        (void)api.takePendingFallbackVideoSeek();
        api.updateFallbackVideoStatus(std::nullopt);
#endif

        const auto now = std::chrono::steady_clock::now();
        const auto beaconAllowed = beaconScheduleActive(config_.beaconSchedule);
        api.updateBeaconSchedule(beaconAllowed);
        auto statuses = nim->pollStatus();
        scanner.tick(*nim, statuses, now);
        statuses = nim->pollStatus();
        const auto analogueStatus = config_.analogue.capture.enabled
            ? captureAnalogueStatus(config_)
            : disabledAnalogueStatus(now);
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
        #if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        if (plutoStatus) {
            const auto plutoSnapshot = plutoStatus->snapshot();
            if (media && plutoSnapshot.connected && !plutoWasConnected) {
                std::cout << "wh-repeater: Pluto MQTT reconnected; reapplying TX configuration\n";
                media->reconfigurePluto(beaconAllowed);
            }
            plutoWasConnected = plutoSnapshot.connected;
            api.updatePlutoStatus(plutoSnapshot);
        }
        #endif
        auto nimActive = arbitrator.choose(statuses);
        std::optional<ActiveInput> active;
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        if (!gatewayMode) {
            auto analogueActive = analogueActiveInput(config_, gatedAnalogueStatus);
            if (activeSourceFamily == ActiveSourceFamily::nimOrGateway) {
                if (nimActive.has_value()) {
                    active = nimActive;
                } else {
                    activeSourceFamily.reset();
                }
            } else if (activeSourceFamily == ActiveSourceFamily::analogue) {
                if (analogueActive.has_value()) {
                    active = analogueActive;
                } else if (heldAnalogueActive.has_value() && now < heldAnalogueUntil) {
                    active = heldAnalogueActive;
                } else {
                    heldAnalogueActive.reset();
                    activeSourceFamily.reset();
                }
            }

            if (!active.has_value() && !activeSourceFamily.has_value()) {
                if (nimActive.has_value()) {
                    active = nimActive;
                    activeSourceFamily = ActiveSourceFamily::nimOrGateway;
                    heldAnalogueActive.reset();
                } else if (analogueActive.has_value()) {
                    active = analogueActive;
                    activeSourceFamily = ActiveSourceFamily::analogue;
                    heldAnalogueActive = analogueActive;
                    heldAnalogueUntil = now + hangTimeForReceiver(config_, analogueActive->receiver);
                }
            } else if (activeSourceFamily == ActiveSourceFamily::analogue && analogueActive.has_value()) {
                heldAnalogueActive = analogueActive;
                heldAnalogueUntil = now + hangTimeForReceiver(config_, analogueActive->receiver);
            }
        } else {
            active = nimActive;
            activeSourceFamily = active.has_value() ? std::optional<ActiveSourceFamily>{ActiveSourceFamily::nimOrGateway} : std::nullopt;
            if (active.has_value()
                && active->receiver != config_.analogue.capture.receiver) {
                heldAnalogueActive.reset();
            }
        }
#else
        active = nimActive;
#endif
        if (!active.has_value()) {
            activeSourceFamily.reset();
        }
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        if (active.has_value()
            && active->receiver != config_.analogue.capture.receiver) {
            heldAnalogueActive.reset();
        }
#endif

        for (const auto& status : statuses) {
            const auto previous = previousReceiverStates.find(status.receiver.value);
            if (previous == previousReceiverStates.end()) {
                previousReceiverStates[status.receiver.value] = status.state;
            } else if (previous->second != status.state) {
                appendTransition(receiverTransitions,
                                 status.receiver,
                                 receiverStateName(previous->second),
                                 receiverStateName(status.state),
                                 status.target.has_value() ? status.target->label : "");
                previous->second = status.state;
            }
        }
        const auto currentActiveReceiver = active.has_value()
            ? std::optional<ReceiverId>{active->receiver}
            : std::nullopt;
        if (currentActiveReceiver != previousActiveReceiver) {
            appendTransition(receiverTransitions,
                             currentActiveReceiver,
                             previousActiveReceiver.has_value()
                                 ? "active RX" + std::to_string(previousActiveReceiver->value)
                                 : "none",
                             currentActiveReceiver.has_value()
                                 ? "active RX" + std::to_string(currentActiveReceiver->value)
                                 : "none",
                             activeInputDetail(active));
            previousActiveReceiver = currentActiveReceiver;
        }

        api.updateStatus(statuses, active);
        api.updateReceiverTransitions(transitionSnapshot(receiverTransitions));

        if (active.has_value()) {
            accessNotice = accessMessage(config_, *active);
            accessNoticeEndTone = false;
            accessNoticeUntil = now + hangTimeForReceiver(config_, active->receiver);
        } else if (accessNotice.has_value() && now >= accessNoticeUntil) {
            accessNotice.reset();
            accessNoticeEndTone = false;
        }

        if (active.has_value()) {
            std::cout << "active RX" << active->receiver.value << " "
                      << active->target.frequencyKhz << " kHz SR"
                      << active->target.symbolRateKs << '\n';
        }

        ident.update(active, now);
        bool mediaForcedTransmit = false;
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        if (media) {
            media->select(active);
            media->setBeaconAllowed(beaconAllowed);
            media->setAccessNotice(accessNotice, accessNoticeEndTone);
            mediaForcedTransmit = media->mode() == MediaPipelineMode::fallbackVideo;
        }
#endif
        hardwarePtt.setTransmitEnabled(!gatewayMode
            && (mediaForcedTransmit
                || active.has_value()
                || (config_.fallback.enabled && (beaconAllowed || accessNotice.has_value()))));
        const auto activeForRouter = active.has_value()
                && active->receiver != config_.analogue.capture.receiver
            ? active
            : std::nullopt;
        router.select(activeForRouter);
        if (gateway) {
            gateway->setActive(activeForRouter);
            router.pump(*nim, *gateway);
            api.updateTsGatewayStatus(gateway->status());
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
        } else if (media) {
            router.pump(*nim, *media);
            media->tick(now);
#endif
        }

        std::this_thread::sleep_for(std::min(config_.statusInterval, kPcGatewayLoopInterval));
    }

    std::cout << "wh-repeater daemon shutting down\n";
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    media.reset();
#endif
    gateway.reset();
    hardwarePtt.setTransmitEnabled(false);
#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
    plutoStatus.reset();
#endif
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
#endif

#if !defined(WH_REPEATER_PI_GATEWAY_ONLY)
int Daemon::runPcGateway()
{
    MediaProcess media{config_};
    PcGatewayInput gatewayInput{config_.gatewayInput};
    GatewayStatusClient gatewayStatus{config_.piStatus};
    PlutoMqttStatusWorker plutoStatus{config_.pluto};
    HardwarePtt hardwarePtt{config_.hardwarePtt};
    IdentInserter ident{config_.ident};
    ApiServer api;
    std::optional<std::string> accessNotice;
    bool accessNoticeEndTone{};
    std::optional<std::string> latestRemoteGatewayStatus;
    std::optional<ActiveInput> pcSessionInput;
    std::optional<std::string> pcSessionServiceName;
    std::optional<std::string> pcSessionVideoFormat;
    std::chrono::steady_clock::time_point pcSessionStartedAt{};
    bool pcWasActive{};
    std::optional<ActiveInput> analogueSessionInput;
    std::optional<std::string> analogueSessionServiceName;
    std::optional<std::string> analogueSessionVideoFormat;
    std::chrono::steady_clock::time_point analogueSessionStartedAt{};
    bool analogueWasActive{};
    bool previewRequestedByApi{};
    std::chrono::steady_clock::time_point accessNoticeUntil{};
    std::chrono::steady_clock::time_point lastGatewayStatusWarning{};
    std::optional<std::chrono::steady_clock::time_point> analogueLockedSince;
    std::optional<ActiveSourceFamily> activeSourceFamily;
    std::optional<bool> piPttState;
    bool plutoWasConnected{};
    DiscardTsSink discardTs;
    auto nextHousekeepingAt = std::chrono::steady_clock::now();

    api.updateConfig(config_);
    api.updateStatus({gatewayInput.receiverStatus()}, std::nullopt);
    api.start();

    std::cout << "wh-repeater PC gateway mode listening on "
              << config_.gatewayInput.listenAddress << ':' << config_.gatewayInput.listenPort << '\n';
    std::cout << "API listening on http://127.0.0.1:8080/api/status\n";

    while (!shutdownRequested.load()) {
        const auto waitNow = std::chrono::steady_clock::now();
        const auto waitTimeout = nextHousekeepingAt > waitNow
            ? std::chrono::duration_cast<std::chrono::milliseconds>(nextHousekeepingAt - waitNow)
            : std::chrono::milliseconds{0};
        (void)gatewayInput.waitForData(waitTimeout);
        if (activeSourceFamily == ActiveSourceFamily::analogue) {
            gatewayInput.pump(discardTs);
        } else {
            gatewayInput.pump(media);
        }

        const auto loopNow = std::chrono::steady_clock::now();
        if (loopNow < nextHousekeepingAt) {
            continue;
        }
        nextHousekeepingAt = loopNow + config_.statusInterval;

        if (auto pendingConfig = api.takePendingConfig(); pendingConfig.has_value()) {
            auto nextConfig = std::move(*pendingConfig);
            const auto shouldPushPiConfig = nextConfig.piStatus.enabled && piManagedConfigChanged(config_, nextConfig);
            if (shouldPushPiConfig) {
                GatewayStatusClient piConfigClient{nextConfig.piStatus};
                std::optional<RepeaterConfig> piConfig;
                if (auto body = piConfigClient.fetchConfig(); body.has_value()) {
                    try {
                        piConfig = configFromJson(*body);
                    } catch (const std::exception& ex) {
                        std::cerr << "wh-repeater: Pi config fetch returned invalid config, using gateway-only projection: "
                                  << ex.what() << '\n';
                    }
                }
                if (!piConfig.has_value()) {
                    piConfig = piManagedProjection(nextConfig);
                }

                auto mergedPiConfig = mergePiManagedConfig(nextConfig, std::move(*piConfig));
                if (piConfigClient.putConfig(mergedPiConfig)) {
                    std::cout << "Pi NIM/gateway configuration updated via "
                              << nextConfig.piStatus.address << ':' << nextConfig.piStatus.port << '\n';
                } else {
                    std::cerr << "wh-repeater: Pi NIM/gateway configuration update failed";
                    if (piConfigClient.lastError().has_value()) {
                        std::cerr << ": " << *piConfigClient.lastError();
                    }
                    std::cerr << '\n';
                }
            }
            api.updateConfig(nextConfig);
            if (!configPath_.empty()) {
                saveConfig(configPath_, nextConfig);
            }
            std::cout << "configuration accepted via API\n";

            config_ = std::move(nextConfig);
            gatewayStatus = GatewayStatusClient{config_.piStatus};
            hardwarePtt = HardwarePtt{config_.hardwarePtt};
            ident = IdentInserter{config_.ident};
            activeSourceFamily.reset();
            piPttState.reset();
            api.updateConfig(config_);
            std::cout << "configuration updated via API; media, gateway input, and Pluto changes apply on service restart\n";
        }

        if (auto fallbackVideo = api.takePendingFallbackVideo(); fallbackVideo.has_value()) {
            auto path = std::move(*fallbackVideo);
            if (path.empty() && !config_.fallback.videoPaths.empty()) {
                path = config_.fallback.videoPaths.front();
            }
            if (path.empty()) {
                std::cerr << "wh-repeater: fallback video play requested but no fallback video path is configured\n";
            } else {
                media.playFallbackVideo(std::move(path));
            }
        }

        if (api.takePendingFallbackVideoStop()) {
            media.stopFallbackVideo();
        }
        if (auto fallbackVideoSeek = api.takePendingFallbackVideoSeek(); fallbackVideoSeek.has_value()) {
            media.seekFallbackVideo(*fallbackVideoSeek);
        }
        api.updateFallbackVideoStatus(media.fallbackVideoStatus());

        if (auto previewEnabled = api.takePendingPreviewEnabled(); previewEnabled.has_value()) {
            previewRequestedByApi = *previewEnabled;
        }
        const auto previewActive = previewRequestedByApi || previewReceiverRunning();
        media.setPreviewEnabled(previewActive);
        api.updatePreviewStatus(previewRequestedByApi, previewActive);

        const auto now = loopNow;
        const auto beaconAllowed = beaconScheduleActive(config_.beaconSchedule);
        api.updateBeaconSchedule(beaconAllowed);
        const auto plutoSnapshot = plutoStatus.snapshot();
        api.updatePlutoStatus(plutoSnapshot);
        const auto analogueStatus = config_.analogue.capture.enabled
            ? captureAnalogueStatus(config_, &gatewayStatus)
            : disabledAnalogueStatus(now);
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
        if (gatewayStatus.due(now)) {
            if (auto remoteStatus = gatewayStatus.poll(now); remoteStatus.has_value()) {
                latestRemoteGatewayStatus = *remoteStatus;
                api.updateRemoteGatewayStatus(std::move(remoteStatus));
            } else if (gatewayStatus.lastError().has_value()) {
                if (lastGatewayStatusWarning == std::chrono::steady_clock::time_point{}
                    || now - lastGatewayStatusWarning >= std::chrono::seconds{10}) {
                    lastGatewayStatusWarning = now;
                    std::cerr << "wh-repeater: Pi gateway status unavailable: "
                              << *gatewayStatus.lastError() << '\n';
                }
            }
        }

        std::optional<ActiveInput> active;
        std::optional<ActiveInput> mediaActive;
        const auto latestMediaStreamInfo = media.streamInfo();
        const auto gatewayActive = gatewayInput.active(now, config_.fallback.inputTimeout)
            ? std::optional<ActiveInput>{gatewayInput.activeInput()}
            : std::nullopt;
        const auto gatewayMediaActive = gatewayActive.has_value()
            ? std::optional<ActiveInput>{mediaDisplayInput(*gatewayActive, latestRemoteGatewayStatus.value_or(""))}
            : std::nullopt;
        const auto analogueActive = analogueActiveInput(config_, gatedAnalogueStatus);
        if (activeSourceFamily == ActiveSourceFamily::nimOrGateway) {
            if (gatewayActive.has_value() && gatewayMediaActive.has_value()) {
                active = gatewayActive;
                mediaActive = gatewayMediaActive;
            } else {
                activeSourceFamily.reset();
            }
        } else if (activeSourceFamily == ActiveSourceFamily::analogue) {
            if (analogueActive.has_value()) {
                active = analogueActive;
                mediaActive = analogueActive;
            } else {
                activeSourceFamily.reset();
            }
        }
        if (!activeSourceFamily.has_value()) {
            if (gatewayActive.has_value() && gatewayMediaActive.has_value()) {
                activeSourceFamily = ActiveSourceFamily::nimOrGateway;
                active = gatewayActive;
                mediaActive = gatewayMediaActive;
            } else if (analogueActive.has_value()) {
                activeSourceFamily = ActiveSourceFamily::analogue;
                active = analogueActive;
                mediaActive = analogueActive;
            }
        }

        if (activeSourceFamily == ActiveSourceFamily::nimOrGateway && mediaActive.has_value()) {
            accessNotice = accessMessage(config_, *mediaActive);
            accessNoticeEndTone = false;
            if (!pcWasActive) {
                pcSessionStartedAt = now;
                pcSessionInput.reset();
                pcSessionServiceName.reset();
                pcSessionVideoFormat.reset();
            }
            if (hasSpecificDisplayInput(*mediaActive)) {
                pcSessionInput = mediaActive;
            } else if (!pcSessionInput.has_value()) {
                pcSessionInput = mediaActive;
            }
            if (mediaActive->status.serviceName.has_value() && !mediaActive->status.serviceName->empty()) {
                pcSessionServiceName = mediaActive->status.serviceName;
            }
            if (latestMediaStreamInfo.has_value()) {
                if (auto service = serviceFromStreamInfo(*latestMediaStreamInfo); service.has_value()) {
                    pcSessionServiceName = std::move(service);
                }
                if (auto video = videoFormatFromStreamInfo(*latestMediaStreamInfo); video.has_value()) {
                    pcSessionVideoFormat = std::move(video);
                }
            }
            pcWasActive = true;
            if (analogueWasActive && analogueSessionInput.has_value()) {
                analogueWasActive = false;
                analogueSessionInput.reset();
                analogueSessionServiceName.reset();
                analogueSessionVideoFormat.reset();
            }
        } else if (activeSourceFamily == ActiveSourceFamily::analogue && mediaActive.has_value()) {
            if (pcWasActive && pcSessionInput.has_value()) {
                accessNotice = accessMessage(config_,
                                             *pcSessionInput,
                                             {},
                                             true,
                                             now - pcSessionStartedAt,
                                             pcSessionServiceName,
                                             pcSessionVideoFormat);
                accessNoticeEndTone = true;
                accessNoticeUntil = now + kPcAccessNoticeHold;
                pcWasActive = false;
            }
            accessNotice = accessMessage(config_, *mediaActive);
            accessNoticeEndTone = false;
            if (!analogueWasActive) {
                analogueSessionStartedAt = now;
                analogueSessionInput = mediaActive;
                analogueSessionServiceName.reset();
                analogueSessionVideoFormat.reset();
            }
            analogueSessionInput = mediaActive;
            if (mediaActive->status.serviceName.has_value() && !mediaActive->status.serviceName->empty()) {
                analogueSessionServiceName = mediaActive->status.serviceName;
            }
            if (latestMediaStreamInfo.has_value()) {
                if (auto service = serviceFromStreamInfo(*latestMediaStreamInfo); service.has_value()) {
                    analogueSessionServiceName = std::move(service);
                }
                if (auto video = videoFormatFromStreamInfo(*latestMediaStreamInfo); video.has_value()) {
                    analogueSessionVideoFormat = std::move(video);
                }
            }
            analogueWasActive = true;
        } else {
            if (pcWasActive && pcSessionInput.has_value()) {
                accessNotice = accessMessage(config_,
                                             *pcSessionInput,
                                             {},
                                             true,
                                             now - pcSessionStartedAt,
                                             pcSessionServiceName,
                                             pcSessionVideoFormat);
                accessNoticeEndTone = true;
                accessNoticeUntil = now + kPcAccessNoticeHold;
                pcWasActive = false;
            } else if (analogueWasActive && analogueSessionInput.has_value()) {
                accessNotice = accessMessage(config_,
                                             *analogueSessionInput,
                                             {},
                                             true,
                                             now - analogueSessionStartedAt,
                                             analogueSessionServiceName,
                                             analogueSessionVideoFormat);
                accessNoticeEndTone = true;
                accessNoticeUntil = now + kPcAccessNoticeHold;
                analogueWasActive = false;
                analogueSessionInput.reset();
                analogueSessionServiceName.reset();
                analogueSessionVideoFormat.reset();
            } else if (accessNotice.has_value() && now >= accessNoticeUntil) {
                accessNotice.reset();
                accessNoticeEndTone = false;
            }
        }

        media.select(mediaActive);
        media.setBeaconAllowed(beaconAllowed);
        media.setAccessNotice(accessNotice, accessNoticeEndTone);
        media.tick(now);

        api.updateStatus({gatewayInput.receiverStatus()}, active);

        const auto mediaForcedTransmit = media.mode() == MediaPipelineMode::fallbackVideo;
        const auto transmitEnabled = mediaForcedTransmit
            || mediaActive.has_value()
            || (config_.fallback.enabled && (beaconAllowed || accessNotice.has_value()));
        if (plutoSnapshot.connected && !plutoWasConnected) {
            std::cout << "wh-repeater: Pluto MQTT reconnected; reapplying TX configuration\n";
            media.reconfigurePluto(transmitEnabled);
        }
        plutoWasConnected = plutoSnapshot.connected;
        hardwarePtt.setTransmitEnabled(transmitEnabled);
        setPiHardwarePtt(gatewayStatus, config_.hardwarePtt, transmitEnabled, piPttState);

    }

    std::cout << "wh-repeater PC gateway daemon shutting down\n";
    hardwarePtt.setTransmitEnabled(false);
    setPiHardwarePtt(gatewayStatus, config_.hardwarePtt, false, piPttState);
    api.stop();
    return 0;
}
#endif

} // namespace whrepeater
