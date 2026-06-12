/*
 * ============================================================================
 *  wh-repeater - TS Gateway UDP Inspector
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Receives raw MPEG-TS datagrams from wh-repeater ts-gateway mode and
 *    reports packet grouping, bitrate, continuity, PID, PMT, and service data.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    This diagnostic is intentionally standalone so it can run on the future
 *    PC gateway server without linking the Pi-local daemon or media pipeline.
 * ============================================================================
 */

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kTsPacketSize{188};
constexpr std::size_t kPreferredDatagramSize{kTsPacketSize * 7};
volatile sig_atomic_t g_stop{};

struct Options {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{5000};
    int seconds{0};
};

struct PidStats {
    std::uint64_t packets{};
    std::uint64_t continuityErrors{};
    std::optional<std::uint8_t> lastContinuity;
};

struct ProgramInfo {
    std::uint16_t serviceId{};
    std::uint16_t pmtPid{};
    std::uint16_t pcrPid{};
    std::string provider;
    std::string serviceName;
    std::vector<std::uint8_t> streamTypes;
};

struct SectionBuffer {
    std::vector<std::uint8_t> bytes;
    std::optional<std::size_t> expectedSize;
};

struct Stats {
    std::uint64_t datagrams{};
    std::uint64_t preferredDatagrams{};
    std::uint64_t shortDatagrams{};
    std::uint64_t unalignedDatagrams{};
    std::uint64_t bytes{};
    std::uint64_t packets{};
    std::uint64_t syncErrors{};
    std::map<std::uint16_t, PidStats> pids;
    std::map<std::uint16_t, ProgramInfo> programs;
    std::map<std::uint16_t, SectionBuffer> sections;
};

[[noreturn]] void usage(std::string_view error = {})
{
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n\n";
    }
    std::cerr
        << "usage: ts-gateway-inspect [--bind ADDRESS] [--port PORT] [--seconds N]\n"
        << "\n"
        << "Receives wh-repeater ts-gateway UDP MPEG-TS output and reports\n"
        << "packet grouping, bitrate, continuity errors, and basic service data.\n";
    std::exit(error.empty() ? 0 : 2);
}

std::uint16_t parsePort(std::string_view text)
{
    const auto value = std::stoul(std::string{text});
    if (value == 0 || value > 65535) {
        usage("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        auto requireValue = [&](std::string_view name) -> std::string_view {
            if (index + 1 >= argc) {
                usage(std::string{name} + " requires a value");
            }
            ++index;
            return argv[index];
        };

        if (arg == "--help" || arg == "-h") {
            usage();
        } else if (arg == "--bind") {
            options.bindAddress = std::string{requireValue(arg)};
        } else if (arg == "--port") {
            options.port = parsePort(requireValue(arg));
        } else if (arg == "--seconds") {
            options.seconds = std::stoi(std::string{requireValue(arg)});
            if (options.seconds < 0) {
                usage("seconds must be zero or positive");
            }
        } else {
            usage("unknown argument: " + std::string{arg});
        }
    }
    return options;
}

void onSignal(int)
{
    g_stop = 1;
}

std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

std::string dvbString(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty()) {
        return {};
    }
    std::size_t offset = bytes[0] < 0x20 ? 1 : 0;
    std::string out;
    for (; offset < bytes.size(); ++offset) {
        const auto ch = bytes[offset];
        out.push_back(ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : ' ');
    }
    return out;
}

void parsePat(Stats& stats, std::span<const std::uint8_t> section)
{
    if (section.size() < 12 || section[0] != 0x00) {
        return;
    }
    const auto sectionLength = static_cast<std::size_t>(((section[1] & 0x0fU) << 8U) | section[2]);
    const auto end = 3 + sectionLength;
    if (end > section.size() || end < 4) {
        return;
    }
    for (std::size_t offset = 8; offset + 4 <= end - 4; offset += 4) {
        const auto serviceId = read16(section, offset);
        const auto pid = static_cast<std::uint16_t>(((section[offset + 2] & 0x1fU) << 8U) | section[offset + 3]);
        if (serviceId == 0) {
            continue;
        }
        auto& program = stats.programs[serviceId];
        program.serviceId = serviceId;
        program.pmtPid = pid;
    }
}

void parsePmt(Stats& stats, std::uint16_t pid, std::span<const std::uint8_t> section)
{
    if (section.size() < 16 || section[0] != 0x02) {
        return;
    }
    const auto serviceId = read16(section, 3);
    const auto sectionLength = static_cast<std::size_t>(((section[1] & 0x0fU) << 8U) | section[2]);
    const auto end = 3 + sectionLength;
    if (end > section.size() || end < 4) {
        return;
    }
    auto& program = stats.programs[serviceId];
    program.serviceId = serviceId;
    program.pmtPid = pid;
    program.pcrPid = static_cast<std::uint16_t>(((section[8] & 0x1fU) << 8U) | section[9]);
    const auto programInfoLength = static_cast<std::size_t>(((section[10] & 0x0fU) << 8U) | section[11]);
    std::size_t offset = 12 + programInfoLength;
    program.streamTypes.clear();
    while (offset + 5 <= end - 4) {
        program.streamTypes.push_back(section[offset]);
        const auto esInfoLength = static_cast<std::size_t>(((section[offset + 3] & 0x0fU) << 8U) | section[offset + 4]);
        offset += 5 + esInfoLength;
    }
}

void parseSdt(Stats& stats, std::span<const std::uint8_t> section)
{
    if (section.size() < 15 || section[0] != 0x42) {
        return;
    }
    const auto sectionLength = static_cast<std::size_t>(((section[1] & 0x0fU) << 8U) | section[2]);
    const auto end = 3 + sectionLength;
    if (end > section.size() || end < 4) {
        return;
    }
    std::size_t offset = 11;
    while (offset + 5 <= end - 4) {
        const auto serviceId = read16(section, offset);
        const auto descriptorLength = static_cast<std::size_t>(((section[offset + 3] & 0x0fU) << 8U) | section[offset + 4]);
        std::size_t desc = offset + 5;
        const auto descEnd = desc + descriptorLength;
        while (desc + 2 <= descEnd && descEnd <= end - 4) {
            const auto tag = section[desc];
            const auto length = section[desc + 1];
            const auto body = desc + 2;
            if (tag == 0x48 && body + length <= descEnd && length >= 3) {
                const auto providerLength = section[body + 1];
                const auto providerOffset = body + 2;
                if (providerOffset + providerLength < body + length) {
                    const auto serviceLength = section[providerOffset + providerLength];
                    const auto serviceOffset = providerOffset + providerLength + 1;
                    auto& program = stats.programs[serviceId];
                    program.serviceId = serviceId;
                    program.provider = dvbString({section.data() + providerOffset, providerLength});
                    if (serviceOffset + serviceLength <= body + length) {
                        program.serviceName = dvbString({section.data() + serviceOffset, serviceLength});
                    }
                }
            }
            desc += 2 + length;
        }
        offset = descEnd;
    }
}

void parseSection(Stats& stats, std::uint16_t pid, std::span<const std::uint8_t> section)
{
    if (pid == 0x0000) {
        parsePat(stats, section);
    } else if (pid == 0x0011) {
        parseSdt(stats, section);
    } else {
        for (const auto& [serviceId, program] : stats.programs) {
            (void)serviceId;
            if (program.pmtPid == pid) {
                parsePmt(stats, pid, section);
                return;
            }
        }
    }
}

void appendPayloadSection(Stats& stats, std::uint16_t pid, bool payloadStart, std::span<const std::uint8_t> payload)
{
    if (payload.empty()) {
        return;
    }

    auto& buffer = stats.sections[pid];
    if (payloadStart) {
        const auto pointer = static_cast<std::size_t>(payload[0]);
        if (pointer + 1 > payload.size()) {
            return;
        }
        payload = payload.subspan(pointer + 1);
        buffer.bytes.clear();
        buffer.expectedSize.reset();
    }
    if (payload.empty() || payload[0] == 0xff) {
        return;
    }

    buffer.bytes.insert(buffer.bytes.end(), payload.begin(), payload.end());
    if (!buffer.expectedSize.has_value() && buffer.bytes.size() >= 3) {
        const auto sectionLength = static_cast<std::size_t>(((buffer.bytes[1] & 0x0fU) << 8U) | buffer.bytes[2]);
        buffer.expectedSize = 3 + sectionLength;
    }
    if (buffer.expectedSize.has_value() && buffer.bytes.size() >= *buffer.expectedSize) {
        parseSection(stats, pid, {buffer.bytes.data(), *buffer.expectedSize});
        buffer.bytes.clear();
        buffer.expectedSize.reset();
    }
}

void inspectPacket(Stats& stats, std::span<const std::uint8_t> packet)
{
    if (packet.size() != kTsPacketSize || packet[0] != 0x47) {
        ++stats.syncErrors;
        return;
    }

    const auto payloadStart = (packet[1] & 0x40U) != 0;
    const auto pid = static_cast<std::uint16_t>(((packet[1] & 0x1fU) << 8U) | packet[2]);
    const auto adaptationControl = static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
    const auto continuity = static_cast<std::uint8_t>(packet[3] & 0x0fU);
    auto& pidStats = stats.pids[pid];
    ++pidStats.packets;

    const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
    if (hasPayload && pid != 0x1fff) {
        if (pidStats.lastContinuity.has_value()) {
            const auto expected = static_cast<std::uint8_t>((*pidStats.lastContinuity + 1U) & 0x0fU);
            if (continuity != expected) {
                ++pidStats.continuityErrors;
            }
        }
        pidStats.lastContinuity = continuity;
    }

    std::size_t payloadOffset = 4;
    if (adaptationControl == 2 || adaptationControl == 3) {
        const auto adaptationLength = packet[4];
        payloadOffset += 1 + adaptationLength;
    }
    if (hasPayload && payloadOffset < packet.size()) {
        appendPayloadSection(stats, pid, payloadStart, packet.subspan(payloadOffset));
    }
}

int openSocket(const Options& options)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        throw std::runtime_error{"create UDP socket: " + std::string{std::strerror(errno)}};
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.bindAddress.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error{"invalid bind address: " + options.bindAddress};
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error{"bind UDP socket: " + std::string{std::strerror(errno)}};
    }
    return fd;
}

std::string streamTypeName(std::uint8_t streamType)
{
    switch (streamType) {
    case 0x02:
        return "MPEG-2 video";
    case 0x03:
        return "MPEG-1 audio";
    case 0x04:
        return "MPEG-2 audio";
    case 0x0f:
        return "AAC";
    case 0x1b:
        return "H.264";
    case 0x24:
        return "H.265";
    default:
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(streamType);
        return out.str();
    }
}

void printSummary(const Stats& stats, double elapsedSeconds)
{
    const auto bitrate = elapsedSeconds > 0.0 ? static_cast<double>(stats.bytes * 8) / elapsedSeconds : 0.0;
    std::cout << "\nTS gateway receive summary\n"
              << "  datagrams: " << stats.datagrams
              << " (" << stats.preferredDatagrams << " x 1316-byte, "
              << stats.shortDatagrams << " short, "
              << stats.unalignedDatagrams << " unaligned)\n"
              << "  packets:   " << stats.packets << "\n"
              << "  bytes:     " << stats.bytes << "\n"
              << "  bitrate:   " << std::fixed << std::setprecision(0) << bitrate << " bit/s\n"
              << "  sync err:  " << stats.syncErrors << "\n";

    std::uint64_t totalContinuityErrors{};
    for (const auto& [pid, pidStats] : stats.pids) {
        (void)pid;
        totalContinuityErrors += pidStats.continuityErrors;
    }
    std::cout << "  cc err:    " << totalContinuityErrors << "\n";

    if (!stats.programs.empty()) {
        std::cout << "\nPrograms\n";
        for (const auto& [serviceId, program] : stats.programs) {
            std::cout << "  service " << serviceId
                      << " pmt=0x" << std::hex << program.pmtPid
                      << " pcr=0x" << program.pcrPid << std::dec;
            if (!program.serviceName.empty()) {
                std::cout << " name=\"" << program.serviceName << "\"";
            }
            if (!program.provider.empty()) {
                std::cout << " provider=\"" << program.provider << "\"";
            }
            std::cout << "\n";
            if (!program.streamTypes.empty()) {
                std::cout << "    streams:";
                for (const auto streamType : program.streamTypes) {
                    std::cout << ' ' << streamTypeName(streamType);
                }
                std::cout << "\n";
            }
        }
    }

    std::cout << "\nTop PIDs\n";
    std::size_t printed{};
    for (const auto& [pid, pidStats] : stats.pids) {
        if (printed++ >= 12) {
            break;
        }
        std::cout << "  0x" << std::hex << std::setw(4) << std::setfill('0') << pid << std::dec << std::setfill(' ')
                  << " packets=" << pidStats.packets
                  << " ccErrors=" << pidStats.continuityErrors << "\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parseOptions(argc, argv);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        const int fd = openSocket(options);
        std::cout << "listening for TS gateway UDP on " << options.bindAddress << ':' << options.port << '\n';

        Stats stats;
        std::vector<std::uint8_t> buffer(65536);
        const auto start = std::chrono::steady_clock::now();

        while (!g_stop) {
            if (options.seconds > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= options.seconds) {
                    break;
                }
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(fd, &readSet);
            timeval timeout{1, 0};
            const auto ready = ::select(fd + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error{"select: " + std::string{std::strerror(errno)}};
            }
            if (ready == 0) {
                continue;
            }

            const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error{"recv: " + std::string{std::strerror(errno)}};
            }

            const auto size = static_cast<std::size_t>(received);
            ++stats.datagrams;
            stats.bytes += size;
            if (size == kPreferredDatagramSize) {
                ++stats.preferredDatagrams;
            } else if (size % kTsPacketSize != 0) {
                ++stats.unalignedDatagrams;
            } else {
                ++stats.shortDatagrams;
            }

            for (std::size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
                ++stats.packets;
                inspectPacket(stats, {buffer.data() + offset, kTsPacketSize});
            }
        }

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printSummary(stats, elapsed);
        ::close(fd);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ts-gateway-inspect: " << ex.what() << '\n';
        return 1;
    }
}
