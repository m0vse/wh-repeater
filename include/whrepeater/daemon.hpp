/*
 * ============================================================================
 *  wh-repeater - Daemon Orchestrator Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the top-level daemon object that wires scanning, arbitration, hardware control, media generation, Pluto output, SD1 status, and the API server together.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/config.hpp"

#include <filesystem>

namespace whrepeater {

class Daemon {
public:
    explicit Daemon(RepeaterConfig config, std::filesystem::path configPath = {});

    int run();

private:
    RepeaterConfig config_;
    std::filesystem::path configPath_;
};

} // namespace whrepeater
