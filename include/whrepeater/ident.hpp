#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/types.hpp"

#include <chrono>
#include <optional>

namespace whrepeater {

class IdentInserter {
public:
    explicit IdentInserter(IdentConfig config);

    void update(std::optional<ActiveInput> input, std::chrono::steady_clock::time_point now);

private:
    IdentConfig config_;
    std::chrono::steady_clock::time_point nextIdent_{};
};

} // namespace whrepeater
