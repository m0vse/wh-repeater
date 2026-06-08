/*
 * ============================================================================
 *  wh-repeater - SD1 Analogue Receiver Controller
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements direct I2C/PiVideo register access for SD1 detection and
 *    lock reporting, including debouncing and background status polling.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/sd1_controller.hpp"

#include "whrepeater/i2c_bus_lock.hpp"

#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

constexpr std::array<std::string_view, 9> sourceMap{
    "auto",
    "video1",
    "video2",
    "video3",
    "svideo",
    "component",
    "component",
    "component",
    "hdmi",
};
constexpr int lockSamplesRequired{3};
constexpr int unlockSamplesRequired{2};

std::optional<std::uint8_t> sourceValue(std::string_view source)
{
    for (std::size_t index = 0; index < sourceMap.size(); ++index) {
        if (sourceMap[index] == source) {
            return static_cast<std::uint8_t>(index);
        }
    }
    return std::nullopt;
}

class FileDescriptor {
public:
    explicit FileDescriptor(const char* path)
        : fd_{::open(path, O_RDWR | O_CLOEXEC)}
    {
    }

    ~FileDescriptor()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const
    {
        return fd_;
    }

private:
    int fd_{-1};
};

std::string systemError(std::string_view action)
{
    return std::string{action} + ": " + std::strerror(errno);
}

} // namespace

Sd1Controller::Sd1Controller(Sd1Config config)
    : config_{std::move(config)}
{
}

AnalogueStatus Sd1Controller::poll()
{
    std::lock_guard i2cLock{sharedI2cBusMutex()};
    AnalogueStatus status;
    status.enabled = config_.enabled;
    status.updatedAt = std::chrono::steady_clock::now();
    if (!config_.enabled) {
        status.error = "disabled";
        return status;
    }

    FileDescriptor fd{config_.i2cDevice.c_str()};
    if (fd.get() < 0) {
        status.error = systemError("open " + config_.i2cDevice);
        return status;
    }

    if (::ioctl(fd.get(), I2C_SLAVE, config_.i2cAddress) < 0) {
        status.error = systemError("select SD1 I2C address");
        return status;
    }

    std::string error;
    const auto reg3 = readRegister(fd.get(), 3, error);
    if (!reg3.has_value()) {
        status.error = "SD1 not detected at " + config_.i2cDevice + " address 0x" + hexByte(config_.i2cAddress);
        return status;
    }
    status.present = true;
    status.model = ((*reg3 & 0x40U) == 0U) ? "SD1" : "HD1";

    if (status.model == "SD1" && !config_.source.empty() && appliedSource_ != config_.source) {
        if (const auto value = sourceValue(config_.source); value.has_value()) {
            if (writeRegister(fd.get(), 6, *value, error)) {
                appliedSource_ = config_.source;
            } else {
                status.error = error;
            }
        } else {
            status.error = "unsupported SD1 source: " + config_.source;
        }
    }

    const auto reg4 = readRegister(fd.get(), 4, error);
    const auto reg5 = readRegister(fd.get(), 5, error);
    const auto selected = readRegister(fd.get(), 6, error);
    const auto ready = readRegister(fd.get(), 8, error);
    const auto lock = readRegister(fd.get(), 9, error);
    const auto active = readRegister(fd.get(), 11, error);
    const auto running = readRegister(fd.get(), 12, error);
    if (!reg4 || !reg5 || !selected || !ready || !lock || !active || !running) {
        status.error = error;
        return status;
    }

    status.ready = *ready != 0;
    status.cameraRunning = *running != 0;
    status.rawLock = *lock;
    status.locked = status.model == "SD1" ? (*lock == 1 || *lock == 2) : (*lock >= 1 && *lock <= 3);
    status.firmwareVersion = hexByte(*reg4 & 0x7FU);
    status.longVersion = hexByte(*reg4) + hexByte(*reg3) + hexByte(*reg5);
    status.selectedSource = sourceName(*selected);
    status.activeSource = sourceName(*active);

    std::array<std::uint8_t, 8> idBytes{};
    for (std::uint8_t index = 0; index < idBytes.size(); ++index) {
        const auto byte = readRegister(fd.get(), static_cast<std::uint8_t>(30 + index), error);
        if (!byte.has_value()) {
            status.error = error;
            return status;
        }
        idBytes[index] = *byte;
    }
    status.hardwareId = hardwareId(idBytes);
    return status;
}

std::optional<std::uint8_t> Sd1Controller::readRegister(int fd, std::uint8_t reg, std::string& error) const
{
    std::uint8_t value{};
    i2c_msg messages[2]{
        i2c_msg{.addr = config_.i2cAddress, .flags = 0, .len = 1, .buf = &reg},
        i2c_msg{.addr = config_.i2cAddress, .flags = I2C_M_RD, .len = 1, .buf = &value},
    };
    i2c_rdwr_ioctl_data transfer{.msgs = messages, .nmsgs = 2};

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (::ioctl(fd, I2C_RDWR, &transfer) >= 0) {
            return value;
        }
        ::usleep(25000);
    }

    error = systemError("read SD1 register " + std::to_string(reg));
    return std::nullopt;
}

bool Sd1Controller::writeRegister(int fd, std::uint8_t reg, std::uint8_t value, std::string& error) const
{
    std::array<std::uint8_t, 2> buffer{reg, value};
    i2c_msg message{.addr = config_.i2cAddress, .flags = 0, .len = buffer.size(), .buf = buffer.data()};
    i2c_rdwr_ioctl_data transfer{.msgs = &message, .nmsgs = 1};

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (::ioctl(fd, I2C_RDWR, &transfer) >= 0) {
            return true;
        }
        ::usleep(25000);
    }

    error = systemError("write SD1 register " + std::to_string(reg));
    return false;
}

std::string Sd1Controller::sourceName(std::uint8_t value)
{
    if (value < sourceMap.size()) {
        return std::string{sourceMap[value]};
    }
    return "unknown";
}

std::string Sd1Controller::hexByte(std::uint8_t value)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

std::string Sd1Controller::hardwareId(const std::array<std::uint8_t, 8>& bytes)
{
    std::string id;
    id.reserve(16);
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        id += hexByte(*it);
    }
    return id;
}

Sd1StatusWorker::Sd1StatusWorker(Sd1Config config, std::chrono::milliseconds interval)
    : controller_{std::move(config)}
    , interval_{interval}
{
    latest_.updatedAt = std::chrono::steady_clock::now();
    thread_ = std::thread{&Sd1StatusWorker::run, this};
}

Sd1StatusWorker::~Sd1StatusWorker()
{
    stop();
}

AnalogueStatus Sd1StatusWorker::snapshot() const
{
    std::lock_guard lock{mutex_};
    return latest_;
}

void Sd1StatusWorker::run()
{
    while (!stopping_.load()) {
        auto status = controller_.poll();
        if (!status.enabled || !status.present || !status.ready || status.error.has_value()) {
            debouncedLocked_ = false;
            lockedSamples_ = 0;
            unlockedSamples_ = 0;
            status.locked = false;
        } else if (status.locked) {
            ++lockedSamples_;
            unlockedSamples_ = 0;
            if (lockedSamples_ >= lockSamplesRequired) {
                debouncedLocked_ = true;
            }
            status.locked = debouncedLocked_;
        } else {
            ++unlockedSamples_;
            lockedSamples_ = 0;
            if (unlockedSamples_ >= unlockSamplesRequired) {
                debouncedLocked_ = false;
            }
            status.locked = debouncedLocked_;
        }
        {
            std::lock_guard lock{mutex_};
            latest_ = std::move(status);
        }

        const auto sleepStep = std::chrono::milliseconds{100};
        auto slept = std::chrono::milliseconds{0};
        while (!stopping_.load() && slept < interval_) {
            std::this_thread::sleep_for(sleepStep);
            slept += sleepStep;
        }
    }
}

void Sd1StatusWorker::stop()
{
    stopping_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace whrepeater
