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
#include <cstdlib>
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

std::optional<std::string> GatewayStatusClient::fetchConfig()
{
    auto response = request("GET", "/api/config");
    if (!response.has_value()) {
        return std::nullopt;
    }
    if (response->status != 200) {
        lastError_ = "Pi config response was HTTP " + std::to_string(response->status);
        return std::nullopt;
    }
    if (response->body.empty() || response->body.front() != '{') {
        lastError_ = "Pi config response body is not JSON";
        return std::nullopt;
    }
    lastError_.reset();
    return response->body;
}

bool GatewayStatusClient::putConfig(const RepeaterConfig& config)
{
    auto response = request("PUT", "/api/config", configToJson(config), "application/json");
    if (!response.has_value()) {
        return false;
    }
    if (response->status != 200 && response->status != 202) {
        lastError_ = "Pi config update response was HTTP " + std::to_string(response->status);
        return false;
    }
    lastError_.reset();
    return true;
}

const std::optional<std::string>& GatewayStatusClient::lastError() const
{
    return lastError_;
}

std::optional<std::string> GatewayStatusClient::fetchStatus()
{
    auto response = request("GET", "/api/status");
    if (!response.has_value()) {
        return std::nullopt;
    }
    if (response->status != 200) {
        lastError_ = "Pi status response was HTTP " + std::to_string(response->status);
        return std::nullopt;
    }
    if (response->body.empty() || response->body.front() != '{') {
        lastError_ = "Pi status response body is not JSON";
        return std::nullopt;
    }
    return response->body;
}

std::optional<GatewayStatusClient::HttpResponse> GatewayStatusClient::request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& contentType)
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
        lastError_ = "Pi API connect failed to " + config_.address + ':' + port + ": " + connectError;
        return std::nullopt;
    }

    std::ostringstream requestStream;
    requestStream << method << ' ' << path << " HTTP/1.1\r\n"
                  << "Host: " << config_.address << "\r\n"
                  << "Connection: close\r\n";
    if (!body.empty() || method == "PUT" || method == "POST") {
        requestStream << "Content-Length: " << body.size() << "\r\n";
        if (!contentType.empty()) {
            requestStream << "Content-Type: " << contentType << "\r\n";
        }
    }
    requestStream << "\r\n" << body;

    const auto requestText = requestStream.str();
    const char* cursor = requestText.data();
    auto remaining = requestText.size();
    while (remaining > 0) {
        if (!waitFor(fd, POLLOUT)) {
            lastError_ = "Pi API request timed out";
            closeFd(fd);
            return std::nullopt;
        }
        const auto written = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (written <= 0) {
            lastError_ = "Pi API request failed: " + std::string{std::strerror(errno)};
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
            lastError_ = "Pi API read failed: " + std::string{std::strerror(errno)};
            closeFd(fd);
            return std::nullopt;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    closeFd(fd);

    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        lastError_ = "Pi API response missing HTTP headers";
        return std::nullopt;
    }
    const auto statusLineEnd = response.find("\r\n");
    const auto statusLine = response.substr(0, statusLineEnd);
    const auto statusCodeStart = statusLine.find(' ');
    if (statusCodeStart == std::string::npos || statusLine.size() < statusCodeStart + 4) {
        lastError_ = "Pi API response missing HTTP status";
        return std::nullopt;
    }
    char* parseEnd = nullptr;
    const auto status = std::strtol(statusLine.c_str() + statusCodeStart + 1, &parseEnd, 10);
    if (parseEnd == statusLine.c_str() + statusCodeStart + 1 || status < 100 || status > 599) {
        lastError_ = "Pi API response has invalid HTTP status";
        return std::nullopt;
    }
    return HttpResponse{static_cast<int>(status), response.substr(headerEnd + 4)};
}

} // namespace whrepeater
