#pragma once

#include "whrepeater/config.hpp"

namespace whrepeater {

class Daemon {
public:
    explicit Daemon(RepeaterConfig config);

    int run();

private:
    RepeaterConfig config_;
};

} // namespace whrepeater
