#include "whrepeater/config.hpp"
#include "whrepeater/daemon.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        const auto configPath = argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{};
        auto config = configPath.empty() ? whrepeater::defaultConfig() : whrepeater::loadConfig(configPath);
        whrepeater::Daemon daemon{std::move(config)};
        return daemon.run();
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: " << ex.what() << '\n';
        return 1;
    }
}
