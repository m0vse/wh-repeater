#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/nim_controller.hpp"

#include <chrono>
#include <unordered_map>

namespace whrepeater {

class ScanScheduler {
public:
    explicit ScanScheduler(std::vector<ReceiverConfig> receivers);

    void tick(NimController& nim, std::chrono::steady_clock::time_point now);

private:
    struct Cursor {
        std::size_t index{};
        std::chrono::steady_clock::time_point nextRetune{};
        bool tuned{};
    };

    std::vector<ReceiverConfig> receivers_;
    std::unordered_map<int, Cursor> cursors_;
};

} // namespace whrepeater
