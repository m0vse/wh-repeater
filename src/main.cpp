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
