/*
 * ============================================================================
 *  wh-repeater - Shared I2C Bus Lock
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the process-wide mutex used to serialize NIM and SD1 I2C transactions during polling, tuning, config reload, and service teardown.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include <mutex>

namespace whrepeater {

std::mutex& sharedI2cBusMutex();

} // namespace whrepeater
