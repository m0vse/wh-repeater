/*
 * ============================================================================
 *  wh-repeater - Pi Gateway Status Client
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Polls the Raspberry Pi gateway localhost-style HTTP API over the network
 *    from the PC gateway daemon.
 * ============================================================================
 */

#include "whrepeater/gateway_status_client.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace whrepeater {
namespace {

constexpr auto ioTimeoutMs = 400;

void closeFd(int& fd)
{
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

bool waitFor(int fd, short events)
{
    pollfd item {};
    item.fd = fd;
    item.events = events;
    const auto result = ::poll(&item, 1, ioTimeoutMs);
    return result > 0 && (item.revents & events) != 0;
}

} // namespace

GatewayStatusClient::GatewayStatusClient(PiStatusConfig config)
    : config_{std::move(config)}
{
}

bool GatewayStatusClient::enabled() const
{
    return config_.enabled;
}

bool GatewayStatusClient::due(std::chrono::steady_clock::time_point now) const
{
    return config_.enabled && now >= nextPoll_;
}

std::optional<std::string> GatewayStatusClient::poll(std::chrono::steady_clock::time_point now)
{
    nextPoll_ = now + config_.pollInterval;
    auto body = fetchStatus();
    if (body.has_value()) {
        lastError_.reset();
    }
    return body;
}

const std::optional<std::string>& GatewayStatusClient::lastError() const
{
    return lastError_;
}

std::optional<std::string> GatewayStatusClient::fetchStatus()
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* rawResults = nullptr;
    const auto port = std::to_string(config_.port);
    const auto lookup = ::getaddrinfo(config_.address.c_str(), port.c_str(), &hints, &rawResults);
    if (lookup != 0) {
        lastError_ = "Pi status lookup failed: " + std::string{::gai_strerror(lookup)};
        return std::nullopt;
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results{rawResults, ::freeaddrinfo};

    int fd = -1;
    std::string connectError;
    for (auto* result = results.get(); result != nullptr; result = result->ai_next) {
        fd = ::socket(result->ai_family, result->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, result->ai_protocol);
        if (fd < 0) {
            connectError = std::strerror(errno);
            continue;
        }
        if (::connect(fd, result->ai_addr, result->ai_addrlen) == 0) {
            break;
        }
        if (errno == EINPROGRESS && waitFor(fd, POLLOUT)) {
            int error = 0;
            socklen_t errorSize = sizeof(error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorSize) == 0 && error == 0) {
                break;
            }
            connectError = error == 0 ? "connect failed" : std::strerror(error);
        } else if (errno == EINPROGRESS) {
            connectError = "connect timed out";
        } else {
            connectError = std::strerror(errno);
        }
        closeFd(fd);
    }
    if (fd < 0) {
        if (connectError.empty()) {
            connectError = "no usable address";
        }
        lastError_ = "Pi status connect failed to " + config_.address + ':' + port + ": " + connectError;
        return std::nullopt;
    }

    const auto request = "GET /api/status HTTP/1.1\r\nHost: " + config_.address + "\r\nConnection: close\r\n\r\n";
    const char* cursor = request.data();
    auto remaining = request.size();
    while (remaining > 0) {
        if (!waitFor(fd, POLLOUT)) {
            lastError_ = "Pi status request timed out";
            closeFd(fd);
            return std::nullopt;
        }
        const auto written = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (written <= 0) {
            lastError_ = "Pi status request failed: " + std::string{std::strerror(errno)};
            closeFd(fd);
            return std::nullopt;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        if (!waitFor(fd, POLLIN)) {
            break;
        }
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            lastError_ = "Pi status read failed: " + std::string{std::strerror(errno)};
            closeFd(fd);
            return std::nullopt;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    closeFd(fd);

    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        lastError_ = "Pi status response missing HTTP headers";
        return std::nullopt;
    }
    if (response.find("HTTP/1.1 200") != 0 && response.find("HTTP/1.0 200") != 0) {
        lastError_ = "Pi status response was not HTTP 200";
        return std::nullopt;
    }
    auto body = response.substr(headerEnd + 4);
    if (body.empty() || body.front() != '{') {
        lastError_ = "Pi status response body is not JSON";
        return std::nullopt;
    }
    return body;
}

} // namespace whrepeater
