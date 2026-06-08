/*
 * ============================================================================
 *  wh-repeater - Serit NIM Hardware Controller
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Ports Winterhill hardware access into C++ for whdriver, Serit NIM selection, STV0910 demodulators, STV6120 tuners, PIC control, TS reads, and receiver status.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/nim_controller.hpp"

#include "whrepeater/i2c_bus_lock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/i2c-dev.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

constexpr std::uint8_t nimDemodAddrA = 0xd0;
constexpr std::uint8_t nimDemodAddrB = 0xd2;
constexpr std::uint8_t nimTunerAddr = 0xc0;

constexpr std::uint16_t stv0910Mid = 0xf100;
constexpr std::uint16_t stv0910Did = 0xf101;
constexpr std::uint16_t stv0910P1I2cRpt = 0xf12a;
constexpr std::uint16_t stv0910P2DmdCfgMd = 0xf214;
constexpr std::uint16_t stv0910P2DmdState = 0xf21b;
constexpr std::uint16_t stv0910P2DmdIState = 0xf216;
constexpr std::uint16_t stv0910P2CfrUp1 = 0xf242;
constexpr std::uint16_t stv0910P2CfrUp0 = 0xf243;
constexpr std::uint16_t stv0910P2CfrLow1 = 0xf246;
constexpr std::uint16_t stv0910P2CfrLow0 = 0xf247;
constexpr std::uint16_t stv0910P2CfrInit1 = 0xf248;
constexpr std::uint16_t stv0910P2CfrInit0 = 0xf249;
constexpr std::uint16_t stv0910P2SfrInit1 = 0xf25e;
constexpr std::uint16_t stv0910P2SfrInit0 = 0xf25f;
constexpr std::uint16_t stv0910P2NosRamPos = 0xf30e;
constexpr std::uint16_t stv0910P2NosRamVal = 0xf30f;
constexpr std::uint16_t stv0910P1DmdCfgMd = 0xf414;
constexpr std::uint16_t stv0910P1DmdState = 0xf41b;
constexpr std::uint16_t stv0910P1DmdIState = 0xf416;
constexpr std::uint16_t stv0910P1CfrUp1 = 0xf442;
constexpr std::uint16_t stv0910P1CfrUp0 = 0xf443;
constexpr std::uint16_t stv0910P1CfrLow1 = 0xf446;
constexpr std::uint16_t stv0910P1CfrLow0 = 0xf447;
constexpr std::uint16_t stv0910P1CfrInit1 = 0xf448;
constexpr std::uint16_t stv0910P1CfrInit0 = 0xf449;
constexpr std::uint16_t stv0910P1SfrInit1 = 0xf45e;
constexpr std::uint16_t stv0910P1SfrInit0 = 0xf45f;
constexpr std::uint16_t stv0910P1NosRamPos = 0xf50e;
constexpr std::uint16_t stv0910P1NosRamVal = 0xf50f;
constexpr std::uint16_t stv0910P1Vth34 = 0xf536;
constexpr std::uint16_t stv0910TstRes0 = 0xff11;

constexpr std::uint8_t stv6120Ctrl1 = 0x00;
constexpr std::uint8_t stv6120Ctrl2 = 0x01;
constexpr std::uint8_t stv6120Ctrl3 = 0x02;
constexpr std::uint8_t stv6120Ctrl4 = 0x03;
constexpr std::uint8_t stv6120Ctrl5 = 0x04;
constexpr std::uint8_t stv6120Ctrl6 = 0x05;
constexpr std::uint8_t stv6120Ctrl7 = 0x06;
constexpr std::uint8_t stv6120Ctrl8 = 0x07;
constexpr std::uint8_t stv6120Stat1 = 0x08;
constexpr std::uint8_t stv6120Ctrl9 = 0x09;
constexpr std::uint8_t stv6120Ctrl10 = 0x0a;
constexpr std::uint8_t stv6120Ctrl11 = 0x0b;
constexpr std::uint8_t stv6120Ctrl12 = 0x0c;
constexpr std::uint8_t stv6120Ctrl13 = 0x0d;
constexpr std::uint8_t stv6120Ctrl14 = 0x0e;
constexpr std::uint8_t stv6120Ctrl15 = 0x0f;
constexpr std::uint8_t stv6120Ctrl16 = 0x10;
constexpr std::uint8_t stv6120Ctrl17 = 0x11;
constexpr std::uint8_t stv6120Stat2 = 0x12;
constexpr std::uint8_t stv6120Ctrl18 = 0x13;
constexpr std::uint8_t stv6120Ctrl19 = 0x14;
constexpr std::uint8_t stv6120Ctrl20 = 0x15;
constexpr std::uint8_t stv6120Ctrl21 = 0x16;
constexpr std::uint8_t stv6120Ctrl22 = 0x17;
constexpr std::uint8_t stv6120Ctrl23 = 0x18;

constexpr std::uint8_t repeaterOff = 0x38;
constexpr std::uint8_t repeaterOn = 0xb8;
constexpr std::uint8_t stv0910ScanBlindBestGuess = 0x15;
constexpr std::size_t whDriverPacketSize = 192;
constexpr std::uint32_t nimTunerXtalKhz = 30'000;
constexpr std::uint32_t nimDemodMclkHz = 135'000'000;
constexpr std::uint16_t stv0910PllLockTimeout = 100;
constexpr std::uint16_t stv6120CalTimeout = 200;
constexpr std::size_t maxQueuedPacketsPerReceiver = 4096;

constexpr std::uint32_t stv0910FieldCp = 0xf1b330f8;
constexpr std::uint32_t stv0910FieldIdf = 0xf1b30007;
constexpr std::uint32_t stv0910FieldNDiv = 0xf1b400ff;
constexpr std::uint32_t stv0910FieldOdf = 0xf1b5003f;
constexpr std::uint32_t stv0910FieldStandby = 0xf1b67080;
constexpr std::uint32_t stv0910FieldBypassPllCore = 0xf1b66040;
constexpr std::uint32_t stv0910FieldPllLock = 0xf1b80001;

constexpr std::uint32_t stv6120RdivThresholdKhz = 27'000;
constexpr std::uint32_t stv6120PThreshold1Khz = 299'000;
constexpr std::uint32_t stv6120PThreshold2Khz = 596'000;
constexpr std::uint32_t stv6120PThreshold3Khz = 1'191'000;

constexpr std::array<std::array<std::uint32_t, 3>, 7> stv6120IcpLookup{{
    {2'380'000, 2'472'000, 0},
    {2'473'000, 2'700'000, 1},
    {2'701'000, 3'021'000, 2},
    {3'022'000, 3'387'000, 3},
    {3'388'000, 3'845'000, 5},
    {3'846'000, 4'394'000, 6},
    {4'395'000, 4'760'000, 7},
}};

constexpr std::array<std::uint16_t, 32> stv6120Cfhf{
    6796, 5828, 4778, 4118, 3513, 3136, 2794, 2562,
    2331, 2169, 2006, 1890, 1771, 1680, 1586, 1514,
    1433, 1374, 1310, 1262, 1208, 1167, 1122, 1087,
    1049, 1018, 983, 956, 926, 902, 875, 854,
};

struct RegisterValue {
    std::uint16_t reg;
    std::uint8_t value;
};

constexpr std::array<RegisterValue, 747> stv0910InitRegisters{{
#include "whrepeater/stv0910_init_table.inc"
}};

struct WhDriverPacket {
    std::array<std::uint8_t, 188> ts;
    std::uint32_t status{};
};

struct PacketCounters {
    std::uint64_t transportPackets{};
    std::uint64_t nullPackets{};
    std::uint64_t syncErrors{};
    std::uint64_t inSequenceErrors{};
    std::uint64_t outSequenceErrors{};
    std::uint64_t restartErrors{};
    std::uint64_t overflowErrors{};
    std::uint8_t lastInSequence{};
    std::uint8_t lastOutSequence{};
    bool hasInSequence{};
    bool hasOutSequence{};
};

std::uint32_t littleEndianU32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

ReceiverStatus makeReceiverStatus(ReceiverId receiver)
{
    ReceiverStatus status;
    status.receiver = receiver;
    return status;
}

int checkedOpen(const std::filesystem::path& path, int flags)
{
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        throw std::system_error{errno, std::generic_category(), "open " + path.string()};
    }
    return fd;
}

void checkedClose(int fd)
{
    if (fd >= 0 && ::close(fd) != 0) {
        std::cerr << "wh-repeater: close failed: " << std::strerror(errno) << '\n';
    }
}

void checkedWriteAll(int fd, const void* data, std::size_t size, std::string_view context)
{
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    auto remaining = size;

    while (remaining > 0) {
        const auto written = ::write(fd, cursor, remaining);
        if (written < 0) {
            throw std::system_error{errno, std::generic_category(), std::string{context}};
        }
        if (written == 0) {
            throw std::system_error{EIO, std::generic_category(), std::string{context} + " made no progress"};
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

void configureWhDriverInterrupt(int fd, std::uint32_t spi5InterruptNumber)
{
    const auto written = ::write(fd, &spi5InterruptNumber, sizeof(spi5InterruptNumber));
    if (written < 0) {
        throw std::system_error{errno, std::generic_category(), "configure whdriver spi5 interrupt"};
    }
    if (written != 0 && written != static_cast<ssize_t>(sizeof(spi5InterruptNumber))) {
        throw std::system_error{EIO, std::generic_category(), "configure whdriver spi5 interrupt returned partial write"};
    }
}

std::uint32_t findInterruptNumber(const std::filesystem::path& interruptsFile, std::string_view marker)
{
    std::ifstream input{interruptsFile};
    if (!input) {
        throw std::runtime_error{"cannot open " + interruptsFile.string()};
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.find(marker) == std::string::npos) {
            continue;
        }

        const auto colon = line.find(':');
        const auto numberText = line.substr(0, colon);
        return static_cast<std::uint32_t>(std::stoul(numberText));
    }

    return 0;
}

class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd)
        : fd_{fd}
    {
    }

    ~FileDescriptor()
    {
        checkedClose(fd_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_{std::exchange(other.fd_, -1)}
    {
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other) {
            checkedClose(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int get() const
    {
        return fd_;
    }

    explicit operator bool() const
    {
        return fd_ >= 0;
    }

private:
    int fd_{-1};
};

class LinuxI2cBus {
public:
    explicit LinuxI2cBus(std::filesystem::path device)
        : device_{std::move(device)}
    {
    }

    std::uint8_t readReg8(std::uint8_t addr, std::uint8_t reg) const
    {
        auto fd = openSlave(addr);
        std::array<std::uint8_t, 1> address{reg};
        checkedWriteAll(fd.get(), address.data(), address.size(), "i2c set 8-bit register address");

        std::array<std::uint8_t, 1> value{};
        const auto read = ::read(fd.get(), value.data(), value.size());
        if (read != static_cast<ssize_t>(value.size())) {
            throw std::system_error{read < 0 ? errno : EIO, std::generic_category(), "i2c read 8-bit register"};
        }
        return value[0];
    }

    void writeReg8(std::uint8_t addr, std::uint8_t reg, std::uint8_t value) const
    {
        auto fd = openSlave(addr);
        const std::array payload{reg, value};
        checkedWriteAll(fd.get(), payload.data(), payload.size(), "i2c write 8-bit register");
    }

    std::uint8_t readReg16(std::uint8_t addr, std::uint16_t reg) const
    {
        auto fd = openSlave(addr);
        const std::array address{static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
        checkedWriteAll(fd.get(), address.data(), address.size(), "i2c set 16-bit register address");

        std::array<std::uint8_t, 1> value{};
        const auto read = ::read(fd.get(), value.data(), value.size());
        if (read != static_cast<ssize_t>(value.size())) {
            throw std::system_error{read < 0 ? errno : EIO, std::generic_category(), "i2c read 16-bit register"};
        }
        return value[0];
    }

    void writeReg16(std::uint8_t addr, std::uint16_t reg, std::uint8_t value) const
    {
        auto fd = openSlave(addr);
        const std::array payload{
            static_cast<std::uint8_t>(reg >> 8),
            static_cast<std::uint8_t>(reg),
            value,
        };
        checkedWriteAll(fd.get(), payload.data(), payload.size(), "i2c write 16-bit register");
    }

private:
    FileDescriptor openSlave(std::uint8_t shiftedAddr) const
    {
        auto fd = FileDescriptor{checkedOpen(device_, O_RDWR | O_CLOEXEC)};
        if (::ioctl(fd.get(), I2C_SLAVE, shiftedAddr >> 1) < 0) {
            throw std::system_error{errno, std::generic_category(), "select i2c slave 0x" + std::to_string(shiftedAddr)};
        }
        return fd;
    }

    std::filesystem::path device_;
};

struct ReceiverHardware {
    ReceiverId receiver;
    int nimIndex{};
    std::uint8_t nimAddress{};
    int tuner{};
};

ReceiverHardware hardwareFor(ReceiverId receiver)
{
    switch (receiver.value) {
    case 1:
        return {.receiver = receiver, .nimIndex = 0, .nimAddress = nimDemodAddrA, .tuner = 1};
    case 2:
        return {.receiver = receiver, .nimIndex = 0, .nimAddress = nimDemodAddrA, .tuner = 2};
    case 3:
        return {.receiver = receiver, .nimIndex = 1, .nimAddress = nimDemodAddrB, .tuner = 1};
    case 4:
        return {.receiver = receiver, .nimIndex = 1, .nimAddress = nimDemodAddrB, .tuner = 2};
    default:
        throw std::invalid_argument{"receiver must be 1..4"};
    }
}

std::uint16_t dmdStateRegister(int tuner)
{
    return tuner == 1 ? stv0910P2DmdState : stv0910P1DmdState;
}

std::uint16_t dmdIStateRegister(int tuner)
{
    return tuner == 1 ? stv0910P2DmdIState : stv0910P1DmdIState;
}

std::uint16_t dmdCfgMdRegister(int tuner)
{
    return tuner == 1 ? stv0910P2DmdCfgMd : stv0910P1DmdCfgMd;
}

std::uint16_t cfrInit0Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrInit0 : stv0910P1CfrInit0;
}

std::uint16_t cfrInit1Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrInit1 : stv0910P1CfrInit1;
}

std::uint16_t cfrUp0Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrUp0 : stv0910P1CfrUp0;
}

std::uint16_t cfrUp1Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrUp1 : stv0910P1CfrUp1;
}

std::uint16_t cfrLow0Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrLow0 : stv0910P1CfrLow0;
}

std::uint16_t cfrLow1Register(int tuner)
{
    return tuner == 1 ? stv0910P2CfrLow1 : stv0910P1CfrLow1;
}

std::uint16_t sfrInit0Register(int tuner)
{
    return tuner == 1 ? stv0910P2SfrInit0 : stv0910P1SfrInit0;
}

std::uint16_t sfrInit1Register(int tuner)
{
    return tuner == 1 ? stv0910P2SfrInit1 : stv0910P1SfrInit1;
}

std::uint16_t nosRamPosRegister(int tuner)
{
    return tuner == 1 ? stv0910P2NosRamPos : stv0910P1NosRamPos;
}

std::uint16_t nosRamValRegister(int tuner)
{
    return tuner == 1 ? stv0910P2NosRamVal : stv0910P1NosRamVal;
}

ReceiverState stateFromHeaderMode(std::uint8_t headerMode)
{
    switch ((headerMode >> 5) & 0x03) {
    case 1:
        return ReceiverState::foundHeader;
    case 2:
        return ReceiverState::lockedDvbs2;
    case 3:
        return ReceiverState::lockedDvbs;
    default:
        return ReceiverState::searching;
    }
}

} // namespace

void StubNimController::initialise()
{
    statuses_.clear();
    for (int receiver = 1; receiver <= 4; ++receiver) {
        statuses_.push_back(makeReceiverStatus(ReceiverId{receiver}));
    }
}

void StubNimController::tune(ReceiverId receiver, const ScanTarget& target)
{
    auto it = std::ranges::find_if(statuses_, [receiver](const ReceiverStatus& status) {
        return status.receiver == receiver;
    });

    if (it == statuses_.end()) {
        statuses_.push_back(makeReceiverStatus(receiver));
        it = statuses_.end() - 1;
    }

    it->state = ReceiverState::searching;
    it->target = target;
    it->updatedAt = std::chrono::steady_clock::now();
}

void StubNimController::stop(ReceiverId receiver)
{
    auto it = std::ranges::find_if(statuses_, [receiver](const ReceiverStatus& status) {
        return status.receiver == receiver;
    });

    if (it != statuses_.end()) {
        it->state = ReceiverState::idle;
        it->target.reset();
        it->updatedAt = std::chrono::steady_clock::now();
    }
}

void StubNimController::shutdown()
{
    for (auto& status : statuses_) {
        status.state = ReceiverState::idle;
        status.target.reset();
        status.updatedAt = std::chrono::steady_clock::now();
    }
}

std::vector<ReceiverStatus> StubNimController::pollStatus()
{
    return statuses_;
}

std::vector<TransportPacket> StubNimController::drainTransportPackets(ReceiverId receiver, std::size_t maxPackets)
{
    (void)receiver;
    (void)maxPackets;
    return {};
}

class SeritNimController::Impl {
public:
    explicit Impl(SeritNimControllerConfig config)
        : config_{std::move(config)}
        , i2c_{config_.i2cDevice}
    {
    }

    ~Impl()
    {
        shutdown();
    }

    void initialise()
    {
        std::lock_guard i2cLock{sharedI2cBusMutex()};
        statuses_.clear();
        for (int receiver = 1; receiver <= 4; ++receiver) {
            statuses_.push_back(makeReceiverStatus(ReceiverId{receiver}));
        }
        {
            std::lock_guard lock{packetCountersMutex_};
            packetCounters_ = {};
        }
        {
            std::lock_guard lock{packetQueuesMutex_};
            for (auto& queue : packetQueues_) {
                queue.clear();
            }
        }

        configureWhDriver();
        startPacketReader();

        initialiseNim(nimDemodAddrA, 0);
        initialiseNim(nimDemodAddrB, 1);
        setRepeaters(-1);
    }

    void tune(ReceiverId receiver, const ScanTarget& target)
    {
        std::lock_guard i2cLock{sharedI2cBusMutex()};
        const auto hw = hardwareFor(receiver);
        auto& status = statusFor(receiver);
        status.target = target;
        status.state = ReceiverState::searching;
        status.updatedAt = std::chrono::steady_clock::now();
        status.merDb.reset();
        status.dNumberDb.reset();
        status.modulation.reset();

        if (!nimPresent_[hw.nimIndex]) {
            status.state = ReceiverState::fault;
            return;
        }

        const auto tunerKhz = tunerFrequencyKhz(target);
        try {
            setRepeaters(hw.nimIndex);
            configureTuner(hw, tunerKhz, target);
            setRepeaters(-1);

            setupDemodReceive(hw, target.symbolRateKs, false);
            writeDemod(hw.nimIndex, hw.nimAddress, dmdIStateRegister(hw.tuner), stv0910ScanBlindBestGuess);
        } catch (const std::exception& ex) {
            status.state = ReceiverState::fault;
            status.updatedAt = std::chrono::steady_clock::now();
            setRepeaters(-1);
            std::cerr << "wh-repeater: tune RX" << receiver.value << " failed: " << ex.what() << '\n';
        }
    }

    void stop(ReceiverId receiver)
    {
        std::lock_guard i2cLock{sharedI2cBusMutex()};
        stopUnlocked(receiver);
    }

    void stopUnlocked(ReceiverId receiver)
    {
        const auto hw = hardwareFor(receiver);
        auto& status = statusFor(receiver);

        if (nimPresent_[hw.nimIndex]) {
            setRepeaters(hw.nimIndex);
            configureTuner(hw, 0, status.target.value_or(ScanTarget{}));
            setRepeaters(-1);
            i2c_.writeReg16(hw.nimAddress, dmdIStateRegister(hw.tuner), 0x1c);
        }

        status.state = ReceiverState::idle;
        status.target.reset();
        status.updatedAt = std::chrono::steady_clock::now();
    }

    void shutdown()
    {
        std::lock_guard i2cLock{sharedI2cBusMutex()};
        for (int receiver = 1; receiver <= 4; ++receiver) {
            try {
                stopUnlocked(ReceiverId{receiver});
            } catch (const std::exception& ex) {
                std::cerr << "wh-repeater: stop RX" << receiver << " during NIM shutdown failed: " << ex.what() << '\n';
            }
        }
        try {
            setRepeaters(-1);
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: disable I2C repeaters during NIM shutdown failed: " << ex.what() << '\n';
        }
        stopPacketReader();
    }

    std::vector<ReceiverStatus> pollStatus()
    {
        std::lock_guard i2cLock{sharedI2cBusMutex()};
        for (auto& status : statuses_) {
            if (!status.target.has_value()) {
                continue;
            }

            const auto hw = hardwareFor(status.receiver);
            if (!nimPresent_[hw.nimIndex]) {
                status.state = ReceiverState::fault;
                status.updatedAt = std::chrono::steady_clock::now();
                continue;
            }

            const auto dmdState = i2c_.readReg16(hw.nimAddress, dmdStateRegister(hw.tuner));
            status.state = stateFromHeaderMode(dmdState);
            if (status.state == ReceiverState::lockedDvbs || status.state == ReceiverState::lockedDvbs2) {
                if (auto mer = readMerDb(hw); mer.has_value()) {
                    status.merDb = mer;
                }
            }

            const auto counters = countersFor(status.receiver);
            status.transportPackets = counters.transportPackets;
            status.continuityErrors = counters.syncErrors
                + counters.inSequenceErrors
                + counters.outSequenceErrors
                + counters.restartErrors
                + counters.overflowErrors;
            status.updatedAt = std::chrono::steady_clock::now();
        }

        return statuses_;
    }

    std::vector<TransportPacket> drainTransportPackets(ReceiverId receiver, std::size_t maxPackets)
    {
        const auto index = static_cast<std::size_t>(receiver.value - 1);
        if (index >= packetQueues_.size() || maxPackets == 0) {
            return {};
        }

        std::vector<TransportPacket> packets;
        packets.reserve(maxPackets);

        std::lock_guard lock{packetQueuesMutex_};
        auto& queue = packetQueues_[index];
        while (!queue.empty() && packets.size() < maxPackets) {
            packets.push_back(queue.front());
            queue.pop_front();
        }

        return packets;
    }

private:
    ReceiverStatus& statusFor(ReceiverId receiver)
    {
        auto it = std::ranges::find_if(statuses_, [receiver](const ReceiverStatus& status) {
            return status.receiver == receiver;
        });
        if (it == statuses_.end()) {
            statuses_.push_back(makeReceiverStatus(receiver));
            return statuses_.back();
        }
        return *it;
    }

    void configureWhDriver()
    {
        const auto spi0 = findInterruptNumber(config_.interruptsFile, "fe204000.spi");
        const auto spi5 = findInterruptNumber(config_.interruptsFile, "fe204a00.spi");
        const auto spi6 = findInterruptNumber(config_.interruptsFile, "fe204c00.spi");

        if (spi0 != 0 || spi6 != 0) {
            throw std::runtime_error{"SPI0/SPI6 Linux devices are enabled; whdriver expects those interrupts for PIC TS ingress"};
        }
        if (spi5 == 0) {
            throw std::runtime_error{"cannot find spi5 interrupt number in " + config_.interruptsFile.string()};
        }

        whDriver_ = FileDescriptor{checkedOpen(config_.whDriverDevice, O_RDWR | O_CLOEXEC | O_NONBLOCK)};
        configureWhDriverInterrupt(whDriver_.get(), spi5);

        whDriver_ = {};
        whDriver_ = FileDescriptor{checkedOpen(config_.whDriverDevice, O_RDWR | O_CLOEXEC | O_NONBLOCK)};
        configureWhDriverInterrupt(whDriver_.get(), spi5);
    }

    void startPacketReader()
    {
        packetReaderRunning_.store(true);
        packetReader_ = std::thread{[this] {
            packetReaderLoop();
        }};
    }

    void stopPacketReader()
    {
        packetReaderRunning_.store(false);
        if (packetReader_.joinable()) {
            packetReader_.join();
        }
    }

    void packetReaderLoop()
    {
        std::array<std::uint8_t, whDriverPacketSize> buffer{};
        while (packetReaderRunning_.load()) {
            const auto bytesRead = ::read(whDriver_.get(), buffer.data(), buffer.size());
            if (bytesRead == static_cast<ssize_t>(buffer.size())) {
                WhDriverPacket packet;
                std::copy_n(buffer.begin(), packet.ts.size(), packet.ts.begin());
                packet.status = littleEndianU32(buffer.data() + packet.ts.size());
                ingestPacket(packet);
                continue;
            }

            if (bytesRead < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    continue;
                }
                if (errno == EIO || errno == ENXIO) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    continue;
                }
                std::cerr << "wh-repeater: whdriver read failed: " << std::strerror(errno) << '\n';
            }
        }
    }

    void ingestPacket(const WhDriverPacket& packet)
    {
        const auto receiverIndex = static_cast<std::size_t>((packet.status >> 12) & 0x03);
        if (receiverIndex >= packetCounters_.size()) {
            return;
        }

        const auto valid = ((packet.status >> 8) & 0x01) != 0;
        const auto restart = ((packet.status >> 9) & 0x01) != 0;
        const auto overflow = ((packet.status >> 10) & 0x01) != 0;
        const auto nullPackets = static_cast<std::uint8_t>((packet.status >> 20) & 0x0f);
        const auto outSequence = static_cast<std::uint8_t>((packet.status >> 24) & 0x0f);
        const auto inSequence = static_cast<std::uint8_t>((packet.status >> 28) & 0x0f);
        const auto picIndex = receiverIndex & 0x02;

        std::lock_guard lock{packetCountersMutex_};
        auto& receiverCounters = packetCounters_[receiverIndex];
        receiverCounters.transportPackets += 1 + nullPackets;
        receiverCounters.nullPackets += nullPackets;

        if (packet.ts[0] != 0x47) {
            ++receiverCounters.syncErrors;
            return;
        }
        if (!valid) {
            return;
        }

        auto& picCounters = packetCounters_[picIndex];
        updateSequence(picCounters.hasOutSequence, picCounters.lastOutSequence, outSequence, picCounters.outSequenceErrors);
        updateSequence(receiverCounters.hasInSequence, receiverCounters.lastInSequence, inSequence, receiverCounters.inSequenceErrors);

        if (restart) {
            ++receiverCounters.restartErrors;
        }
        if (overflow) {
            ++receiverCounters.overflowErrors;
        }

        enqueuePacketLocked(receiverIndex, packet.ts);
    }

    void enqueuePacketLocked(std::size_t receiverIndex, const std::array<std::uint8_t, 188>& ts)
    {
        TransportPacket packet;
        std::ranges::transform(ts, packet.begin(), [](std::uint8_t value) {
            return static_cast<std::byte>(value);
        });

        std::lock_guard queueLock{packetQueuesMutex_};
        auto& queue = packetQueues_[receiverIndex];
        if (queue.size() >= maxQueuedPacketsPerReceiver) {
            queue.pop_front();
            ++packetCounters_[receiverIndex].overflowErrors;
        }
        queue.push_back(packet);
    }

    static void updateSequence(bool& hasPrevious, std::uint8_t& previous, std::uint8_t current, std::uint64_t& errors)
    {
        if (!hasPrevious) {
            previous = current;
            hasPrevious = true;
            return;
        }

        const auto delta = static_cast<std::uint8_t>((current - previous) & 0x0f);
        if (delta != 1) {
            ++errors;
        }
        previous = current;
    }

    PacketCounters countersFor(ReceiverId receiver)
    {
        const auto index = static_cast<std::size_t>(receiver.value - 1);
        if (index >= packetCounters_.size()) {
            return {};
        }

        std::lock_guard lock{packetCountersMutex_};
        return packetCounters_[index];
    }

    void initialiseNim(std::uint8_t nimAddress, int index)
    {
        try {
            setRepeaters(-1);
            i2c_.writeReg16(nimAddress, stv0910P1Vth34, 0xaa);
            const auto readBack = i2c_.readReg16(nimAddress, stv0910P1Vth34);
            if (readBack != 0xaa) {
                nimPresent_[index] = false;
                return;
            }

            nimPresent_[index] = true;
            const auto chipId = i2c_.readReg16(nimAddress, stv0910Mid);
            const auto deviceId = i2c_.readReg16(nimAddress, stv0910Did);
            std::cout << "NIM " << (index == 0 ? 'A' : 'B') << " STV0910 MID=0x"
                      << std::hex << static_cast<int>(chipId) << " DID=0x"
                      << static_cast<int>(deviceId) << std::dec << '\n';
            initialiseDemod(index, nimAddress);
        } catch (const std::exception& ex) {
            nimPresent_[index] = false;
            std::cerr << "wh-repeater: NIM " << (index == 0 ? 'A' : 'B')
                      << " not initialised: " << ex.what() << '\n';
        }
    }

    void setRepeaters(int enabledNim)
    {
        for (int index = 0; index < 2; ++index) {
            if (!nimPresent_[index] && enabledNim != -1) {
                continue;
            }

            const auto desired = index == enabledNim;
            if (repeaterOn_[index] == desired) {
                continue;
            }

            const auto addr = index == 0 ? nimDemodAddrA : nimDemodAddrB;
            i2c_.writeReg16(addr, stv0910P1I2cRpt, desired ? repeaterOn : repeaterOff);
            repeaterOn_[index] = desired;
        }
    }

    void initialiseDemod(int nimIndex, std::uint8_t nimAddress)
    {
        demodShadow_[nimIndex].clear();
        for (const auto& reg : stv0910InitRegisters) {
            writeDemod(nimIndex, nimAddress, reg.reg, reg.value);
        }
        writeDemod(nimIndex, nimAddress, stv0910TstRes0, 0x80);
        writeDemod(nimIndex, nimAddress, stv0910TstRes0, 0x00);
        setupDemodClocks(nimIndex, nimAddress);
    }

    void writeDemod(int nimIndex, std::uint8_t nimAddress, std::uint16_t reg, std::uint8_t value)
    {
        setRepeaters(-1);
        i2c_.writeReg16(nimAddress, reg, value);
        demodShadow_[nimIndex][reg] = value;
    }

    std::uint8_t readDemod(std::uint8_t nimAddress, std::uint16_t reg)
    {
        setRepeaters(-1);
        return i2c_.readReg16(nimAddress, reg);
    }

    void writeDemodField(int nimIndex, std::uint8_t nimAddress, std::uint32_t field, std::uint8_t value)
    {
        const auto reg = static_cast<std::uint16_t>(field >> 16);
        const auto mask = static_cast<std::uint8_t>(field & 0xff);
        const auto shift = static_cast<std::uint8_t>((field >> 12) & 0x0f);
        const auto current = demodShadow_[nimIndex][reg];
        const auto next = static_cast<std::uint8_t>((current & ~mask) | ((value << shift) & mask));
        writeDemod(nimIndex, nimAddress, reg, next);
    }

    std::uint8_t readDemodField(std::uint8_t nimAddress, std::uint32_t field)
    {
        const auto reg = static_cast<std::uint16_t>(field >> 16);
        const auto mask = static_cast<std::uint8_t>(field & 0xff);
        const auto shift = static_cast<std::uint8_t>((field >> 12) & 0x0f);
        return static_cast<std::uint8_t>((readDemod(nimAddress, reg) & mask) >> shift);
    }

    void setupDemodClocks(int nimIndex, std::uint8_t nimAddress)
    {
        constexpr std::uint8_t odf = 4;
        constexpr std::uint8_t idf = 1;
        const auto fXtalMhz = nimTunerXtalKhz / 1000;
        const auto fPhiMhz = nimDemodMclkHz / 1'000'000;
        const auto ndiv = static_cast<std::uint8_t>((fPhiMhz * odf * idf) / fXtalMhz);

        writeDemodField(nimIndex, nimAddress, stv0910FieldOdf, odf);
        writeDemodField(nimIndex, nimAddress, stv0910FieldIdf, idf);
        writeDemodField(nimIndex, nimAddress, stv0910FieldNDiv, ndiv);
        writeDemodField(nimIndex, nimAddress, stv0910FieldCp, 7);
        writeDemodField(nimIndex, nimAddress, stv0910FieldStandby, 0);
        writeDemodField(nimIndex, nimAddress, stv0910FieldBypassPllCore, 0);

        for (std::uint16_t timeout = 0; timeout < stv0910PllLockTimeout; ++timeout) {
            if (readDemodField(nimAddress, stv0910FieldPllLock) != 0) {
                return;
            }
        }

        throw std::runtime_error{"STV0910 PLL lock timed out"};
    }

    void setupDemodReceive(const ReceiverHardware& hw, std::uint32_t symbolRateKs, bool calibrated)
    {
        writeDemod(hw.nimIndex, hw.nimAddress, dmdIStateRegister(hw.tuner), 0x1c);
        setupCarrierLoop(hw, symbolRateKs, calibrated);
        setupTimingLoop(hw, symbolRateKs);
    }

    void setupCarrierLoop(const ReceiverHardware& hw, std::uint32_t symbolRateKs, bool calibrated)
    {
        writeDemod(hw.nimIndex, hw.nimAddress, cfrInit0Register(hw.tuner), 0x00);
        writeDemod(hw.nimIndex, hw.nimAddress, cfrInit1Register(hw.tuner), 0x00);

        auto limit = static_cast<std::int64_t>(calibrated ? symbolRateKs / 2 : 1000);
        limit = limit * 65536 / 135000;

        writeDemod(hw.nimIndex, hw.nimAddress, cfrUp0Register(hw.tuner), static_cast<std::uint8_t>(limit & 0xff));
        writeDemod(hw.nimIndex, hw.nimAddress, cfrUp1Register(hw.tuner), static_cast<std::uint8_t>((limit >> 8) & 0xff));

        const auto lowLimit = -limit;
        writeDemod(hw.nimIndex, hw.nimAddress, cfrLow0Register(hw.tuner), static_cast<std::uint8_t>(lowLimit & 0xff));
        writeDemod(hw.nimIndex, hw.nimAddress, cfrLow1Register(hw.tuner), static_cast<std::uint8_t>((lowLimit >> 8) & 0xff));

        auto dmdCfgMd = readDemod(hw.nimAddress, dmdCfgMdRegister(hw.tuner));
        if (symbolRateKs < 1000) {
            dmdCfgMd &= static_cast<std::uint8_t>(~0x40);
        } else {
            dmdCfgMd |= 0x40;
        }
        writeDemod(hw.nimIndex, hw.nimAddress, dmdCfgMdRegister(hw.tuner), dmdCfgMd);
    }

    void setupTimingLoop(const ReceiverHardware& hw, std::uint32_t symbolRateKs)
    {
        const auto srReg = static_cast<std::uint16_t>((symbolRateKs << 16) / 135 / 1000);
        writeDemod(hw.nimIndex, hw.nimAddress, sfrInit1Register(hw.tuner), static_cast<std::uint8_t>(srReg >> 8));
        writeDemod(hw.nimIndex, hw.nimAddress, sfrInit0Register(hw.tuner), static_cast<std::uint8_t>(srReg & 0xff));
    }

    void configureTuner(const ReceiverHardware& hw, std::uint32_t freqKhz, const ScanTarget& target)
    {
        const auto commonK = static_cast<std::uint8_t>(nimTunerXtalKhz / 1'000 - 16);
        rdiv_ = nimTunerXtalKhz >= stv6120RdivThresholdKhz ? 1 : 0;

        const auto ctrl1 = static_cast<std::uint8_t>((commonK << 3) | (rdiv_ << 2) | 1);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl1, ctrl1);

        auto calCtrl8 = i2c_.readReg8(nimTunerAddr, stv6120Ctrl8);
        calCtrl8 &= static_cast<std::uint8_t>(~0xe0);
        calCtrl8 |= static_cast<std::uint8_t>(1 << 6);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl8, calCtrl8);
        ctrl8_ = 0;

        auto calCtrl17 = i2c_.readReg8(nimTunerAddr, stv6120Ctrl17);
        calCtrl17 &= static_cast<std::uint8_t>(~0x20);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl17, calCtrl17);
        ctrl17_ = 0;

        const bool tuner1 = hw.tuner == 1;
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl2 : stv6120Ctrl11, static_cast<std::uint8_t>((1 << 5) | (1 << 4) | 0x03));

        auto& ctrlLowPass = tuner1 ? ctrl7_ : ctrl16_;
        ctrlLowPass = target.symbolRateKs == 27'500 || target.symbolRateKs == 22'000 ? 0x92 : 0x80;
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl7 : stv6120Ctrl16, ctrlLowPass);

        auto ctrl9 = i2c_.readReg8(nimTunerAddr, stv6120Ctrl9);
        auto ctrl10 = i2c_.readReg8(nimTunerAddr, stv6120Ctrl10);

        ctrl10 &= static_cast<std::uint8_t>(~((1 << 2) | (1 << 5)));
        const auto rfSel = target.antenna == Antenna::top ? 1 : 2;

        if (tuner1) {
            ctrl9 &= static_cast<std::uint8_t>(~0x03);
            ctrl9 |= static_cast<std::uint8_t>(rfSel);
            ctrl10 = freqKhz ? static_cast<std::uint8_t>(ctrl10 | (1 << 0)) : static_cast<std::uint8_t>(ctrl10 & ~(1 << 0));
        } else {
            ctrl9 &= static_cast<std::uint8_t>(~0x0c);
            ctrl9 |= static_cast<std::uint8_t>(rfSel << 2);
            ctrl10 = freqKhz ? static_cast<std::uint8_t>(ctrl10 | (1 << 1)) : static_cast<std::uint8_t>(ctrl10 & ~(1 << 1));
        }

        if (target.antenna == Antenna::top) {
            ctrl10 |= static_cast<std::uint8_t>(1 << 3);
        } else {
            ctrl10 |= static_cast<std::uint8_t>(1 << 4);
        }

        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl9, ctrl9);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl10, ctrl10);

        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl18, 0x00);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl19, 0x00);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl20, static_cast<std::uint8_t>((2 << 6) | 0x0c));
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl21, 0x00);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl22, 0x00);
        i2c_.writeReg8(nimTunerAddr, stv6120Ctrl23, static_cast<std::uint8_t>((2 << 6) | 0x0c));

        if (freqKhz > 0) {
            calibrateLowPass(hw.tuner);
            setTunerFrequency(hw.tuner, freqKhz);
        }
    }

    void calibrateLowPass(int tuner)
    {
        const bool tuner1 = tuner == 1;
        const auto ctrlReg = tuner1 ? stv6120Ctrl7 : stv6120Ctrl16;
        const auto statReg = tuner1 ? stv6120Stat1 : stv6120Stat2;
        const auto ctrlShadow = tuner1 ? ctrl7_ : ctrl16_;

        i2c_.writeReg8(nimTunerAddr, ctrlReg, static_cast<std::uint8_t>(ctrlShadow & ~(1 << 7)));
        i2c_.writeReg8(nimTunerAddr, statReg, static_cast<std::uint8_t>(1 << 1));
        waitForTunerBitClear(statReg, 1 << 1, "STV6120 low-pass calibration");
        i2c_.writeReg8(nimTunerAddr, ctrlReg, ctrlShadow);
    }

    void setTunerFrequency(int tuner, std::uint32_t freqKhz)
    {
        const auto p = dividerForFrequency(freqKhz);
        const auto fVcoKhz = freqKhz << (p + 1);
        const auto scaledVco = fVcoKhz << rdiv_;
        const auto n = static_cast<std::uint16_t>(scaledVco / nimTunerXtalKhz);
        const auto f = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(scaledVco % nimTunerXtalKhz) << 18) / nimTunerXtalKhz);
        const auto icp = chargePumpForVco(fVcoKhz);
        const auto cfhf = highPassFilterForFrequency(freqKhz);

        const bool tuner1 = tuner == 1;
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl3 : stv6120Ctrl12, static_cast<std::uint8_t>(n & 0x00ff));
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl4 : stv6120Ctrl13,
            static_cast<std::uint8_t>(((f & 0x0000007f) << 1) | ((n & 0x0100) >> 8)));
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl5 : stv6120Ctrl14, static_cast<std::uint8_t>((f & 0x00007f80) >> 7));
        i2c_.writeReg8(nimTunerAddr, tuner1 ? stv6120Ctrl6 : stv6120Ctrl15,
            static_cast<std::uint8_t>(((f & 0x00038000) >> 15) | (icp << 4) | 0x08));

        if (tuner1) {
            i2c_.writeReg8(nimTunerAddr, stv6120Ctrl7, static_cast<std::uint8_t>((p << 5) | ctrl7_));
            i2c_.writeReg8(nimTunerAddr, stv6120Ctrl8, static_cast<std::uint8_t>(cfhf | ctrl8_));
        } else {
            i2c_.writeReg8(nimTunerAddr, stv6120Ctrl16, static_cast<std::uint8_t>((p << 5) | ctrl16_));
            i2c_.writeReg8(nimTunerAddr, stv6120Ctrl17, static_cast<std::uint8_t>(cfhf | ctrl17_));
        }

        const auto statReg = tuner1 ? stv6120Stat1 : stv6120Stat2;
        i2c_.writeReg8(nimTunerAddr, statReg, static_cast<std::uint8_t>((1 << 2) | 0x08));
        waitForTunerBitClear(statReg, 1 << 2, "STV6120 VCO calibration");
        waitForTunerBitSet(statReg, 1 << 0, "STV6120 PLL lock");
    }

    void waitForTunerBitClear(std::uint8_t reg, std::uint8_t mask, std::string_view context)
    {
        for (std::uint16_t timeout = 0; timeout < stv6120CalTimeout; ++timeout) {
            if ((i2c_.readReg8(nimTunerAddr, reg) & mask) == 0) {
                return;
            }
        }
        throw std::runtime_error{std::string{context} + " timed out"};
    }

    void waitForTunerBitSet(std::uint8_t reg, std::uint8_t mask, std::string_view context)
    {
        for (std::uint16_t timeout = 0; timeout < stv6120CalTimeout; ++timeout) {
            if ((i2c_.readReg8(nimTunerAddr, reg) & mask) == mask) {
                return;
            }
        }
        throw std::runtime_error{std::string{context} + " timed out"};
    }

    static std::uint8_t dividerForFrequency(std::uint32_t freqKhz)
    {
        if (freqKhz <= stv6120PThreshold1Khz) {
            return 3;
        }
        if (freqKhz <= stv6120PThreshold2Khz) {
            return 2;
        }
        if (freqKhz <= stv6120PThreshold3Khz) {
            return 1;
        }
        return 0;
    }

    static std::uint8_t chargePumpForVco(std::uint32_t fVcoKhz)
    {
        auto it = std::ranges::find_if(stv6120IcpLookup, [fVcoKhz](const auto& row) {
            return fVcoKhz <= row[1];
        });
        if (it == stv6120IcpLookup.end()) {
            return static_cast<std::uint8_t>(stv6120IcpLookup.back()[2]);
        }
        return static_cast<std::uint8_t>((*it)[2]);
    }

    static std::uint8_t highPassFilterForFrequency(std::uint32_t freqKhz)
    {
        std::uint8_t cfhf = 0;
        while (cfhf < stv6120Cfhf.size() && ((3 * freqKhz / 1000) <= stv6120Cfhf[cfhf])) {
            ++cfhf;
        }
        return cfhf == 0 ? 0 : static_cast<std::uint8_t>(cfhf - 1);
    }

    std::uint32_t tunerFrequencyKhz(const ScanTarget& target) const
    {
        if (target.localOscillatorKhz == 0) {
            return target.frequencyKhz;
        }
        return target.frequencyKhz > target.localOscillatorKhz
            ? target.frequencyKhz - target.localOscillatorKhz
            : target.localOscillatorKhz - target.frequencyKhz;
    }

    std::optional<double> readMerDb(const ReceiverHardware& hw)
    {
        const auto high = i2c_.readReg16(hw.nimAddress, nosRamPosRegister(hw.tuner));
        const auto low = i2c_.readReg16(hw.nimAddress, nosRamValRegister(hw.tuner));
        if (((high >> 2) & 0x01) == 0) {
            return std::nullopt;
        }

        const auto raw = static_cast<std::uint16_t>(((high & 0x03) << 8) | low);
        if (raw == 0) {
            return std::nullopt;
        }

        // Same source registers as Winterhill. The exact LongMynd lookup/scale table still needs
        // porting; expose a monotonic placeholder so lock status can be observed during bring-up.
        return static_cast<double>(raw) / 10.0;
    }

    SeritNimControllerConfig config_;
    LinuxI2cBus i2c_;
    FileDescriptor whDriver_;
    std::array<bool, 2> nimPresent_{false, false};
    std::array<bool, 2> repeaterOn_{false, false};
    std::uint8_t rdiv_{};
    std::uint8_t ctrl7_{};
    std::uint8_t ctrl8_{};
    std::uint8_t ctrl16_{};
    std::uint8_t ctrl17_{};
    std::array<std::unordered_map<std::uint16_t, std::uint8_t>, 2> demodShadow_;
    std::array<PacketCounters, 4> packetCounters_;
    std::mutex packetCountersMutex_;
    std::array<std::deque<TransportPacket>, 4> packetQueues_;
    std::mutex packetQueuesMutex_;
    std::atomic_bool packetReaderRunning_{false};
    std::thread packetReader_;
    std::vector<ReceiverStatus> statuses_;
};

SeritNimController::SeritNimController(SeritNimControllerConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))}
{
}

SeritNimController::~SeritNimController() = default;

void SeritNimController::initialise()
{
    impl_->initialise();
}

void SeritNimController::tune(ReceiverId receiver, const ScanTarget& target)
{
    impl_->tune(receiver, target);
}

void SeritNimController::stop(ReceiverId receiver)
{
    impl_->stop(receiver);
}

void SeritNimController::shutdown()
{
    impl_->shutdown();
}

std::vector<ReceiverStatus> SeritNimController::pollStatus()
{
    return impl_->pollStatus();
}

std::vector<TransportPacket> SeritNimController::drainTransportPackets(ReceiverId receiver, std::size_t maxPackets)
{
    return impl_->drainTransportPackets(receiver, maxPackets);
}

} // namespace whrepeater
