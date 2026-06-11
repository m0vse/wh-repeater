/*
 * ============================================================================
 *  wh-repeater - Media Process Supervisor
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Runs the FFmpeg/V4L2 media pipeline in a forked child process, sends
 *    compact control and TS messages over a Unix socket, and restarts the
 *    media child if it exits or stops accepting messages.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/media_process.hpp"

#include "whrepeater/media_pipeline.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace whrepeater {
namespace {

enum class MediaMessage : std::uint32_t {
    shutdown = 1,
    select = 2,
    beacon = 3,
    notice = 4,
    playFallbackVideo = 5,
    stopFallbackVideo = 6,
    tick = 7,
    transportStream = 8,
};

struct MessageHeader {
    std::uint32_t type{};
    std::uint32_t size{};
};

template <typename T>
void appendPod(std::vector<std::byte>& output, const T& value)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(T));
}

void appendBool(std::vector<std::byte>& output, bool value)
{
    const std::uint8_t byte = value ? 1 : 0;
    appendPod(output, byte);
}

void appendString(std::vector<std::byte>& output, std::string_view value)
{
    const auto size = static_cast<std::uint32_t>(value.size());
    appendPod(output, size);
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    output.insert(output.end(), bytes, bytes + value.size());
}

void appendOptionalString(std::vector<std::byte>& output, const std::optional<std::string>& value)
{
    appendBool(output, value.has_value());
    if (value.has_value()) {
        appendString(output, *value);
    }
}

void appendScanTarget(std::vector<std::byte>& output, const ScanTarget& target)
{
    appendPod(output, target.frequencyKhz);
    appendPod(output, target.symbolRateKs);
    appendPod(output, target.localOscillatorKhz);
    appendPod(output, static_cast<std::uint32_t>(target.antenna));
    appendPod(output, static_cast<std::uint32_t>(target.system));
    appendString(output, target.fec);
    appendString(output, target.label);
}

void appendReceiverStatus(std::vector<std::byte>& output, const ReceiverStatus& status)
{
    appendPod(output, status.receiver.value);
    appendPod(output, static_cast<std::uint32_t>(status.state));
    appendBool(output, status.target.has_value());
    if (status.target.has_value()) {
        appendScanTarget(output, *status.target);
    }
    appendBool(output, status.merDb.has_value());
    if (status.merDb.has_value()) {
        appendPod(output, *status.merDb);
    }
    appendBool(output, status.dNumberDb.has_value());
    if (status.dNumberDb.has_value()) {
        appendPod(output, *status.dNumberDb);
    }
    appendOptionalString(output, status.serviceName);
    appendOptionalString(output, status.modulation);
    appendPod(output, status.transportPackets);
    appendPod(output, status.continuityErrors);
}

std::vector<std::byte> activePayload(const std::optional<ActiveInput>& input)
{
    std::vector<std::byte> payload;
    appendBool(payload, input.has_value());
    if (!input.has_value()) {
        return payload;
    }
    appendPod(payload, input->receiver.value);
    appendScanTarget(payload, input->target);
    appendReceiverStatus(payload, input->status);
    return payload;
}

std::vector<std::byte> boolPayload(bool value)
{
    std::vector<std::byte> payload;
    appendBool(payload, value);
    return payload;
}

std::vector<std::byte> stringPayload(std::string_view value)
{
    std::vector<std::byte> payload;
    appendString(payload, value);
    return payload;
}

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> payload)
        : payload_{payload}
    {
    }

    template <typename T>
    T readPod()
    {
        if (offset_ + sizeof(T) > payload_.size()) {
            throw std::runtime_error{"short media-process message"};
        }
        T value{};
        std::memcpy(&value, payload_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    bool readBool()
    {
        return readPod<std::uint8_t>() != 0;
    }

    std::string readString()
    {
        const auto size = readPod<std::uint32_t>();
        if (offset_ + size > payload_.size()) {
            throw std::runtime_error{"short media-process string"};
        }
        std::string value{reinterpret_cast<const char*>(payload_.data() + offset_), size};
        offset_ += size;
        return value;
    }

    std::optional<std::string> readOptionalString()
    {
        if (!readBool()) {
            return std::nullopt;
        }
        return readString();
    }

private:
    std::span<const std::byte> payload_;
    std::size_t offset_{};
};

ScanTarget readScanTarget(PayloadReader& reader)
{
    ScanTarget target;
    target.frequencyKhz = reader.readPod<std::uint32_t>();
    target.symbolRateKs = reader.readPod<std::uint32_t>();
    target.localOscillatorKhz = reader.readPod<std::uint32_t>();
    target.antenna = static_cast<Antenna>(reader.readPod<std::uint32_t>());
    target.system = static_cast<DvbSystem>(reader.readPod<std::uint32_t>());
    target.fec = reader.readString();
    target.label = reader.readString();
    return target;
}

ReceiverStatus readReceiverStatus(PayloadReader& reader)
{
    ReceiverStatus status;
    status.receiver = ReceiverId{reader.readPod<int>()};
    status.state = static_cast<ReceiverState>(reader.readPod<std::uint32_t>());
    if (reader.readBool()) {
        status.target = readScanTarget(reader);
    }
    if (reader.readBool()) {
        status.merDb = reader.readPod<double>();
    }
    if (reader.readBool()) {
        status.dNumberDb = reader.readPod<double>();
    }
    status.serviceName = reader.readOptionalString();
    status.modulation = reader.readOptionalString();
    status.transportPackets = reader.readPod<std::uint64_t>();
    status.continuityErrors = reader.readPod<std::uint64_t>();
    status.updatedAt = std::chrono::steady_clock::now();
    return status;
}

std::optional<ActiveInput> readActive(std::span<const std::byte> payload)
{
    PayloadReader reader{payload};
    if (!reader.readBool()) {
        return std::nullopt;
    }
    ActiveInput input;
    input.receiver = ReceiverId{reader.readPod<int>()};
    input.target = readScanTarget(reader);
    input.status = readReceiverStatus(reader);
    return input;
}

bool readBoolPayload(std::span<const std::byte> payload)
{
    PayloadReader reader{payload};
    return reader.readBool();
}

std::string readStringPayload(std::span<const std::byte> payload)
{
    PayloadReader reader{payload};
    return reader.readString();
}

void closeFd(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void runMediaChild(RepeaterConfig config, int socket)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    MediaPipeline media{std::move(config)};
    std::vector<std::byte> buffer(2 * 1024 * 1024 + sizeof(MessageHeader));
    bool stopping = false;
    while (!stopping) {
        pollfd pfd{.fd = socket, .events = POLLIN, .revents = 0};
        const auto pollStatus = ::poll(&pfd, 1, 1000);
        if (pollStatus < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error{"media child poll failed: " + std::string{std::strerror(errno)}};
        }
        if (pollStatus == 0) {
            continue;
        }
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            break;
        }

        const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error{"media child receive failed: " + std::string{std::strerror(errno)}};
        }
        if (static_cast<std::size_t>(received) < sizeof(MessageHeader)) {
            std::cerr << "wh-repeater-media: ignoring short control message\n";
            continue;
        }

        MessageHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        const auto payloadSize = static_cast<std::size_t>(received) - sizeof(MessageHeader);
        if (header.size != payloadSize) {
            std::cerr << "wh-repeater-media: ignoring malformed control message\n";
            continue;
        }
        const std::span<const std::byte> payload{buffer.data() + sizeof(MessageHeader), payloadSize};
        switch (static_cast<MediaMessage>(header.type)) {
        case MediaMessage::shutdown:
            stopping = true;
            break;
        case MediaMessage::select:
            media.select(readActive(payload));
            break;
        case MediaMessage::beacon:
            media.setBeaconAllowed(readBoolPayload(payload));
            break;
        case MediaMessage::notice:
            if (payload.empty()) {
                media.setAccessNotice(std::nullopt);
            } else {
                media.setAccessNotice(readStringPayload(payload));
            }
            break;
        case MediaMessage::playFallbackVideo:
            media.playFallbackVideo(readStringPayload(payload));
            break;
        case MediaMessage::stopFallbackVideo:
            media.stopFallbackVideo();
            break;
        case MediaMessage::tick:
            media.tick(std::chrono::steady_clock::now());
            break;
        case MediaMessage::transportStream:
            media.write(payload);
            break;
        }
    }
}

} // namespace

MediaProcess::MediaProcess(RepeaterConfig config)
    : config_{std::move(config)}
{
    ensureRunning();
}

MediaProcess::~MediaProcess()
{
    stopChild();
}

void MediaProcess::select(std::optional<ActiveInput> input)
{
    active_ = std::move(input);
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::select), activePayload(active_), true);
}

void MediaProcess::setBeaconAllowed(bool allowed)
{
    beaconAllowed_ = allowed;
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::beacon), boolPayload(allowed), true);
}

void MediaProcess::setAccessNotice(std::optional<std::string> notice)
{
    accessNotice_ = std::move(notice);
    if (!accessNotice_.has_value()) {
        (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), {}, true);
        return;
    }
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), stringPayload(*accessNotice_), true);
}

void MediaProcess::playFallbackVideo(std::string path)
{
    fallbackVideoPath_ = path;
    mode_ = MediaPipelineMode::fallbackVideo;
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::playFallbackVideo), stringPayload(path), true);
}

void MediaProcess::stopFallbackVideo()
{
    fallbackVideoPath_.reset();
    mode_ = MediaPipelineMode::fallback;
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::stopFallbackVideo), {}, true);
}

void MediaProcess::tick(std::chrono::steady_clock::time_point)
{
    if (fallbackVideoPath_.has_value()) {
        mode_ = MediaPipelineMode::fallbackVideo;
    } else if (active_.has_value()) {
        mode_ = MediaPipelineMode::retransmit;
    } else if (config_.fallback.enabled) {
        mode_ = MediaPipelineMode::fallback;
    } else {
        mode_ = MediaPipelineMode::idle;
    }
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::tick), {}, false);
}

void MediaProcess::write(std::span<const std::byte> packet)
{
    if (packet.empty()) {
        return;
    }
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::transportStream), packet, false);
}

MediaPipelineMode MediaProcess::mode() const
{
    return mode_;
}

void MediaProcess::ensureRunning()
{
    reapChild(true);
    if (childPid_ < 0) {
        startChild();
    }
}

void MediaProcess::startChild()
{
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) < 0) {
        throw std::runtime_error{"create media socketpair failed: " + std::string{std::strerror(errno)}};
    }

    const auto pid = ::fork();
    if (pid < 0) {
        closeFd(sockets[0]);
        closeFd(sockets[1]);
        throw std::runtime_error{"fork media process failed: " + std::string{std::strerror(errno)}};
    }

    if (pid == 0) {
        closeFd(sockets[0]);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        try {
            runMediaChild(config_, sockets[1]);
            closeFd(sockets[1]);
            _exit(0);
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater-media: " << ex.what() << '\n';
            closeFd(sockets[1]);
            _exit(1);
        }
    }

    closeFd(sockets[1]);
    socket_ = sockets[0];
    childPid_ = static_cast<int>(pid);
    std::cout << "media process started pid=" << childPid_ << '\n';
    replayState();
}

void MediaProcess::stopChild()
{
    if (socket_ >= 0) {
        const MessageHeader header{static_cast<std::uint32_t>(MediaMessage::shutdown), 0};
        (void)::send(socket_, &header, sizeof(header), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    closeFd(socket_);
    if (childPid_ >= 0) {
        int status{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto waited = ::waitpid(childPid_, &status, WNOHANG);
            if (waited == childPid_) {
                childPid_ = -1;
                return;
            }
            if (waited < 0 && errno == ECHILD) {
                childPid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        ::kill(childPid_, SIGKILL);
        (void)::waitpid(childPid_, &status, 0);
        childPid_ = -1;
    }
}

void MediaProcess::reapChild(bool restart)
{
    if (childPid_ < 0) {
        return;
    }
    int status{};
    const auto waited = ::waitpid(childPid_, &status, WNOHANG);
    if (waited == 0) {
        return;
    }
    if (waited < 0 && errno != ECHILD) {
        return;
    }
    if (waited == childPid_) {
        if (WIFSIGNALED(status)) {
            std::cerr << "wh-repeater: media process exited on signal " << WTERMSIG(status) << '\n';
        } else {
            std::cerr << "wh-repeater: media process exited with status "
                      << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << '\n';
        }
    }
    closeFd(socket_);
    childPid_ = -1;
    if (restart) {
        startChild();
    }
}

bool MediaProcess::sendMessage(std::uint32_t type, std::span<const std::byte> payload, bool essential)
{
    ensureRunning();
    if (socket_ < 0) {
        return false;
    }

    std::vector<std::byte> message;
    message.reserve(sizeof(MessageHeader) + payload.size());
    const MessageHeader header{type, static_cast<std::uint32_t>(payload.size())};
    appendPod(message, header);
    message.insert(message.end(), payload.begin(), payload.end());
    const auto status = ::send(socket_, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (status == static_cast<ssize_t>(message.size())) {
        return true;
    }
    if (status < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !essential) {
        return false;
    }
    if (status < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && essential) {
        pollfd pfd{.fd = socket_, .events = POLLOUT, .revents = 0};
        if (::poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLOUT) != 0) {
            const auto retryStatus = ::send(socket_, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (retryStatus == static_cast<ssize_t>(message.size())) {
                return true;
            }
        }
    }
    if (status < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) {
        std::cerr << "wh-repeater: media process socket closed; restarting\n";
        stopChild();
        startChild();
        return false;
    }
    if (essential) {
        std::cerr << "wh-repeater: send media command failed: "
                  << (status < 0 ? std::strerror(errno) : "short write") << '\n';
    }
    return false;
}

void MediaProcess::replayState()
{
    if (socket_ < 0) {
        return;
    }
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::select), activePayload(active_), true);
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::beacon), boolPayload(beaconAllowed_), true);
    if (accessNotice_.has_value()) {
        (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), stringPayload(*accessNotice_), true);
    } else {
        (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), {}, true);
    }
    if (fallbackVideoPath_.has_value()) {
        (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::playFallbackVideo), stringPayload(*fallbackVideoPath_), true);
    }
}

} // namespace whrepeater
