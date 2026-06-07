#pragma once

#include "whrepeater/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace whrepeater {

class TsSink {
public:
    virtual ~TsSink() = default;
    virtual void write(std::span<const std::byte> packet) = 0;
};

class TsRouter {
public:
    void select(std::optional<ActiveInput> input);
    void pump(TsSink& sink);

private:
    std::optional<ActiveInput> active_;
};

} // namespace whrepeater
