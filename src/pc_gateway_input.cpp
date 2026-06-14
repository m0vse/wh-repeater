/*
 * ============================================================================
 *  wh-repeater - PC Gateway UDP Input
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Receives 188-byte aligned MPEG-TS UDP datagrams from the Pi gateway and
 *    forwards validated transport packets into the media pipeline.
 * ============================================================================
 */

#include "whrepeater/pc_gateway_input.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace whrepeater {
namespace {

constexpr std::size_t transportPacketBytes = 188;
constexpr std::size_t defaultDatagramBytes = 1316;
constexpr int udpReceiveBufferBytes = 4 * 1024 * 1024;
constexpr ReceiverId pcGatewayReceiver{1};

void closeFd(int& fd)
{
    if (fd >= 0) {
        if (::close(fd) < 0) {
            std::cerr << "wh-repeater: PC gateway UDP close failed: " << std::strerror(errno) << '\n';
        }
        fd = -1;
    }
}

} // namespace

PcGatewayInput::PcGatewayInput(GatewayInputConfig config)
    : config_{std::move(config)}
{
    if (config_.packetSize == 0) {
        config_.packetSize = defaultDatagramBytes;
    }
    buffer_.resize(config_.packetSize);
    status_.enabled = true;
    status_.listenAddress = config_.listenAddress;
    status_.listenPort = config_.listenPort;
    openSocket();
}

PcGatewayInput::~PcGatewayInput()
{
    closeFd(socket_);
}

void PcGatewayInput::pump(TsSink& sink)
{
    for (;;) {
        const auto received = ::recv(socket_, buffer_.data(), buffer_.size(), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            recordError("PC gateway UDP receive failed: " + std::string{std::strerror(errno)});
            return;
        }
        if (received == 0) {
            return;
        }

        const auto size = static_cast<std::size_t>(received);
        status_.datagrams += 1;
        status_.bytes += size;
        status_.updatedAt = std::chrono::steady_clock::now();

        if (size % transportPacketBytes != 0) {
            status_.alignmentErrors += 1;
            status_.lastError = "received UDP datagram is not 188-byte aligned";
            continue;
        }

        bool valid = true;
        for (std::size_t offset = 0; offset < size; offset += transportPacketBytes) {
            if (buffer_[offset] != std::byte{0x47}) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            status_.syncErrors += 1;
            status_.lastError = "received UDP datagram has MPEG-TS sync errors";
            continue;
        }

        for (std::size_t offset = 0; offset < size; offset += transportPacketBytes) {
            sink.write(std::span<const std::byte>{buffer_.data() + offset, transportPacketBytes});
            status_.transportPackets += 1;
        }
    }
}

bool PcGatewayInput::waitForData(std::chrono::milliseconds timeout) const
{
    if (socket_ < 0) {
        return false;
    }
    pollfd item{};
    item.fd = socket_;
    item.events = POLLIN;
    const auto timeoutMs = static_cast<int>(std::clamp<long long>(timeout.count(), 0, 1000));
    const auto result = ::poll(&item, 1, timeoutMs);
    return result > 0 && (item.revents & POLLIN) != 0;
}

bool PcGatewayInput::active(std::chrono::steady_clock::time_point now,
                            std::chrono::milliseconds timeout) const
{
    return status_.transportPackets != 0 && now - status_.updatedAt <= timeout;
}

ReceiverStatus PcGatewayInput::receiverStatus() const
{
    ReceiverStatus status;
    status.receiver = pcGatewayReceiver;
    status.state = status_.transportPackets == 0 ? ReceiverState::searching : ReceiverState::lockedDvbs2;
    status.target = ScanTarget{
        .frequencyKhz = 0,
        .symbolRateKs = 0,
        .localOscillatorKhz = 0,
        .antenna = Antenna::top,
        .system = DvbSystem::unknown,
        .fec = "auto",
        .label = "Pi UDP gateway",
    };
    status.serviceName = "Pi UDP gateway";
    status.modulation = "MPEG-TS over UDP";
    status.transportPackets = status_.transportPackets;
    status.continuityErrors = status_.syncErrors + status_.alignmentErrors;
    status.updatedAt = status_.updatedAt;
    return status;
}

ActiveInput PcGatewayInput::activeInput() const
{
    const auto status = receiverStatus();
    return ActiveInput{pcGatewayReceiver, *status.target, status};
}

PcGatewayInputStatus PcGatewayInput::status() const
{
    return status_;
}

void PcGatewayInput::openSocket()
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* rawResults = nullptr;
    const auto port = std::to_string(config_.listenPort);
    const auto lookup = ::getaddrinfo(config_.listenAddress.c_str(), port.c_str(), &hints, &rawResults);
    if (lookup != 0) {
        throw std::runtime_error{"PC gateway UDP lookup failed for "
            + config_.listenAddress + ':' + port + ": " + ::gai_strerror(lookup)};
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results{rawResults, ::freeaddrinfo};

    for (auto* result = results.get(); result != nullptr; result = result->ai_next) {
        socket_ = ::socket(result->ai_family, result->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, result->ai_protocol);
        if (socket_ < 0) {
            continue;
        }

        int reuse = 1;
        (void)::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int receiveBuffer = udpReceiveBufferBytes;
        if (::setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer)) < 0) {
            std::cerr << "wh-repeater: PC gateway UDP receive buffer request failed: "
                      << std::strerror(errno) << '\n';
        }
        if (::bind(socket_, result->ai_addr, result->ai_addrlen) == 0) {
            return;
        }
        closeFd(socket_);
    }

    throw std::runtime_error{"bind PC gateway UDP listener failed on "
        + config_.listenAddress + ':' + port + ": " + std::strerror(errno)};
}

void PcGatewayInput::recordError(std::string error)
{
    status_.lastError = std::move(error);
    status_.updatedAt = std::chrono::steady_clock::now();
    std::cerr << "wh-repeater: " << *status_.lastError << '\n';
}

} // namespace whrepeater
