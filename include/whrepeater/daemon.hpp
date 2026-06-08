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
