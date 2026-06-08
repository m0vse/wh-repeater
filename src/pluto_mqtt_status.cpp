/*
 * ============================================================================
 *  wh-repeater - Pluto MQTT Status Client
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements MQTT connection, subscription, parsing, and snapshot reporting for Pluto/F5OEO transmit status topics.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/pluto_mqtt_status.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <span>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace whrepeater {
namespace {

constexpr auto connectTimeoutMs{500};
constexpr auto reconnectDelay{std::chrono::seconds{3}};

void closeFd(int fd)
{
    if (fd >= 0 && ::close(fd) != 0) {
        std::cerr << "wh-repeater: Pluto MQTT status close failed: " << std::strerror(errno) << '\n';
    }
}

void appendUint16(std::vector<std::byte>& packet, std::uint16_t value)
{
    packet.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    packet.push_back(static_cast<std::byte>(value & 0xff));
}

std::uint16_t readUint16(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<unsigned char>(data[offset]) << 8)
                                      | static_cast<unsigned char>(data[offset + 1]));
}

void appendString(std::vector<std::byte>& packet, std::string_view value)
{
    appendUint16(packet, static_cast<std::uint16_t>(value.size()));
    for (const auto ch : value) {
        packet.push_back(static_cast<std::byte>(ch));
    }
}

void appendRemainingLength(std::vector<std::byte>& packet, std::size_t length)
{
    do {
        auto encoded = static_cast<std::uint8_t>(length % 128);
        length /= 128;
        if (length > 0) {
            encoded |= 0x80;
        }
        packet.push_back(static_cast<std::byte>(encoded));
    } while (length > 0);
}

bool sendAll(int fd, std::span<const std::byte> data)
{
    auto* cursor = data.data();
    auto remaining = data.size();
    while (remaining > 0) {
        const auto sent = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

int connectTcp(std::string_view host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* rawResults{};
    const auto service = std::to_string(port);
    const auto status = ::getaddrinfo(std::string{host}.c_str(), service.c_str(), &hints, &rawResults);
    if (status != 0) {
        return -1;
    }

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results{rawResults, ::freeaddrinfo};
    for (auto* candidate = results.get(); candidate != nullptr; candidate = candidate->ai_next) {
        const auto fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        const auto connectStatus = ::connect(fd, candidate->ai_addr, candidate->ai_addrlen);
        if (connectStatus == 0) {
            if (flags >= 0) {
                (void)::fcntl(fd, F_SETFL, flags);
            }
            return fd;
        }
        if (errno != EINPROGRESS) {
            closeFd(fd);
            continue;
        }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        timeval timeout{.tv_sec = 0, .tv_usec = connectTimeoutMs * 1000};
        const auto ready = ::select(fd + 1, nullptr, &writefds, nullptr, &timeout);
        if (ready > 0) {
            int error{};
            socklen_t errorSize{sizeof(error)};
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorSize) == 0 && error == 0) {
                if (flags >= 0) {
                    (void)::fcntl(fd, F_SETFL, flags);
                }
                return fd;
            }
        }
        closeFd(fd);
    }

    return -1;
}

std::vector<std::byte> mqttConnectPacket()
{
    std::vector<std::byte> variable;
    appendString(variable, "MQTT");
    variable.push_back(std::byte{4});
    variable.push_back(std::byte{2});
    appendUint16(variable, 20);
    appendString(variable, "wh-repeater-status");

    std::vector<std::byte> packet;
    packet.push_back(std::byte{0x10});
    appendRemainingLength(packet, variable.size());
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

std::vector<std::byte> mqttSubscribePacket(std::string_view topic)
{
    std::vector<std::byte> variable;
    appendUint16(variable, 1);
    appendString(variable, topic);
    variable.push_back(std::byte{0});

    std::vector<std::byte> packet;
    packet.push_back(std::byte{0x82});
    appendRemainingLength(packet, variable.size());
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

bool readByte(int fd, std::byte& byte, std::atomic_bool& stopping)
{
    while (!stopping.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval timeout{.tv_sec = 0, .tv_usec = 250'000};
        const auto ready = ::select(fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            continue;
        }
        unsigned char value{};
        const auto count = ::recv(fd, &value, 1, 0);
        if (count != 1) {
            return false;
        }
        byte = static_cast<std::byte>(value);
        return true;
    }
    return false;
}

bool readPacket(int fd, std::uint8_t& type, std::vector<std::byte>& body, std::atomic_bool& stopping)
{
    std::byte header{};
    if (!readByte(fd, header, stopping)) {
        return false;
    }
    type = static_cast<std::uint8_t>(header) >> 4;

    std::size_t multiplier = 1;
    std::size_t remaining = 0;
    for (;;) {
        std::byte encoded{};
        if (!readByte(fd, encoded, stopping)) {
            return false;
        }
        const auto value = static_cast<std::uint8_t>(encoded);
        remaining += (value & 127) * multiplier;
        if ((value & 128) == 0) {
            break;
        }
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            return false;
        }
    }

    body.assign(remaining, std::byte{0});
    auto offset = std::size_t{0};
    while (offset < body.size() && !stopping.load()) {
        const auto count = ::recv(fd, body.data() + offset, body.size() - offset, 0);
        if (count <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return offset == body.size();
}

std::string bytesToString(std::span<const std::byte> data)
{
    std::string text;
    text.reserve(data.size());
    for (const auto byte : data) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

} // namespace

PlutoMqttStatusWorker::PlutoMqttStatusWorker(PlutoConfig config)
    : config_{std::move(config)}
{
    status_.enabled = config_.mqttEnabled;
    status_.host = config_.mqttHost;
    status_.port = config_.mqttPort;
    status_.callsign = config_.callsign;
    if (!config_.mqttEnabled) {
        status_.error = "MQTT disabled";
        return;
    }
    worker_ = std::thread{[this] {
        workerLoop();
    }};
}

PlutoMqttStatusWorker::~PlutoMqttStatusWorker()
{
    stopping_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

PlutoMqttStatus PlutoMqttStatusWorker::snapshot() const
{
    std::lock_guard lock{mutex_};
    return status_;
}

void PlutoMqttStatusWorker::workerLoop()
{
    const auto topic = "dt/pluto/" + config_.callsign + "/#";
    while (!stopping_.load()) {
        const auto fd = connectTcp(config_.mqttHost, config_.mqttPort);
        if (fd < 0) {
            setDisconnected("MQTT broker not reachable");
            std::this_thread::sleep_for(reconnectDelay);
            continue;
        }

        const auto connect = mqttConnectPacket();
        const auto subscribe = mqttSubscribePacket(topic);
        if (!sendAll(fd, connect)) {
            closeFd(fd);
            setDisconnected("MQTT connect packet failed");
            std::this_thread::sleep_for(reconnectDelay);
            continue;
        }

        std::uint8_t type{};
        std::vector<std::byte> body;
        if (!readPacket(fd, type, body, stopping_) || type != 2 || body.size() < 2 || body[1] != std::byte{0}) {
            closeFd(fd);
            setDisconnected("MQTT CONNACK failed");
            std::this_thread::sleep_for(reconnectDelay);
            continue;
        }

        if (!sendAll(fd, subscribe)) {
            closeFd(fd);
            setDisconnected("MQTT subscribe failed");
            std::this_thread::sleep_for(reconnectDelay);
            continue;
        }
        if (!readPacket(fd, type, body, stopping_) || type != 9) {
            closeFd(fd);
            setDisconnected("MQTT SUBACK failed");
            std::this_thread::sleep_for(reconnectDelay);
            continue;
        }

        setConnected(true);
        while (!stopping_.load()) {
            if (!readPacket(fd, type, body, stopping_)) {
                break;
            }
            if (type != 3 || body.size() < 2) {
                continue;
            }
            const auto topicSize = readUint16(body, 0);
            if (body.size() < static_cast<std::size_t>(topicSize) + 2) {
                continue;
            }
            const auto topicText = bytesToString(std::span<const std::byte>{body}.subspan(2, topicSize));
            const auto payload = bytesToString(std::span<const std::byte>{body}.subspan(2 + topicSize));
            recordStatus(topicText, payload);
        }

        closeFd(fd);
        if (!stopping_.load()) {
            setDisconnected("MQTT connection closed");
            std::this_thread::sleep_for(reconnectDelay);
        }
    }
}

void PlutoMqttStatusWorker::setDisconnected(std::string error)
{
    std::lock_guard lock{mutex_};
    status_.connected = false;
    status_.error = std::move(error);
    status_.updatedAt = std::chrono::steady_clock::now();
}

void PlutoMqttStatusWorker::setConnected(bool connected)
{
    std::lock_guard lock{mutex_};
    status_.connected = connected;
    status_.error.reset();
    status_.updatedAt = std::chrono::steady_clock::now();
}

void PlutoMqttStatusWorker::recordStatus(std::string topic, std::string payload)
{
    const auto prefix = "dt/pluto/" + config_.callsign + "/";
    if (topic.rfind(prefix, 0) == 0) {
        topic.erase(0, prefix.size());
    }
    std::lock_guard lock{mutex_};
    status_.connected = true;
    status_.values[std::move(topic)] = std::move(payload);
    status_.error.reset();
    status_.updatedAt = std::chrono::steady_clock::now();
}

} // namespace whrepeater
