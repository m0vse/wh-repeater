#pragma once

#include "whrepeater/types.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace whrepeater {

struct PlutoConfig {
    std::string address{"192.168.2.1"};
    std::uint16_t port{8282};
};

struct IdentConfig {
    bool enabled{true};
    std::string serviceName{"WH Repeater"};
    std::chrono::seconds interval{std::chrono::minutes{10}};
};

struct ReceiverConfig {
    ReceiverId receiver;
    std::vector<ScanTarget> targets;
    std::chrono::milliseconds dwellTime{1500};
};

struct RepeaterConfig {
    std::vector<ReceiverConfig> receivers;
    PlutoConfig pluto;
    IdentConfig ident;
    std::chrono::milliseconds statusInterval{500};
    double minimumMerDb{2.0};
    double minimumDNumberDb{0.0};
};

RepeaterConfig loadConfig(const std::filesystem::path& path);
RepeaterConfig defaultConfig();

} // namespace whrepeater
