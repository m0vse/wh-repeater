/*
 * ============================================================================
 *  wh-repeater - API Server Implementation
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements the localhost HTTP/JSON API for health, status snapshots,
 *    configuration retrieval, and configuration updates from the web UI.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/api_server.hpp"

#include "whrepeater/gateway_status_client.hpp"

#include <arpa/inet.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace whrepeater {
namespace {

void closeFd(int fd)
{
    if (fd >= 0 && ::close(fd) != 0) {
        std::cerr << "wh-repeater: API close failed: " << std::strerror(errno) << '\n';
    }
}

std::string jsonString(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                out += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[(byte >> 4) & 0x0f]);
                out.push_back(hex[byte & 0x0f]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
    return out;
}

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

std::string antennaName(Antenna antenna)
{
    return antenna == Antenna::top ? "top" : "bottom";
}

std::string systemName(DvbSystem system)
{
    switch (system) {
    case DvbSystem::dvbs:
        return "dvbs";
    case DvbSystem::dvbs2:
        return "dvbs2";
    case DvbSystem::unknown:
        return "auto";
    }
    return "auto";
}

std::string optionalDoubleJson(const std::optional<double>& value)
{
    if (!value.has_value()) {
        return "null";
    }
    std::ostringstream out;
    out << *value;
    return out.str();
}

struct CpuSample {
    std::uint64_t idle{};
    std::uint64_t total{};
};

struct CpuTracker {
    std::optional<CpuSample> previous;
};

struct ProcessTracker {
    std::optional<std::uint64_t> previousUsageUsec;
    std::optional<double> previousUptimeSeconds;
};

std::optional<std::uint64_t> meminfoKb(std::string_view key)
{
    std::ifstream input{"/proc/meminfo"};
    std::string name;
    std::uint64_t value{};
    std::string unit;
    while (input >> name >> value >> unit) {
        if (!name.empty() && name.back() == ':') {
            name.pop_back();
        }
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<CpuSample> readCpuSample()
{
    std::ifstream input{"/proc/stat"};
    std::string label;
    std::uint64_t user{};
    std::uint64_t nice{};
    std::uint64_t system{};
    std::uint64_t idle{};
    std::uint64_t iowait{};
    std::uint64_t irq{};
    std::uint64_t softirq{};
    std::uint64_t steal{};
    if (!(input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || label != "cpu") {
        return std::nullopt;
    }
    const auto idleAll = idle + iowait;
    const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
    return CpuSample{.idle = idleAll, .total = total};
}

std::optional<double> cpuUsagePercent(CpuTracker& tracker)
{
    const auto current = readCpuSample();
    if (!current.has_value()) {
        return std::nullopt;
    }
    if (!tracker.previous.has_value()) {
        tracker.previous = current;
        return std::nullopt;
    }
    const auto previous = *tracker.previous;
    tracker.previous = current;
    if (current->total <= previous.total || current->idle < previous.idle) {
        return std::nullopt;
    }
    const auto totalDelta = current->total - previous.total;
    const auto idleDelta = current->idle - previous.idle;
    if (totalDelta == 0 || idleDelta > totalDelta) {
        return std::nullopt;
    }
    return 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
}

std::optional<double> loadAverage1m()
{
    std::ifstream input{"/proc/loadavg"};
    double value{};
    if (input >> value) {
        return value;
    }
    return std::nullopt;
}

std::optional<double> uptimeSeconds()
{
    std::ifstream input{"/proc/uptime"};
    double value{};
    if (input >> value) {
        return value;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> selfCgroupPath()
{
    std::ifstream input{"/proc/self/cgroup"};
    std::string line;
    while (std::getline(input, line)) {
        const auto secondColon = line.find(':', line.find(':') == std::string::npos ? std::string::npos : line.find(':') + 1);
        if (secondColon == std::string::npos) {
            continue;
        }
        auto relative = line.substr(secondColon + 1);
        if (!relative.empty() && relative.front() == '/') {
            relative.erase(relative.begin());
        }
        return std::filesystem::path{"/sys/fs/cgroup"} / relative;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> readUintFile(const std::filesystem::path& path)
{
    std::ifstream input{path};
    std::uint64_t value{};
    if (input >> value) {
        return value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> cgroupStatValue(const std::filesystem::path& path, std::string_view key)
{
    std::ifstream input{path};
    std::string name;
    std::uint64_t value{};
    while (input >> name >> value) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> procSelfStatusValue(std::string_view key)
{
    std::ifstream input{"/proc/self/status"};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields{line};
        std::string name;
        std::uint64_t value{};
        if (!(fields >> name >> value)) {
            continue;
        }
        if (!name.empty() && name.back() == ':') {
            name.pop_back();
        }
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::string processStatsJson(ProcessTracker& tracker)
{
    const auto cgroup = selfCgroupPath();
    std::optional<std::uint64_t> memoryKb;
    std::optional<std::uint64_t> tasks;
    std::optional<std::uint64_t> usageUsec;
    if (cgroup.has_value()) {
        if (const auto memoryBytes = readUintFile(*cgroup / "memory.current"); memoryBytes.has_value()) {
            memoryKb = *memoryBytes / 1024;
        }
        tasks = readUintFile(*cgroup / "pids.current");
        usageUsec = cgroupStatValue(*cgroup / "cpu.stat", "usage_usec");
    }
    if (!memoryKb.has_value()) {
        memoryKb = procSelfStatusValue("VmRSS");
    }
    if (!tasks.has_value()) {
        tasks = procSelfStatusValue("Threads");
    }

    std::optional<double> cpuPercent;
    const auto now = uptimeSeconds();
    if (usageUsec.has_value() && now.has_value()) {
        if (tracker.previousUsageUsec.has_value()
            && tracker.previousUptimeSeconds.has_value()
            && *usageUsec >= *tracker.previousUsageUsec
            && *now > *tracker.previousUptimeSeconds) {
            const auto usedSeconds = static_cast<double>(*usageUsec - *tracker.previousUsageUsec) / 1'000'000.0;
            const auto elapsedSeconds = *now - *tracker.previousUptimeSeconds;
            cpuPercent = 100.0 * usedSeconds / elapsedSeconds;
        }
        tracker.previousUsageUsec = usageUsec;
        tracker.previousUptimeSeconds = now;
    }

    std::ostringstream out;
    out << "{"
        << "\"cpuPercent\":" << optionalDoubleJson(cpuPercent) << ","
        << "\"cpuSeconds\":" << (usageUsec.has_value() ? std::to_string(static_cast<double>(*usageUsec) / 1'000'000.0) : "null") << ","
        << "\"memoryKb\":" << (memoryKb.has_value() ? std::to_string(*memoryKb) : "null") << ","
        << "\"tasks\":" << (tasks.has_value() ? std::to_string(*tasks) : "null")
        << "}";
    return out.str();
}

std::optional<double> firstThermalTemperatureC()
{
    std::error_code ec;
    const std::filesystem::path thermalRoot{"/sys/class/thermal"};
    if (!std::filesystem::is_directory(thermalRoot, ec)) {
        return std::nullopt;
    }
    for (std::filesystem::directory_iterator it{thermalRoot, ec}, end; !ec && it != end; it.increment(ec)) {
        const auto tempPath = it->path() / "temp";
        std::ifstream input{tempPath};
        double milliC{};
        if (input >> milliC && milliC > -100000.0 && milliC < 200000.0) {
            return milliC / 1000.0;
        }
    }
    return std::nullopt;
}

std::string hostname()
{
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) == 0) {
        return buffer.data();
    }
    return {};
}

std::string localSystemStatsJson(std::string_view role, CpuTracker& cpuTracker, ProcessTracker& processTracker)
{
    const auto memTotal = meminfoKb("MemTotal");
    const auto memAvailable = meminfoKb("MemAvailable");
    const auto swapTotal = meminfoKb("SwapTotal");
    const auto swapFree = meminfoKb("SwapFree");
    const auto usedKb = memTotal.has_value() && memAvailable.has_value() && *memTotal >= *memAvailable
        ? std::optional<std::uint64_t>{*memTotal - *memAvailable}
        : std::nullopt;
    const auto swapUsedKb = swapTotal.has_value() && swapFree.has_value() && *swapTotal >= *swapFree
        ? std::optional<std::uint64_t>{*swapTotal - *swapFree}
        : std::nullopt;
    const auto memPercent = memTotal.has_value() && usedKb.has_value() && *memTotal > 0
        ? std::optional<double>{100.0 * static_cast<double>(*usedKb) / static_cast<double>(*memTotal)}
        : std::nullopt;

    std::ostringstream out;
    out << "{"
        << "\"role\":" << jsonString(role) << ","
        << "\"hostname\":" << jsonString(hostname()) << ","
        << "\"cpuPercent\":" << optionalDoubleJson(cpuUsagePercent(cpuTracker)) << ","
        << "\"load1\":" << optionalDoubleJson(loadAverage1m()) << ","
        << "\"memoryTotalKb\":" << (memTotal.has_value() ? std::to_string(*memTotal) : "null") << ","
        << "\"memoryUsedKb\":" << (usedKb.has_value() ? std::to_string(*usedKb) : "null") << ","
        << "\"memoryAvailableKb\":" << (memAvailable.has_value() ? std::to_string(*memAvailable) : "null") << ","
        << "\"memoryUsedPercent\":" << optionalDoubleJson(memPercent) << ","
        << "\"swapTotalKb\":" << (swapTotal.has_value() ? std::to_string(*swapTotal) : "null") << ","
        << "\"swapUsedKb\":" << (swapUsedKb.has_value() ? std::to_string(*swapUsedKb) : "null") << ","
        << "\"process\":" << processStatsJson(processTracker) << ","
        << "\"temperatureC\":" << optionalDoubleJson(firstThermalTemperatureC()) << ","
        << "\"uptimeSeconds\":" << optionalDoubleJson(uptimeSeconds())
        << "}";
    return out.str();
}

CpuTracker& apiCpuTracker()
{
    static CpuTracker tracker;
    return tracker;
}

ProcessTracker& apiProcessTracker()
{
    static ProcessTracker tracker;
    return tracker;
}

std::string optionalTargetJson(const std::optional<ScanTarget>& target)
{
    if (!target.has_value()) {
        return "null";
    }

    std::ostringstream out;
    out << "{"
        << "\"frequencyKhz\":" << target->frequencyKhz << ","
        << "\"symbolRateKs\":" << target->symbolRateKs << ","
        << "\"localOscillatorKhz\":" << target->localOscillatorKhz << ","
        << "\"antenna\":" << jsonString(antennaName(target->antenna)) << ","
        << "\"system\":" << jsonString(systemName(target->system)) << ","
        << "\"fec\":" << jsonString(target->fec) << ","
        << "\"label\":" << jsonString(target->label)
        << "}";
    return out.str();
}

std::string receiverNimName(ReceiverId receiver)
{
    return receiver.value <= 2 ? "A" : "B";
}

int receiverTunerNumber(ReceiverId receiver)
{
    return (receiver.value == 1 || receiver.value == 3) ? 1 : 2;
}

std::string receiverAntennaName(ReceiverId receiver)
{
    return (receiver.value == 1 || receiver.value == 3) ? "top" : "bottom";
}

std::uint64_t updatedMsAgo(std::chrono::steady_clock::time_point updatedAt)
{
    const auto now = std::chrono::steady_clock::now();
    if (updatedAt > now) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - updatedAt).count());
}

std::uint64_t updatedMsAgo(const ReceiverStatus& status)
{
    return updatedMsAgo(status.updatedAt);
}

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

std::string_view trimJson(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<int> jsonIntValue(std::string_view value)
{
    value = trimJson(value);
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed{};
    bool negative{};
    std::size_t index{};
    if (value[index] == '-') {
        negative = true;
        ++index;
    }
    if (index >= value.size() || !std::isdigit(static_cast<unsigned char>(value[index]))) {
        return std::nullopt;
    }
    for (; index < value.size(); ++index) {
        const auto ch = value[index];
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    return negative ? -parsed : parsed;
}

std::optional<std::int64_t> jsonInt64Value(std::string_view value)
{
    value = trimJson(value);
    if (value.empty()) {
        return std::nullopt;
    }
    std::int64_t parsed{};
    bool negative{};
    std::size_t index{};
    if (value[index] == '-') {
        negative = true;
        ++index;
    }
    if (index >= value.size() || !std::isdigit(static_cast<unsigned char>(value[index]))) {
        return std::nullopt;
    }
    for (; index < value.size(); ++index) {
        const auto ch = value[index];
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    return negative ? -parsed : parsed;
}

std::string unquotedJsonString(std::string_view value)
{
    value = trimJson(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return {};
    }
    std::string out;
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        const auto ch = value[index];
        if (ch == '\\' && index + 1 < value.size() - 1) {
            ++index;
            out.push_back(value[index]);
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<std::chrono::milliseconds> parseTimecode(std::string_view text)
{
    text = trimJson(text);
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::string_view> parts;
    std::size_t start{};
    while (start <= text.size()) {
        const auto colon = text.find(':', start);
        parts.push_back(text.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start));
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }

    auto parseWhole = [](std::string_view value) -> std::optional<std::int64_t> {
        if (value.empty()) {
            return std::nullopt;
        }
        std::int64_t parsed{};
        for (const auto ch : value) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
            parsed = parsed * 10 + (ch - '0');
        }
        return parsed;
    };

    std::int64_t milliseconds{};
    auto secondsText = parts.back();
    std::int64_t fractionalMs{};
    if (const auto dot = secondsText.find('.'); dot != std::string_view::npos) {
        auto fraction = secondsText.substr(dot + 1);
        secondsText = secondsText.substr(0, dot);
        int digits{};
        for (const auto ch : fraction) {
            if (!std::isdigit(static_cast<unsigned char>(ch)) || digits >= 3) {
                break;
            }
            fractionalMs = fractionalMs * 10 + (ch - '0');
            ++digits;
        }
        while (digits++ < 3) {
            fractionalMs *= 10;
        }
    }
    const auto seconds = parseWhole(secondsText);
    if (!seconds.has_value()) {
        return std::nullopt;
    }
    milliseconds += *seconds * 1000 + fractionalMs;
    if (parts.size() >= 2) {
        const auto minutes = parseWhole(parts[parts.size() - 2]);
        if (!minutes.has_value()) {
            return std::nullopt;
        }
        milliseconds += *minutes * 60 * 1000;
    }
    if (parts.size() == 3) {
        const auto hours = parseWhole(parts[0]);
        if (!hours.has_value()) {
            return std::nullopt;
        }
        milliseconds += *hours * 60 * 60 * 1000;
    }
    return std::chrono::milliseconds{std::max<std::int64_t>(0, milliseconds)};
}

std::string formatTimecode(std::chrono::milliseconds position)
{
    auto totalMs = std::max<std::int64_t>(0, position.count());
    const auto hours = totalMs / 3'600'000;
    totalMs %= 3'600'000;
    const auto minutes = totalMs / 60'000;
    totalMs %= 60'000;
    const auto seconds = totalMs / 1000;
    const auto millis = totalMs % 1000;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds << '.'
        << std::setw(3) << millis;
    return out.str();
}

bool receiverEnabled(const RepeaterConfig& config, ReceiverId receiver)
{
    for (const auto& configured : config.receivers) {
        if (configured.receiver == receiver) {
            return configured.enabled;
        }
    }
    return true;
}

std::string filterReceiverArrayByConfig(std::string_view receiversJson, const RepeaterConfig& config)
{
    const auto body = trimJson(receiversJson);
    if (body.size() < 2 || body.front() != '[' || body.back() != ']') {
        return std::string{receiversJson};
    }

    std::ostringstream out;
    out << "[";
    bool wrote{};
    auto pos = std::size_t{1};
    while (pos + 1 < body.size()) {
        while (pos < body.size() && (std::isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ',')) {
            ++pos;
        }
        if (pos >= body.size() || body[pos] == ']') {
            break;
        }
        if (body[pos] != '{') {
            return std::string{receiversJson};
        }

        const auto start = pos;
        int depth{};
        bool inString{};
        bool escaped{};
        for (; pos < body.size(); ++pos) {
            const auto ch = body[pos];
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
                    ++pos;
                    break;
                }
            }
        }
        if (depth != 0) {
            return std::string{receiversJson};
        }

        const auto object = body.substr(start, pos - start);
        std::optional<int> id;
        if (const auto idValue = jsonMember(object, "id")) {
            id = jsonIntValue(*idValue);
        }
        if (!id.has_value() || receiverEnabled(config, ReceiverId{*id})) {
            if (wrote) {
                out << ",";
            }
            wrote = true;
            out << object;
        }
    }
    out << "]";
    return out.str();
}

std::string serviceNameForMode(std::string_view mode)
{
    if (mode == "pc-gateway") {
        return "wh-pc-gateway.service";
    }
    if (mode == "ts-gateway") {
        return "wh-pi-gateway.service";
    }
    return "wh-repeater.service";
}

int requestSystemdServiceAction(std::string_view action, std::string_view serviceName)
{
    const auto command = "systemctl --no-block " + std::string{action} + " " + std::string{serviceName} + " >/dev/null 2>&1";
    return std::system(command.c_str());
}

int requestSystemdServiceAction(std::string_view action, const RepeaterConfig& config)
{
    return requestSystemdServiceAction(action, serviceNameForMode(config.mode));
}

bool systemdServiceActive(std::string_view serviceName)
{
    const auto command = "systemctl is-active --quiet " + std::string{serviceName} + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

void writeResponse(int clientFd, std::string_view status, std::string_view contentType, std::string_view body)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;

    const auto text = response.str();
    const char* cursor = text.data();
    auto remaining = text.size();
    while (remaining > 0) {
        const auto written = ::send(clientFd, cursor, remaining, MSG_NOSIGNAL);
        if (written <= 0) {
            return;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

std::size_t contentLength(std::string_view request)
{
    constexpr std::string_view header = "Content-Length:";
    auto pos = request.find(header);
    if (pos == std::string_view::npos) {
        return 0;
    }
    pos += header.size();
    while (pos < request.size() && std::isspace(static_cast<unsigned char>(request[pos]))) {
        ++pos;
    }
    auto end = pos;
    while (end < request.size() && std::isdigit(static_cast<unsigned char>(request[end]))) {
        ++end;
    }
    if (end == pos) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoul(std::string{request.substr(pos, end - pos)}));
}

std::string readHttpRequest(int clientFd)
{
    std::string request;
    std::array<char, 4096> buffer{};
    std::size_t expectedBody = 0;

    for (;;) {
        const auto bytesRead = ::recv(clientFd, buffer.data(), buffer.size(), 0);
        if (bytesRead <= 0) {
            break;
        }
        request.append(buffer.data(), static_cast<std::size_t>(bytesRead));

        const auto bodyStart = request.find("\r\n\r\n");
        if (bodyStart == std::string::npos) {
            continue;
        }

        expectedBody = contentLength(request);
        const auto receivedBody = request.size() - (bodyStart + 4);
        if (receivedBody >= expectedBody) {
            break;
        }
    }

    return request;
}

std::filesystem::path expandUserPath(std::string_view path)
{
    if (path == "~") {
        if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return home;
        }
    }
    if (path.size() > 2 && path[0] == '~' && path[1] == '/') {
        if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path{home} / std::string{path.substr(2)};
        }
    }
    return std::filesystem::path{std::string{path}};
}

bool supportedFallbackVideoFile(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".mp4"
        || extension == ".m4v"
        || extension == ".mov"
        || extension == ".mkv"
        || extension == ".ts"
        || extension == ".m2ts"
        || extension == ".mpeg"
        || extension == ".mpg";
}

int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

std::string urlDecode(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            out.push_back(' ');
        } else if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hexValue(value[index + 1]);
            const auto low = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                index += 2;
            } else {
                out.push_back(value[index]);
            }
        } else {
            out.push_back(value[index]);
        }
    }
    return out;
}

std::optional<std::string> queryParameter(std::string_view path, std::string_view name)
{
    const auto queryStart = path.find('?');
    if (queryStart == std::string_view::npos) {
        return std::nullopt;
    }
    auto cursor = queryStart + 1;
    while (cursor <= path.size()) {
        const auto next = path.find('&', cursor);
        const auto item = path.substr(cursor, next == std::string_view::npos ? std::string_view::npos : next - cursor);
        const auto equals = item.find('=');
        const auto key = equals == std::string_view::npos ? item : item.substr(0, equals);
        if (key == name) {
            return urlDecode(equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1));
        }
        if (next == std::string_view::npos) {
            break;
        }
        cursor = next + 1;
    }
    return std::nullopt;
}

} // namespace

ApiServer::ApiServer(ApiServerConfig config)
    : serverConfig_{std::move(config)}
{
}

ApiServer::~ApiServer()
{
    stop();
}

void ApiServer::start()
{
    if (running_.load()) {
        return;
    }

    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        throw std::system_error{errno, std::generic_category(), "create API socket"};
    }

    const int yes = 1;
    if (::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        closeFd(serverFd_);
        serverFd_ = -1;
        throw std::system_error{errno, std::generic_category(), "configure API socket"};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(serverConfig_.port);
    if (::inet_pton(AF_INET, serverConfig_.bindAddress.c_str(), &addr.sin_addr) != 1) {
        closeFd(serverFd_);
        serverFd_ = -1;
        throw std::runtime_error{"invalid API bind address: " + serverConfig_.bindAddress};
    }

    if (::bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeFd(serverFd_);
        serverFd_ = -1;
        throw std::system_error{errno, std::generic_category(), "bind API socket"};
    }

    if (::listen(serverFd_, 8) != 0) {
        closeFd(serverFd_);
        serverFd_ = -1;
        throw std::system_error{errno, std::generic_category(), "listen API socket"};
    }

    running_.store(true);
    serverThread_ = std::thread{[this] {
        serve();
    }};
}

void ApiServer::stop()
{
    running_.store(false);
    closeFd(serverFd_);
    serverFd_ = -1;

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void ApiServer::updateStatus(std::vector<ReceiverStatus> statuses, std::optional<ActiveInput> active)
{
    std::lock_guard lock{snapshotMutex_};
    statuses_ = std::move(statuses);
    active_ = std::move(active);
}

void ApiServer::updateAnalogueStatus(AnalogueStatus status)
{
    std::lock_guard lock{snapshotMutex_};
    analogueStatus_ = std::move(status);
}

void ApiServer::updatePlutoStatus(PlutoMqttStatus status)
{
    std::lock_guard lock{snapshotMutex_};
    plutoStatus_ = std::move(status);
}

void ApiServer::updateTsGatewayStatus(TsGatewayStatus status)
{
    std::lock_guard lock{snapshotMutex_};
    tsGatewayStatus_ = std::move(status);
}

void ApiServer::updateRemoteGatewayStatus(std::optional<std::string> statusJson)
{
    std::lock_guard lock{snapshotMutex_};
    remoteGatewayStatusJson_ = std::move(statusJson);
}

void ApiServer::updateReceiverTransitions(std::vector<ReceiverTransition> transitions)
{
    std::lock_guard lock{snapshotMutex_};
    receiverTransitions_ = std::move(transitions);
}

void ApiServer::updateBeaconSchedule(bool active)
{
    std::lock_guard lock{snapshotMutex_};
    beaconScheduleActive_ = active;
}

void ApiServer::updatePreviewStatus(bool requested, bool active)
{
    std::lock_guard lock{snapshotMutex_};
    previewRequested_ = requested;
    previewActive_ = active;
}

void ApiServer::updateFallbackVideoStatus(std::optional<std::string> statusJson)
{
    std::lock_guard lock{snapshotMutex_};
    fallbackVideoStatusJson_ = std::move(statusJson);
}

void ApiServer::updateConfig(RepeaterConfig config)
{
    std::lock_guard lock{snapshotMutex_};
    repeaterConfig_ = std::move(config);
}

std::optional<RepeaterConfig> ApiServer::takePendingConfig()
{
    std::lock_guard lock{snapshotMutex_};
    auto config = std::move(pendingConfig_);
    pendingConfig_.reset();
    return config;
}

std::optional<std::string> ApiServer::takePendingFallbackVideo()
{
    std::lock_guard lock{snapshotMutex_};
    auto path = std::move(pendingFallbackVideo_);
    pendingFallbackVideo_.reset();
    return path;
}

bool ApiServer::takePendingFallbackVideoStop()
{
    std::lock_guard lock{snapshotMutex_};
    const auto pending = pendingFallbackVideoStop_;
    pendingFallbackVideoStop_ = false;
    return pending;
}

std::optional<std::chrono::milliseconds> ApiServer::takePendingFallbackVideoSeek()
{
    std::lock_guard lock{snapshotMutex_};
    auto pending = pendingFallbackVideoSeek_;
    pendingFallbackVideoSeek_.reset();
    return pending;
}

std::optional<bool> ApiServer::takePendingPreviewEnabled()
{
    std::lock_guard lock{snapshotMutex_};
    auto pending = pendingPreviewEnabled_;
    pendingPreviewEnabled_.reset();
    return pending;
}

void ApiServer::serve()
{
    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverFd_, &readSet);
        timeval timeout{.tv_sec = 0, .tv_usec = 250'000};
        const auto ready = ::select(serverFd_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR || errno == EBADF) {
                continue;
            }
            std::cerr << "wh-repeater: API select failed: " << std::strerror(errno) << '\n';
            continue;
        }
        if (ready == 0 || !FD_ISSET(serverFd_, &readSet)) {
            continue;
        }

        const auto clientFd = ::accept(serverFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno != EINTR && errno != EBADF) {
                std::cerr << "wh-repeater: API accept failed: " << std::strerror(errno) << '\n';
            }
            continue;
        }

        handleClient(clientFd);
        closeFd(clientFd);
    }
}

void ApiServer::handleClient(int clientFd)
{
    const auto requestText = readHttpRequest(clientFd);
    if (requestText.empty()) {
        return;
    }

    std::string_view request{requestText};
    const auto lineEnd = request.find("\r\n");
    const auto requestLine = request.substr(0, lineEnd);
    const auto firstSpace = requestLine.find(' ');
    const auto secondSpace = firstSpace == std::string_view::npos ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) {
        writeResponse(clientFd, "400 Bad Request", "application/json", "{\"error\":\"bad request\"}\n");
        return;
    }

    const auto method = requestLine.substr(0, firstSpace);
    const auto path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (method != "GET" && method != "PUT" && method != "POST") {
        writeResponse(clientFd, "405 Method Not Allowed", "application/json", "{\"error\":\"method not allowed\"}\n");
        return;
    }

    if (method == "GET" && path == "/api/status") {
        const auto body = statusJson();
        writeResponse(clientFd, "200 OK", "application/json", body);
        return;
    }

    if (method == "GET" && path == "/api/system") {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }
        const auto body = localSystemStatsJson(config.mode, apiCpuTracker(), apiProcessTracker()) + "\n";
        writeResponse(clientFd, "200 OK", "application/json", body);
        return;
    }

    if (method == "GET" && path == "/api/config") {
        const auto body = configJson();
        writeResponse(clientFd, "200 OK", "application/json", body);
        return;
    }

    if (method == "GET" && path == "/api/preview/status") {
        bool requested{};
        bool active{};
        {
            std::lock_guard lock{snapshotMutex_};
            requested = previewRequested_;
            active = previewActive_;
        }
        const auto serviceActive = systemdServiceActive("wh-preview.service");
        active = active || serviceActive;
        std::ostringstream body;
        body << "{\"requested\":" << (requested ? "true" : "false")
             << ",\"active\":" << (active ? "true" : "false")
             << ",\"serviceActive\":" << (serviceActive ? "true" : "false") << "}\n";
        writeResponse(clientFd, "200 OK", "application/json", body.str());
        return;
    }

    if (method == "GET" && path.rfind("/api/fallback/videos", 0) == 0) {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }

        const auto requestedDirectory = queryParameter(path, "directory");
        const auto configuredDirectory = requestedDirectory.has_value() && !requestedDirectory->empty()
            ? *requestedDirectory
            : (config.fallback.videoDirectory.empty() ? std::string{"/home/pi/Videos"} : config.fallback.videoDirectory);
        const auto directory = expandUserPath(configuredDirectory);

        std::vector<std::filesystem::directory_entry> entries;
        std::error_code ec;
        if (std::filesystem::is_directory(directory, ec)) {
            for (std::filesystem::directory_iterator it{directory, ec}, end; !ec && it != end; it.increment(ec)) {
                const auto& entry = *it;
                if (entry.is_regular_file(ec) && supportedFallbackVideoFile(entry.path())) {
                    entries.push_back(entry);
                }
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.path().filename().string() < right.path().filename().string();
        });

        std::ostringstream body;
        body << "{"
             << "\"directory\":" << jsonString(directory.string()) << ","
             << "\"videos\":[";
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index != 0) {
                body << ",";
            }
            body << "{"
                 << "\"name\":" << jsonString(entries[index].path().filename().string()) << ","
                 << "\"path\":" << jsonString(entries[index].path().string())
                 << "}";
        }
        body << "]";
        if (ec) {
            body << ",\"error\":" << jsonString(ec.message());
        }
        body << "}\n";
        writeResponse(clientFd, "200 OK", "application/json", body.str());
        return;
    }

    if (method == "PUT" && path == "/api/config") {
        const auto bodyStart = request.find("\r\n\r\n");
        if (bodyStart == std::string_view::npos) {
            writeResponse(clientFd, "400 Bad Request", "application/json", "{\"error\":\"missing request body\"}\n");
            return;
        }

        try {
            auto config = configFromJson(request.substr(bodyStart + 4));
            {
                std::lock_guard lock{snapshotMutex_};
                pendingConfig_ = config;
                repeaterConfig_ = config;
            }
            writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true}\n");
        } catch (const std::exception& ex) {
            const auto body = std::string{"{\"error\":"} + jsonString(ex.what()) + "}\n";
            writeResponse(clientFd, "400 Bad Request", "application/json", body);
        }
        return;
    }

    if (method == "POST" && path == "/api/fallback/play") {
        std::string videoPath;
        const auto bodyStart = request.find("\r\n\r\n");
        if (bodyStart != std::string_view::npos) {
            const auto body = request.substr(bodyStart + 4);
            const auto key = body.find("\"path\"");
            if (key != std::string_view::npos) {
                const auto colon = body.find(':', key);
                const auto firstQuote = colon == std::string_view::npos ? std::string_view::npos : body.find('"', colon);
                const auto secondQuote = firstQuote == std::string_view::npos ? std::string_view::npos : body.find('"', firstQuote + 1);
                if (firstQuote != std::string_view::npos && secondQuote != std::string_view::npos) {
                    videoPath = std::string{body.substr(firstQuote + 1, secondQuote - firstQuote - 1)};
                }
            }
        }
        {
            std::lock_guard lock{snapshotMutex_};
            pendingFallbackVideo_ = std::move(videoPath);
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/fallback/stop") {
        {
            std::lock_guard lock{snapshotMutex_};
            pendingFallbackVideoStop_ = true;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/fallback/seek") {
        const auto bodyStart = request.find("\r\n\r\n");
        if (bodyStart == std::string_view::npos) {
            writeResponse(clientFd, "400 Bad Request", "application/json", "{\"error\":\"missing request body\"}\n");
            return;
        }

        const auto body = request.substr(bodyStart + 4);
        std::optional<std::chrono::milliseconds> position;
        if (const auto value = jsonMember(body, "positionMs"); value.has_value()) {
            if (const auto parsed = jsonInt64Value(*value); parsed.has_value()) {
                position = std::chrono::milliseconds{std::max<std::int64_t>(0, *parsed)};
            }
        }
        if (!position.has_value()) {
            if (const auto value = jsonMember(body, "timecode"); value.has_value()) {
                position = parseTimecode(unquotedJsonString(*value));
            }
        }
        if (!position.has_value()) {
            writeResponse(clientFd, "400 Bad Request", "application/json", "{\"error\":\"invalid timecode\"}\n");
            return;
        }

        {
            std::lock_guard lock{snapshotMutex_};
            pendingFallbackVideoSeek_ = position;
        }
        const auto response = std::string{"{\"accepted\":true,\"positionMs\":"}
            + std::to_string(position->count())
            + ",\"timecode\":" + jsonString(formatTimecode(*position)) + "}\n";
        writeResponse(clientFd, "202 Accepted", "application/json", response);
        return;
    }

    if (method == "POST" && path == "/api/preview/start") {
        {
            std::lock_guard lock{snapshotMutex_};
            pendingPreviewEnabled_ = true;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"preview\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/preview/stop") {
        {
            std::lock_guard lock{snapshotMutex_};
            pendingPreviewEnabled_ = false;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"preview\":false}\n");
        return;
    }

    if (method == "POST" && path == "/api/preview/service/start") {
        const auto status = requestSystemdServiceAction("start", "wh-preview.service");
        writeResponse(clientFd,
                      status == 0 ? "202 Accepted" : "500 Internal Server Error",
                      "application/json",
                      status == 0 ? "{\"accepted\":true,\"state\":\"start-requested\"}\n"
                                  : "{\"error\":\"preview service start failed\"}\n");
        return;
    }

    if (method == "POST" && path == "/api/preview/service/stop") {
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"state\":\"stop-requested\"}\n");
        (void)requestSystemdServiceAction("stop", "wh-preview.service");
        return;
    }

    if (method == "POST" && path == "/api/service/start") {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }
        const auto status = requestSystemdServiceAction("start", config);
        writeResponse(clientFd,
                      status == 0 ? "202 Accepted" : "500 Internal Server Error",
                      "application/json",
                      status == 0 ? "{\"accepted\":true,\"state\":\"start-requested\"}\n"
                                  : "{\"error\":\"systemctl start failed\"}\n");
        return;
    }

    if (method == "POST" && path == "/api/service/stop") {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"state\":\"stop-requested\"}\n");
        (void)requestSystemdServiceAction("stop", config);
        return;
    }

    if (method == "POST" && path == "/api/service/restart") {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"state\":\"restart-requested\"}\n");
        (void)requestSystemdServiceAction("restart", config);
        return;
    }

    if (method == "POST" && path == "/api/pi/service/restart") {
        RepeaterConfig config;
        {
            std::lock_guard lock{snapshotMutex_};
            config = repeaterConfig_;
        }
        if (!config.piStatus.enabled) {
            writeResponse(clientFd, "409 Conflict", "application/json", "{\"error\":\"Pi status polling is disabled\"}\n");
            return;
        }
        GatewayStatusClient client{config.piStatus};
        if (!client.restartService()) {
            const auto error = client.lastError().value_or("Pi service restart failed");
            writeResponse(clientFd, "502 Bad Gateway", "application/json", "{\"error\":" + jsonString(error) + "}\n");
            return;
        }
        writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true,\"state\":\"pi-restart-requested\"}\n");
        return;
    }

    if (method == "GET" && path == "/api/health") {
        writeResponse(clientFd, "200 OK", "application/json", "{\"ok\":true}\n");
        return;
    }

    writeResponse(clientFd, "404 Not Found", "application/json", "{\"error\":\"not found\"}\n");
}

std::string ApiServer::configJson() const
{
    RepeaterConfig config;
    {
        std::lock_guard lock{snapshotMutex_};
        config = repeaterConfig_;
    }

    return configToJson(config);
}

std::string ApiServer::statusJson() const
{
    std::vector<ReceiverStatus> statuses;
    std::optional<ActiveInput> active;
    std::optional<AnalogueStatus> analogue;
    std::optional<PlutoMqttStatus> pluto;
    std::optional<TsGatewayStatus> tsGateway;
    std::optional<std::string> remoteGatewayStatus;
    std::optional<std::string> fallbackVideoStatus;
    std::vector<ReceiverTransition> transitions;
    bool beaconScheduleActive{};
    bool previewRequested{};
    bool previewActive{};
    RepeaterConfig config;
    {
        std::lock_guard lock{snapshotMutex_};
        statuses = statuses_;
        active = active_;
        analogue = analogueStatus_;
        pluto = plutoStatus_;
        tsGateway = tsGatewayStatus_;
        remoteGatewayStatus = remoteGatewayStatusJson_;
        fallbackVideoStatus = fallbackVideoStatusJson_;
        transitions = receiverTransitions_;
        beaconScheduleActive = beaconScheduleActive_;
        previewRequested = previewRequested_;
        previewActive = previewActive_;
        config = repeaterConfig_;
    }

    const std::string_view remoteStatus = remoteGatewayStatus.has_value()
        ? std::string_view{*remoteGatewayStatus}
        : std::string_view{};
    const auto remoteActive = config.mode == "pc-gateway" && !remoteStatus.empty()
        ? jsonMember(remoteStatus, "activeReceiver")
        : std::optional<std::string_view>{};
    const auto remoteReceivers = config.mode == "pc-gateway" && !remoteStatus.empty()
        ? jsonMember(remoteStatus, "receivers")
        : std::optional<std::string_view>{};
    const auto remoteTransitions = config.mode == "pc-gateway" && !remoteStatus.empty()
        ? jsonMember(remoteStatus, "receiverTransitions")
        : std::optional<std::string_view>{};
    const auto remoteSystem = config.mode == "pc-gateway" && !remoteStatus.empty()
        ? jsonMember(remoteStatus, "systemStats")
        : std::optional<std::string_view>{};
    const auto remoteSystemLocal = remoteSystem.has_value()
        ? jsonMember(*remoteSystem, "local")
        : std::optional<std::string_view>{};
    const auto remoteSystemPi = remoteSystem.has_value()
        ? jsonMember(*remoteSystem, "pi")
        : std::optional<std::string_view>{};
    const auto localSystem = localSystemStatsJson(config.mode, apiCpuTracker(), apiProcessTracker());
    const auto localAnalogueActive = active.has_value()
        && active->receiver == config.analogue.capture.receiver;
    const auto analogueVisible = analogue.has_value() && analogue->enabled;
    std::ostringstream out;
    const auto writeAnalogueReceiver = [&] {
        const auto state = !analogue->present ? "fault" : analogue->locked ? "lockedAnalogue" : analogue->ready ? "idle" : "fault";
        out << "{"
            << "\"id\":" << config.analogue.capture.receiver.value << ","
            << "\"name\":" << jsonString(config.analogue.capture.label.empty() ? "Analogue" : config.analogue.capture.label) << ","
            << "\"type\":\"analogue\","
            << "\"deviceId\":" << jsonString(config.analogue.capture.deviceId) << ","
            << "\"state\":" << jsonString(state) << ","
            << "\"target\":null,"
            << "\"merDb\":null,"
            << "\"dNumberDb\":null,"
            << "\"serviceName\":" << jsonString(config.analogue.capture.label.empty() ? "Analogue" : config.analogue.capture.label) << ","
            << "\"modulation\":\"SD analogue\","
            << "\"transportPackets\":0,"
            << "\"continuityErrors\":0,"
            << "\"updatedMsAgo\":" << updatedMsAgo(analogue->updatedAt) << ","
            << "\"present\":" << (analogue->present ? "true" : "false") << ","
            << "\"detected\":" << (analogue->present ? "true" : "false") << ","
            << "\"ready\":" << (analogue->ready ? "true" : "false") << ","
            << "\"locked\":" << (analogue->locked ? "true" : "false") << ","
            << "\"rawLock\":" << static_cast<int>(analogue->rawLock) << ","
            << "\"cameraRunning\":" << (analogue->cameraRunning ? "true" : "false") << ","
            << "\"source\":" << jsonString(analogue->activeSource.empty() ? analogue->selectedSource : analogue->activeSource) << ","
            << "\"firmwareVersion\":" << jsonString(analogue->firmwareVersion) << ","
            << "\"hardwareId\":" << jsonString(analogue->hardwareId) << ","
            << "\"error\":" << (analogue->error.has_value() ? jsonString(*analogue->error) : "null")
            << "}";
    };

    out << "{";
    out << "\"mode\":" << jsonString(config.mode) << ",";
    if (localAnalogueActive) {
        out << "\"activeReceiver\":" << active->receiver.value;
    } else if (remoteActive.has_value()
        && (!jsonIntValue(*remoteActive).has_value()
            || receiverEnabled(config, ReceiverId{*jsonIntValue(*remoteActive)}))) {
        out << "\"activeReceiver\":" << *remoteActive;
    } else if (active.has_value()) {
        out << "\"activeReceiver\":" << active->receiver.value;
    } else {
        out << "\"activeReceiver\":null";
    }
    out << ",\"receivers\":";

    if (remoteReceivers.has_value()) {
        const auto filteredRemoteReceivers = filterReceiverArrayByConfig(*remoteReceivers, config);
        if (analogueVisible) {
            const auto body = trimJson(filteredRemoteReceivers);
            if (body.size() >= 2 && body.front() == '[' && body.back() == ']') {
                const auto contents = trimJson(body.substr(1, body.size() - 2));
                out << "[";
                if (!contents.empty()) {
                    out << contents << ",";
                }
                writeAnalogueReceiver();
                out << "]";
            } else {
                out << filteredRemoteReceivers;
            }
        } else {
            out << filteredRemoteReceivers;
        }
    } else {
        out << "[";

        bool wroteReceiver = false;
        for (std::size_t index = 0; index < statuses.size(); ++index) {
            const auto& status = statuses[index];
            if (!receiverEnabled(config, status.receiver)) {
                continue;
            }
            if (wroteReceiver) {
                out << ",";
            }
            wroteReceiver = true;
            out << "{"
                << "\"id\":" << status.receiver.value << ","
                << "\"name\":" << jsonString(config.mode == "pc-gateway"
                    ? "Pi UDP gateway"
                    : "RX" + std::to_string(status.receiver.value)) << ","
                << "\"type\":" << jsonString(config.mode == "pc-gateway" ? "gateway" : "nim") << ","
                << "\"deviceId\":" << jsonString(config.mode == "pc-gateway"
                    ? "udp-gateway"
                    : "nim" + std::to_string(status.receiver.value)) << ",";
            if (config.mode == "pc-gateway") {
                out << "\"nim\":null,"
                    << "\"tuner\":null,"
                    << "\"antenna\":null,";
            } else {
                out << "\"nim\":" << jsonString(receiverNimName(status.receiver)) << ","
                    << "\"tuner\":" << receiverTunerNumber(status.receiver) << ","
                    << "\"antenna\":" << jsonString(receiverAntennaName(status.receiver)) << ",";
            }
            out
                << "\"state\":" << jsonString(receiverStateName(status.state)) << ","
                << "\"target\":" << optionalTargetJson(status.target) << ","
                << "\"merDb\":" << optionalDoubleJson(status.merDb) << ","
                << "\"dNumberDb\":" << optionalDoubleJson(status.dNumberDb) << ","
                << "\"serviceName\":" << (status.serviceName.has_value() ? jsonString(*status.serviceName) : "null") << ","
                << "\"modulation\":" << (status.modulation.has_value() ? jsonString(*status.modulation) : "null") << ","
                << "\"transportPackets\":" << status.transportPackets << ","
                << "\"continuityErrors\":" << status.continuityErrors << ","
                << "\"updatedMsAgo\":" << updatedMsAgo(status)
                << "}";
        }

        if (analogueVisible) {
            if (wroteReceiver) {
                out << ",";
            }
            writeAnalogueReceiver();
        }
        out << "]";
    }

    out << ",\"tsGateway\":";
    if (tsGateway.has_value()) {
        out << "{"
            << "\"enabled\":" << (tsGateway->enabled ? "true" : "false") << ","
            << "\"address\":" << jsonString(tsGateway->address) << ","
            << "\"port\":" << tsGateway->port << ","
            << "\"activeReceiver\":";
        if (tsGateway->activeReceiver.has_value()) {
            out << tsGateway->activeReceiver->value;
        } else {
            out << "null";
        }
        out << ",\"transportPackets\":" << tsGateway->transportPackets
            << ",\"datagrams\":" << tsGateway->datagrams
            << ",\"bytes\":" << tsGateway->bytes
            << ",\"sendErrors\":" << tsGateway->sendErrors
            << ",\"updatedMsAgo\":" << updatedMsAgo(tsGateway->updatedAt)
            << ",\"lastError\":" << (tsGateway->lastError.has_value() ? jsonString(*tsGateway->lastError) : "null")
            << "}";
    } else {
        out << "{"
            << "\"enabled\":false,"
            << "\"address\":" << jsonString(config.tsGateway.address) << ","
            << "\"port\":" << config.tsGateway.port << ","
            << "\"activeReceiver\":null,"
            << "\"transportPackets\":0,"
            << "\"datagrams\":0,"
            << "\"bytes\":0,"
            << "\"sendErrors\":0,"
            << "\"updatedMsAgo\":null,"
            << "\"lastError\":null"
            << "}";
    }

    out << ",\"receiverTransitions\":";
    if (remoteTransitions.has_value()) {
        out << *remoteTransitions;
    } else {
        out << "[";
        for (std::size_t index = 0; index < transitions.size(); ++index) {
            if (index != 0) {
                out << ",";
            }
            const auto& transition = transitions[index];
            out << "{"
                << "\"receiver\":";
            if (transition.receiver.has_value()) {
                out << transition.receiver->value;
            } else {
                out << "null";
            }
            out << ",\"from\":" << jsonString(transition.from)
                << ",\"to\":" << jsonString(transition.to)
                << ",\"detail\":" << jsonString(transition.detail)
                << ",\"updatedMsAgo\":" << updatedMsAgo(transition.updatedAt)
                << "}";
        }
        out << "]";
    }

    out << ",\"analogue\":";
    if (analogue.has_value()) {
        out << "{"
            << "\"enabled\":" << (analogue->enabled ? "true" : "false") << ","
            << "\"present\":" << (analogue->present ? "true" : "false") << ","
            << "\"ready\":" << (analogue->ready ? "true" : "false") << ","
            << "\"locked\":" << (analogue->locked ? "true" : "false") << ","
            << "\"cameraRunning\":" << (analogue->cameraRunning ? "true" : "false") << ","
            << "\"model\":" << jsonString(analogue->model) << ","
            << "\"selectedSource\":" << jsonString(analogue->selectedSource) << ","
            << "\"activeSource\":" << jsonString(analogue->activeSource) << ","
            << "\"firmwareVersion\":" << jsonString(analogue->firmwareVersion) << ","
            << "\"longVersion\":" << jsonString(analogue->longVersion) << ","
            << "\"hardwareId\":" << jsonString(analogue->hardwareId) << ","
            << "\"rawLock\":" << static_cast<int>(analogue->rawLock) << ","
            << "\"updatedMsAgo\":" << updatedMsAgo(analogue->updatedAt) << ","
            << "\"error\":" << (analogue->error.has_value() ? jsonString(*analogue->error) : "null")
            << "}";
    } else {
        out << "null";
    }
    out << ",\"beaconSchedule\":{"
        << "\"enabled\":" << (config.beaconSchedule.enabled ? "true" : "false") << ","
        << "\"active\":" << (beaconScheduleActive ? "true" : "false") << ","
        << "\"startTime\":" << jsonString(config.beaconSchedule.startTime) << ","
        << "\"endTime\":" << jsonString(config.beaconSchedule.endTime)
        << "}";
    out << ",\"preview\":{"
        << "\"requested\":" << (previewRequested ? "true" : "false") << ","
        << "\"active\":" << (previewActive ? "true" : "false")
        << "}";
    out << ",\"fallbackVideo\":" << (fallbackVideoStatus.has_value() ? *fallbackVideoStatus : "null");
    out << ",\"systemStats\":{";
    if (config.mode == "pc-gateway") {
        out << "\"pc\":" << localSystem << ",\"pi\":";
        if (remoteSystemPi.has_value()) {
            out << *remoteSystemPi;
        } else if (remoteSystemLocal.has_value()) {
            out << *remoteSystemLocal;
        } else if (remoteSystem.has_value()) {
            out << *remoteSystem;
        } else {
            out << "null";
        }
    } else if (config.mode == "ts-gateway") {
        out << "\"pc\":null,\"pi\":" << localSystem;
    } else {
        out << "\"local\":" << localSystem;
    }
    out << "}";
    out << ",\"pluto\":";
    if (pluto.has_value()) {
        out << "{"
            << "\"enabled\":" << (pluto->enabled ? "true" : "false") << ","
            << "\"connected\":" << (pluto->connected ? "true" : "false") << ","
            << "\"host\":" << jsonString(pluto->host) << ","
            << "\"port\":" << pluto->port << ","
            << "\"protocol\":" << jsonString(pluto->protocol) << ","
            << "\"deviceId\":" << jsonString(pluto->deviceId) << ","
            << "\"callsign\":" << jsonString(pluto->callsign) << ","
            << "\"updatedMsAgo\":" << updatedMsAgo(pluto->updatedAt) << ","
            << "\"error\":" << (pluto->error.has_value() ? jsonString(*pluto->error) : "null") << ","
            << "\"values\":{";
        bool firstValue = true;
        for (const auto& [key, value] : pluto->values) {
            if (!firstValue) {
                out << ",";
            }
            firstValue = false;
            out << jsonString(key) << ":" << jsonString(value);
        }
        out << "}}";
    } else {
        out << "null";
    }
    out << "}\n";
    return out.str();
}

} // namespace whrepeater
