/*
 * ============================================================================
 *  wh-repeater - Transport Stream Gateway Sink
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements raw 188-byte MPEG-TS packet forwarding over UDP, grouped into
 *    1316-byte datagrams for an external PC-side gateway/transcode target.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/ts_gateway_sink.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

constexpr std::size_t tsPacketSize{188};
constexpr std::size_t packetsPerDatagram{7};
constexpr std::size_t datagramSize{tsPacketSize * packetsPerDatagram};

void closeSocket(int fd)
{
    if (fd >= 0 && ::close(fd) != 0) {
        std::cerr << "wh-repeater: TS gateway UDP close failed: " << std::strerror(errno) << '\n';
    }
}

} // namespace

TsGatewaySink::TsGatewaySink(TsGatewayConfig config)
    : config_{std::move(config)}
{
    datagram_.reserve(datagramSize);
    status_.enabled = true;
    status_.address = config_.address;
    status_.port = config_.port;
    status_.updatedAt = std::chrono::steady_clock::now();
}

TsGatewaySink::~TsGatewaySink()
{
    try {
        flush();
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: TS gateway flush failed: " << ex.what() << '\n';
    }
    closeSocket(socket_);
}

void TsGatewaySink::setActive(std::optional<ActiveInput> active)
{
    status_.activeReceiver = active.has_value() ? std::optional<ReceiverId>{active->receiver} : std::nullopt;
    status_.updatedAt = std::chrono::steady_clock::now();
    if (!active.has_value()) {
        flush();
    }
}

void TsGatewaySink::write(std::span<const std::byte> packet)
{
    if (packet.empty()) {
        return;
    }
    if (packet.size() % tsPacketSize != 0) {
        ++status_.sendErrors;
        status_.lastError = "unaligned TS write: " + std::to_string(packet.size()) + " bytes";
        status_.updatedAt = std::chrono::steady_clock::now();
        std::cerr << "wh-repeater: refusing unaligned TS gateway write: " << packet.size() << " bytes\n";
        return;
    }

    openSocket();
    for (const auto byte : packet) {
        datagram_.push_back(byte);
        if (datagram_.size() == datagramSize) {
            flushDatagram();
        }
    }
}

void TsGatewaySink::flush()
{
    flushDatagram();
}

TsGatewayStatus TsGatewaySink::status() const
{
    return status_;
}

void TsGatewaySink::openSocket()
{
    if (socket_ >= 0) {
        return;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        ++status_.sendErrors;
        status_.lastError = "create UDP socket: " + std::string{std::strerror(errno)};
        status_.updatedAt = std::chrono::steady_clock::now();
        throw std::runtime_error{"create TS gateway UDP socket failed: " + std::string{std::strerror(errno)}};
    }
}

void TsGatewaySink::flushDatagram()
{
    if (datagram_.empty()) {
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.address.c_str(), &addr.sin_addr) != 1) {
        ++status_.sendErrors;
        status_.lastError = "invalid destination address: " + config_.address;
        status_.updatedAt = std::chrono::steady_clock::now();
        datagram_.clear();
        return;
    }

    const auto sent = ::sendto(socket_,
                              datagram_.data(),
                              datagram_.size(),
                              MSG_NOSIGNAL,
                              reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
    if (sent < 0) {
        ++status_.sendErrors;
        status_.lastError = "UDP send: " + std::string{std::strerror(errno)};
        std::cerr << "wh-repeater: TS gateway UDP send failed: " << std::strerror(errno) << '\n';
    } else {
        const auto bytes = static_cast<std::uint64_t>(sent);
        status_.bytes += bytes;
        status_.transportPackets += bytes / tsPacketSize;
        ++status_.datagrams;
    }
    status_.updatedAt = std::chrono::steady_clock::now();
    datagram_.clear();
}

} // namespace whrepeater
