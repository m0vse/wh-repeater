#include "whrepeater/api_server.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

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

void ApiServer::updateBeaconSchedule(bool active)
{
    std::lock_guard lock{snapshotMutex_};
    beaconScheduleActive_ = active;
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
    if (method != "GET" && method != "PUT") {
        writeResponse(clientFd, "405 Method Not Allowed", "application/json", "{\"error\":\"method not allowed\"}\n");
        return;
    }

    if (method == "GET" && path == "/api/status") {
        const auto body = statusJson();
        writeResponse(clientFd, "200 OK", "application/json", body);
        return;
    }

    if (method == "GET" && path == "/api/config") {
        const auto body = configJson();
        writeResponse(clientFd, "200 OK", "application/json", body);
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
            }
            writeResponse(clientFd, "202 Accepted", "application/json", "{\"accepted\":true}\n");
        } catch (const std::exception& ex) {
            const auto body = std::string{"{\"error\":"} + jsonString(ex.what()) + "}\n";
            writeResponse(clientFd, "400 Bad Request", "application/json", body);
        }
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
    bool beaconScheduleActive{};
    RepeaterConfig config;
    {
        std::lock_guard lock{snapshotMutex_};
        statuses = statuses_;
        active = active_;
        analogue = analogueStatus_;
        pluto = plutoStatus_;
        beaconScheduleActive = beaconScheduleActive_;
        config = repeaterConfig_;
    }

    std::ostringstream out;
    out << "{";
    if (active.has_value()) {
        out << "\"activeReceiver\":" << active->receiver.value;
    } else {
        out << "\"activeReceiver\":null";
    }
    out << ",\"receivers\":[";

    bool wroteReceiver = false;
    for (std::size_t index = 0; index < statuses.size(); ++index) {
        const auto& status = statuses[index];
        if (wroteReceiver) {
            out << ",";
        }
        wroteReceiver = true;
        out << "{"
            << "\"id\":" << status.receiver.value << ","
            << "\"name\":" << jsonString("RX" + std::to_string(status.receiver.value)) << ","
            << "\"type\":\"nim\","
            << "\"deviceId\":" << jsonString("nim" + std::to_string(status.receiver.value)) << ","
            << "\"nim\":" << jsonString(receiverNimName(status.receiver)) << ","
            << "\"tuner\":" << receiverTunerNumber(status.receiver) << ","
            << "\"antenna\":" << jsonString(receiverAntennaName(status.receiver)) << ","
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

    if (analogue.has_value() && analogue->enabled) {
        if (wroteReceiver) {
            out << ",";
        }
        const auto state = !analogue->present ? "fault" : analogue->locked ? "lockedAnalogue" : analogue->ready ? "idle" : "fault";
        out << "{"
            << "\"id\":" << config.analogue.sd1.receiver.value << ","
            << "\"name\":\"SD1\","
            << "\"type\":\"analogue\","
            << "\"deviceId\":" << jsonString(config.analogue.sd1.deviceId) << ","
            << "\"state\":" << jsonString(state) << ","
            << "\"target\":null,"
            << "\"merDb\":null,"
            << "\"dNumberDb\":null,"
            << "\"serviceName\":" << jsonString("Analogue " + (analogue->activeSource.empty() ? analogue->selectedSource : analogue->activeSource)) << ","
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
    }

    out << "],\"analogue\":";
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
    out << ",\"pluto\":";
    if (pluto.has_value()) {
        out << "{"
            << "\"enabled\":" << (pluto->enabled ? "true" : "false") << ","
            << "\"connected\":" << (pluto->connected ? "true" : "false") << ","
            << "\"host\":" << jsonString(pluto->host) << ","
            << "\"port\":" << pluto->port << ","
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
