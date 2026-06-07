#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/ts_router.hpp"

#include <cstddef>
#include <span>

namespace whrepeater {

class PlutoSink final : public TsSink {
public:
    explicit PlutoSink(PlutoConfig config);

    void write(std::span<const std::byte> packet) override;

private:
    PlutoConfig config_;
};

} // namespace whrepeater
