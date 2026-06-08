/*
 * ============================================================================
 *  wh-repeater - Hardware PTT Controller Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares optional GPIO push-to-talk output control for external RF
 *    amplifier or sequencer switching alongside Pluto transmit mute state.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"

namespace whrepeater {

class HardwarePtt {
public:
    explicit HardwarePtt(HardwarePttConfig config);
    ~HardwarePtt();

    HardwarePtt(const HardwarePtt&) = delete;
    HardwarePtt& operator=(const HardwarePtt&) = delete;

    HardwarePtt(HardwarePtt&& other) noexcept;
    HardwarePtt& operator=(HardwarePtt&& other) noexcept;

    void setTransmitEnabled(bool enabled);

private:
    void openLine();
    void setLine(bool active);
    void closeLine();

    HardwarePttConfig config_;
    int chipFd_{-1};
    int lineFd_{-1};
    bool lineOpen_{false};
    bool transmitEnabled_{false};
};

} // namespace whrepeater
