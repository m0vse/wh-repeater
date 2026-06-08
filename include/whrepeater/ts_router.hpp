#pragma once

#include "whrepeater/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace whrepeater {

class NimController;

class TsSink {
public:
    virtual ~TsSink() = default;
    virtual void write(std::span<const std::byte> packet) = 0;
};

class TsRouter {
public:
    void select(std::optional<ActiveInput> input);
    void pump(NimController& nim, TsSink& sink);

private:
    static constexpr std::size_t maxPacketsPerPump{256};

    std::optional<ActiveInput> active_;
};

} // namespace whrepeater
