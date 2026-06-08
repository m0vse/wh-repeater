/*
 * ============================================================================
 *  wh-repeater - Hardware PTT Controller
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements optional GPIO character-device output control for external
 *    amplifier or sequencer PTT, with fail-safe inactive output on shutdown.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/hardware_ptt.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/gpio.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace whrepeater {
namespace {

void closeFd(int& fd, std::string_view label)
{
    if (fd >= 0) {
        if (::close(fd) != 0) {
            std::cerr << "wh-repeater: close " << label << " failed: " << std::strerror(errno) << '\n';
        }
        fd = -1;
    }
}

} // namespace

HardwarePtt::HardwarePtt(HardwarePttConfig config)
    : config_{std::move(config)}
{
    if (config_.enabled) {
        try {
            openLine();
            setLine(false);
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: hardware PTT disabled: " << ex.what() << '\n';
            closeLine();
        }
    }
}

HardwarePtt::~HardwarePtt()
{
    closeLine();
}

HardwarePtt::HardwarePtt(HardwarePtt&& other) noexcept
    : config_{std::move(other.config_)}
    , chipFd_{std::exchange(other.chipFd_, -1)}
    , lineFd_{std::exchange(other.lineFd_, -1)}
    , lineOpen_{std::exchange(other.lineOpen_, false)}
    , transmitEnabled_{std::exchange(other.transmitEnabled_, false)}
{
}

HardwarePtt& HardwarePtt::operator=(HardwarePtt&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    closeLine();
    config_ = std::move(other.config_);
    chipFd_ = std::exchange(other.chipFd_, -1);
    lineFd_ = std::exchange(other.lineFd_, -1);
    lineOpen_ = std::exchange(other.lineOpen_, false);
    transmitEnabled_ = std::exchange(other.transmitEnabled_, false);
    return *this;
}

void HardwarePtt::setTransmitEnabled(bool enabled)
{
    if (!config_.enabled || !lineOpen_ || transmitEnabled_ == enabled) {
        return;
    }

    try {
        setLine(enabled);
        transmitEnabled_ = enabled;
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: hardware PTT update failed: " << ex.what() << '\n';
        closeLine();
    }
}

void HardwarePtt::openLine()
{
    chipFd_ = ::open(config_.chip.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd_ < 0) {
        throw std::runtime_error{"open " + config_.chip + ": " + std::strerror(errno)};
    }

    gpio_v2_line_request request{};
    request.offsets[0] = config_.line;
    request.num_lines = 1;
    std::snprintf(request.consumer, sizeof(request.consumer), "wh-repeater-ptt");
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = config_.activeHigh ? 0 : 1;
    request.config.attrs[0].mask = 1;

    if (::ioctl(chipFd_, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        throw std::runtime_error{"request GPIO line " + std::to_string(config_.line) + ": " + std::strerror(errno)};
    }

    lineFd_ = request.fd;
    lineOpen_ = true;
}

void HardwarePtt::setLine(bool active)
{
    if (!lineOpen_) {
        return;
    }

    const bool physicalHigh = config_.activeHigh ? active : !active;
    gpio_v2_line_values values{};
    values.mask = 1;
    values.bits = physicalHigh ? 1 : 0;
    if (::ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        throw std::runtime_error{"set GPIO line " + std::to_string(config_.line) + ": " + std::strerror(errno)};
    }
}

void HardwarePtt::closeLine()
{
    if (lineOpen_) {
        try {
            setLine(false);
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: hardware PTT deassert failed: " << ex.what() << '\n';
        }
    }
    lineOpen_ = false;
    transmitEnabled_ = false;
    closeFd(lineFd_, "hardware PTT line");
    closeFd(chipFd_, "hardware PTT chip");
}

} // namespace whrepeater
