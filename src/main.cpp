/*
 * ============================================================================
 *  wh-repeater - Daemon Entry Point
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Loads configuration, selects the requested config path, constructs the daemon, and reports top-level startup/runtime failures.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/config.hpp"
#include "whrepeater/daemon.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        const auto explicitConfigPath = argc > 1;
        const auto configPath = explicitConfigPath ? std::filesystem::path{argv[1]} : std::filesystem::path{"wh-repeater.json"};
        auto config = explicitConfigPath || std::filesystem::exists(configPath)
            ? whrepeater::loadConfig(configPath)
            : whrepeater::defaultConfig();
        whrepeater::Daemon daemon{std::move(config), configPath};
        return daemon.run();
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: " << ex.what() << '\n';
        return 1;
    }
}
