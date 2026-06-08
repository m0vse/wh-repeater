/*
 * ============================================================================
 *  wh-repeater - Pluto Output and MQTT Control
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements UDP TS output to PlutoPlus and MQTT control messages for
 *    transmit mute/PTT, modulation, gain, FEC, symbol rate, and related
 *    settings.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/pluto_sink.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <array>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

constexpr std::size_t tsPacketSize{188};
constexpr std::size_t packetsPerDatagram{7};
constexpr std::size_t datagramSize{tsPacketSize * packetsPerDatagram};
constexpr auto mqttTimeoutMs{250};

std::string mqttDeviceId(const PlutoConfig& config)
{
    return config.mqttDeviceId.empty() ? config.callsign : config.mqttDeviceId;
}

std::string tsSourceAddress(const PlutoConfig& config)
{
    return config.address + ":" + std::to_string(config.port);
}

void closeSocket(int fd)
{
    if (fd >= 0 && ::close(fd) != 0) {
        std::cerr << "wh-repeater: Pluto UDP close failed: " << std::strerror(errno) << '\n';
    }
}

void appendUint16(std::vector<std::byte>& packet, std::uint16_t value)
{
    packet.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    packet.push_back(static_cast<std::byte>(value & 0xff));
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

int connectTcp(std::string_view host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* rawResults{};
    const auto service = std::to_string(port);
    const auto status = ::getaddrinfo(std::string{host}.c_str(), service.c_str(), &hints, &rawResults);
    if (status != 0) {
        std::cerr << "wh-repeater: Pluto MQTT lookup failed for " << host << ": " << ::gai_strerror(status) << '\n';
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
            closeSocket(fd);
            continue;
        }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        timeval timeout{.tv_sec = 0, .tv_usec = mqttTimeoutMs * 1000};
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
        closeSocket(fd);
    }

    std::cerr << "wh-repeater: Pluto MQTT connect failed to " << host << ':' << port << '\n';
    return -1;
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

bool publishMqtt(const PlutoConfig& config, std::string_view topic, std::string_view payload)
{
    const auto fd = connectTcp(config.mqttHost, config.mqttPort);
    if (fd < 0) {
        return false;
    }

    std::vector<std::byte> variable;
    appendString(variable, "MQTT");
    variable.push_back(std::byte{4});
    variable.push_back(std::byte{2});
    appendUint16(variable, 10);
    appendString(variable, "wh-repeater");

    std::vector<std::byte> connect;
    connect.push_back(std::byte{0x10});
    appendRemainingLength(connect, variable.size());
    connect.insert(connect.end(), variable.begin(), variable.end());

    std::vector<std::byte> publishVariable;
    appendString(publishVariable, topic);
    for (const auto ch : payload) {
        publishVariable.push_back(static_cast<std::byte>(ch));
    }

    std::vector<std::byte> publish;
    publish.push_back(std::byte{0x30});
    appendRemainingLength(publish, publishVariable.size());
    publish.insert(publish.end(), publishVariable.begin(), publishVariable.end());

    const std::array<std::byte, 2> disconnect{std::byte{0xe0}, std::byte{0x00}};
    const auto ok = sendAll(fd, connect) && sendAll(fd, publish) && sendAll(fd, disconnect);
    if (!ok) {
        std::cerr << "wh-repeater: Pluto MQTT publish failed for " << topic << '\n';
    }
    closeSocket(fd);
    return ok;
}

} // namespace

PlutoSink::PlutoSink(PlutoConfig config)
    : config_{std::move(config)}
{
    datagram_.reserve(datagramSize);
}

PlutoSink::~PlutoSink()
{
    flushDatagram();
    closeSocket(socket_);
}

PlutoSink::PlutoSink(PlutoSink&& other) noexcept
    : config_{std::move(other.config_)}
    , socket_{std::exchange(other.socket_, -1)}
    , datagram_{std::move(other.datagram_)}
{
}

PlutoSink& PlutoSink::operator=(PlutoSink&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    flushDatagram();
    closeSocket(socket_);
    config_ = std::move(other.config_);
    socket_ = std::exchange(other.socket_, -1);
    datagram_ = std::move(other.datagram_);
    return *this;
}

void PlutoSink::write(std::span<const std::byte> packet)
{
    if (packet.size() % tsPacketSize != 0) {
        std::cerr << "wh-repeater: refusing unaligned TS write to Pluto: " << packet.size() << " bytes\n";
        return;
    }
    writeMuxData(packet);
}

void PlutoSink::writeMuxData(std::span<const std::byte> data)
{
    if (data.empty()) {
        return;
    }
    configureTransmitter();
    openSocket();
    for (const auto byte : data) {
        datagram_.push_back(byte);
        if (datagram_.size() == datagramSize) {
            flushDatagram();
        }
    }
}

void PlutoSink::openSocket()
{
    if (socket_ >= 0) {
        return;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        throw std::runtime_error{"create Pluto UDP socket failed: " + std::string{std::strerror(errno)}};
    }
}

void PlutoSink::flushDatagram()
{
    if (datagram_.empty()) {
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error{"invalid Pluto TS address: " + config_.address};
    }

    const auto sent = ::sendto(socket_,
                              datagram_.data(),
                              datagram_.size(),
                              MSG_NOSIGNAL,
                              reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
    if (sent < 0) {
        std::cerr << "wh-repeater: Pluto UDP send failed: " << std::strerror(errno) << '\n';
    }
    datagram_.clear();
}

void PlutoSink::setTransmitEnabled(bool enabled)
{
    if (transmitEnabled_ == enabled) {
        return;
    }
    configureTransmitter();
    publishControl("tx/mute", enabled ? "0" : "1");
    transmitEnabled_ = enabled;
}

void PlutoSink::configureTransmitter()
{
    if (configured_ || !config_.mqttEnabled) {
        return;
    }

    const auto deviceId = mqttDeviceId(config_);
    if (deviceId.empty()) {
        std::cerr << "wh-repeater: Pluto MQTT enabled but device id is empty\n";
        configured_ = true;
        return;
    }

    if (config_.mqttProtocol != "tezuka") {
        publishMqtt(config_, "cmd/pluto/call", config_.callsign);
    }
    publishControl("tx/gain", std::to_string(config_.txGainDb));
    publishControl("tx/mute", "1");
    publishControl("tx/frequency", std::to_string(config_.txFrequencyHz));
    publishControl("tx/nco", std::to_string(config_.ncoHz));
    publishControl("tx/dvbs2/sr", std::to_string(config_.symbolRateS));
    publishControl("tx/dvbs2/tssourcemode", "0");
    publishControl("tx/dvbs2/tssourceaddress", tsSourceAddress(config_));
    if (config_.system == "dvbs") {
        publishControl("tx/stream/mode", "dvbs");
        publishControl("tx/dvbs2/fec", config_.fec);
    } else {
        publishControl("tx/stream/mode", "dvbs2-ts");
        publishControl("tx/dvbs2/fec", config_.fec);
        publishControl("tx/dvbs2/constel", config_.constellation);
        publishControl("tx/dvbs2/pilots", config_.pilots ? "1" : "0");
        publishControl("tx/dvbs2/frame", config_.frame);
        publishControl("tx/dvbs2/fecmode", config_.fecMode);
        publishControl("tx/dvbs2/gainvariable", "-100");
        publishControl("tx/dvbs2/agcgain", "-100");
    }
    configured_ = true;
}

void PlutoSink::publishControl(std::string_view suffix, std::string_view payload)
{
    const auto deviceId = mqttDeviceId(config_);
    if (!config_.mqttEnabled || deviceId.empty()) {
        return;
    }
    const auto topic = "cmd/pluto/" + deviceId + "/" + std::string{suffix};
    (void)publishMqtt(config_, topic, payload);
}

} // namespace whrepeater
