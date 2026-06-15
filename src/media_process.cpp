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
    preview = 9,
    streamInfo = 10,
    seekFallbackVideo = 11,
    fallbackVideoStatus = 12,
    noticeEndTone = 13,
};

struct MessageHeader {
    std::uint32_t type{};
    std::uint32_t size{};
};

constexpr std::size_t transportPacketBytes{188};
constexpr std::size_t maxTransportPayload{transportPacketBytes * 340};
constexpr std::size_t maxBufferedTransport{transportPacketBytes * 65536};

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
    auto stableStatus = input->status;
    stableStatus.merDb.reset();
    stableStatus.dNumberDb.reset();
    stableStatus.transportPackets = 0;
    stableStatus.continuityErrors = 0;
    appendReceiverStatus(payload, stableStatus);
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

std::vector<std::byte> int64Payload(std::int64_t value)
{
    std::vector<std::byte> payload;
    appendPod(payload, value);
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

std::int64_t readInt64Payload(std::span<const std::byte> payload)
{
    PayloadReader reader{payload};
    return reader.readPod<std::int64_t>();
}

void sendChildMessage(int socket, MediaMessage type, std::span<const std::byte> payload)
{
    std::vector<std::byte> message;
    message.reserve(sizeof(MessageHeader) + payload.size());
    const MessageHeader header{static_cast<std::uint32_t>(type), static_cast<std::uint32_t>(payload.size())};
    appendPod(message, header);
    message.insert(message.end(), payload.begin(), payload.end());
    (void)::send(socket, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
}

void sendChildString(int socket, MediaMessage type, std::string_view value)
{
    sendChildMessage(socket, type, stringPayload(value));
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
        case MediaMessage::noticeEndTone:
            media.setAccessNoticeEndTone(readBoolPayload(payload));
            break;
        case MediaMessage::playFallbackVideo:
            media.playFallbackVideo(readStringPayload(payload));
            break;
        case MediaMessage::stopFallbackVideo:
            media.stopFallbackVideo();
            break;
        case MediaMessage::seekFallbackVideo:
            media.seekFallbackVideo(std::chrono::milliseconds{readInt64Payload(payload)});
            break;
        case MediaMessage::tick:
            media.tick(std::chrono::steady_clock::now());
            break;
        case MediaMessage::transportStream:
            media.write(payload);
            break;
        case MediaMessage::preview:
            media.setPreviewEnabled(readBoolPayload(payload));
            break;
        case MediaMessage::streamInfo:
        case MediaMessage::fallbackVideoStatus:
            break;
        }
        if (auto streamInfo = media.takeStreamInfoUpdate(); streamInfo.has_value()) {
            sendChildString(socket, MediaMessage::streamInfo, *streamInfo);
        }
        if (auto fallbackStatus = media.takeFallbackVideoStatusUpdate(); fallbackStatus.has_value()) {
            sendChildString(socket, MediaMessage::fallbackVideoStatus, *fallbackStatus);
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
    auto payload = activePayload(active_);
    if (lastSelectPayload_.has_value() && *lastSelectPayload_ == payload) {
        return;
    }
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::select), payload, true)) {
        lastSelectPayload_ = std::move(payload);
    }
}

void MediaProcess::setBeaconAllowed(bool allowed)
{
    beaconAllowed_ = allowed;
    auto payload = boolPayload(allowed);
    if (lastBeaconPayload_.has_value() && *lastBeaconPayload_ == payload) {
        return;
    }
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::beacon), payload, true)) {
        lastBeaconPayload_ = std::move(payload);
    }
}

void MediaProcess::setAccessNotice(std::optional<std::string> notice, bool endTone)
{
    accessNotice_ = std::move(notice);
    accessNoticeEndTone_ = accessNotice_.has_value() && endTone;
    auto payload = accessNotice_.has_value() ? stringPayload(*accessNotice_) : std::vector<std::byte>{};
    if (!lastNoticePayload_.has_value() || *lastNoticePayload_ != payload) {
        if (sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), payload, true)) {
            lastNoticePayload_ = std::move(payload);
        }
    }
    auto tonePayload = boolPayload(accessNoticeEndTone_);
    if (!lastNoticeEndTonePayload_.has_value() || *lastNoticeEndTonePayload_ != tonePayload) {
        if (sendMessage(static_cast<std::uint32_t>(MediaMessage::noticeEndTone), tonePayload, true)) {
            lastNoticeEndTonePayload_ = std::move(tonePayload);
        }
    }
}

void MediaProcess::playFallbackVideo(std::string path)
{
    fallbackVideoPath_ = path;
    fallbackVideoSeek_.reset();
    fallbackVideoStatus_.reset();
    mode_ = MediaPipelineMode::fallbackVideo;
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::playFallbackVideo), stringPayload(path), true);
}

void MediaProcess::stopFallbackVideo()
{
    fallbackVideoPath_.reset();
    fallbackVideoSeek_.reset();
    fallbackVideoStatus_.reset();
    mode_ = MediaPipelineMode::fallback;
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::stopFallbackVideo), {}, true);
}

void MediaProcess::seekFallbackVideo(std::chrono::milliseconds position)
{
    fallbackVideoSeek_ = std::max(std::chrono::milliseconds{0}, position);
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::seekFallbackVideo),
                      int64Payload(fallbackVideoSeek_->count()),
                      true);
}

void MediaProcess::setPreviewEnabled(bool enabled)
{
    previewEnabled_ = enabled;
    auto payload = boolPayload(enabled);
    if (lastPreviewPayload_.has_value() && *lastPreviewPayload_ == payload) {
        return;
    }
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::preview), payload, true)) {
        lastPreviewPayload_ = std::move(payload);
    }
}

void MediaProcess::tick(std::chrono::steady_clock::time_point)
{
    drainChildMessages();
    if (fallbackVideoPath_.has_value()) {
        mode_ = MediaPipelineMode::fallbackVideo;
    } else if (active_.has_value()) {
        mode_ = MediaPipelineMode::retransmit;
    } else if (config_.fallback.enabled) {
        mode_ = MediaPipelineMode::fallback;
    } else {
        mode_ = MediaPipelineMode::idle;
    }
    flushTransportBuffer(false);
    (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::tick), {}, false);
    drainChildMessages();
}

void MediaProcess::write(std::span<const std::byte> packet)
{
    if (packet.empty()) {
        return;
    }
    transportBuffer_.insert(transportBuffer_.end(), packet.begin(), packet.end());
    flushTransportBuffer(false);
    if (transportBuffer_.size() > maxBufferedTransport) {
        const auto keepBytes = maxBufferedTransport / 2;
        const auto alignedKeepBytes = keepBytes - (keepBytes % transportPacketBytes);
        const auto eraseBytes = transportBuffer_.size() - alignedKeepBytes;
        transportBuffer_.erase(transportBuffer_.begin(), transportBuffer_.begin() + static_cast<std::ptrdiff_t>(eraseBytes));
        std::cerr << "wh-repeater: media transport backlog exceeded "
                  << maxBufferedTransport << " bytes; dropped oldest queued TS\n";
    }
}

MediaPipelineMode MediaProcess::mode() const
{
    return mode_;
}

std::optional<std::string> MediaProcess::streamInfo() const
{
    return streamInfo_;
}

std::optional<std::string> MediaProcess::fallbackVideoStatus() const
{
    return fallbackVideoStatus_;
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
    lastSelectPayload_.reset();
    lastBeaconPayload_.reset();
    lastNoticePayload_.reset();
    lastPreviewPayload_.reset();
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

void MediaProcess::drainChildMessages()
{
    if (socket_ < 0) {
        return;
    }

    std::vector<std::byte> buffer(64 * 1024);
    while (true) {
        const auto received = ::recv(socket_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                std::cerr << "wh-repeater: media process socket closed while reading; restarting\n";
                stopChild();
                startChild();
            }
            return;
        }
        if (received == 0) {
            return;
        }
        if (static_cast<std::size_t>(received) < sizeof(MessageHeader)) {
            continue;
        }

        MessageHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        const auto payloadSize = static_cast<std::size_t>(received) - sizeof(MessageHeader);
        if (header.size != payloadSize) {
            continue;
        }
        const std::span<const std::byte> payload{buffer.data() + sizeof(MessageHeader), payloadSize};
        if (static_cast<MediaMessage>(header.type) == MediaMessage::streamInfo) {
            streamInfo_ = readStringPayload(payload);
        } else if (static_cast<MediaMessage>(header.type) == MediaMessage::fallbackVideoStatus) {
            fallbackVideoStatus_ = readStringPayload(payload);
        }
    }
}

void MediaProcess::flushTransportBuffer(bool essential)
{
    while (!transportBuffer_.empty()) {
        const auto chunkSize = std::min(maxTransportPayload, transportBuffer_.size());
        const std::span<const std::byte> chunk{transportBuffer_.data(), chunkSize};
        if (!sendMessage(static_cast<std::uint32_t>(MediaMessage::transportStream), chunk, essential)) {
            return;
        }
        transportBuffer_.erase(transportBuffer_.begin(), transportBuffer_.begin() + static_cast<std::ptrdiff_t>(chunkSize));
    }
}

void MediaProcess::replayState()
{
    if (socket_ < 0) {
        return;
    }
    auto select = activePayload(active_);
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::select), select, true)) {
        lastSelectPayload_ = std::move(select);
    }
    auto beacon = boolPayload(beaconAllowed_);
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::beacon), beacon, true)) {
        lastBeaconPayload_ = std::move(beacon);
    }
    std::vector<std::byte> notice;
    if (accessNotice_.has_value()) {
        notice = stringPayload(*accessNotice_);
    }
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::notice), notice, true)) {
        lastNoticePayload_ = std::move(notice);
    }
    auto noticeEndTone = boolPayload(accessNoticeEndTone_);
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::noticeEndTone), noticeEndTone, true)) {
        lastNoticeEndTonePayload_ = std::move(noticeEndTone);
    }
    auto preview = boolPayload(previewEnabled_);
    if (sendMessage(static_cast<std::uint32_t>(MediaMessage::preview), preview, true)) {
        lastPreviewPayload_ = std::move(preview);
    }
    if (fallbackVideoPath_.has_value()) {
        (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::playFallbackVideo), stringPayload(*fallbackVideoPath_), true);
        if (fallbackVideoSeek_.has_value()) {
            (void)sendMessage(static_cast<std::uint32_t>(MediaMessage::seekFallbackVideo),
                              int64Payload(fallbackVideoSeek_->count()),
                              true);
        }
    }
}

} // namespace whrepeater
