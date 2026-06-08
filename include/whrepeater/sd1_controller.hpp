/*
 * ============================================================================
 *  wh-repeater - SD1 Analogue Receiver Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares direct PiVideo/I2C status polling for the Lintest Systems SD1
 *    analogue capture board and exposes it as receiver RX5.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace whrepeater {

class Sd1Controller {
public:
    explicit Sd1Controller(Sd1Config config);

    [[nodiscard]] AnalogueStatus poll();

private:
    [[nodiscard]] std::optional<std::uint8_t> readRegister(int fd, std::uint8_t reg, std::string& error) const;
    [[nodiscard]] bool writeRegister(int fd, std::uint8_t reg, std::uint8_t value, std::string& error) const;
    [[nodiscard]] static std::string sourceName(std::uint8_t value);
    [[nodiscard]] static std::string hexByte(std::uint8_t value);
    [[nodiscard]] static std::string hardwareId(const std::array<std::uint8_t, 8>& bytes);

    Sd1Config config_;
    std::optional<std::string> appliedSource_;
};

class Sd1StatusWorker {
public:
    explicit Sd1StatusWorker(Sd1Config config, std::chrono::milliseconds interval = std::chrono::seconds{2});
    ~Sd1StatusWorker();

    Sd1StatusWorker(const Sd1StatusWorker&) = delete;
    Sd1StatusWorker& operator=(const Sd1StatusWorker&) = delete;

    [[nodiscard]] AnalogueStatus snapshot() const;

private:
    void run();
    void stop();

    Sd1Controller controller_;
    std::chrono::milliseconds interval_;
    mutable std::mutex mutex_;
    AnalogueStatus latest_;
    int lockedSamples_{0};
    int unlockedSamples_{0};
    bool debouncedLocked_{false};
    std::atomic_bool stopping_{false};
    std::thread thread_;
};

} // namespace whrepeater
