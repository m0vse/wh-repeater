/*
 * ============================================================================
 *  wh-repeater - Shared I2C Bus Lock Implementation
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Defines the global mutex that prevents concurrent SD1 and Serit NIM
 *    operations from colliding on the Raspberry Pi I2C bus.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/i2c_bus_lock.hpp"

namespace whrepeater {

std::mutex& sharedI2cBusMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace whrepeater
