/*
 * ============================================================================
 *  wh-repeater - Transport Stream Router Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the transport-stream routing stage that accepts aligned
 *    188-byte TS packets and forwards selected input data toward the Pluto
 *    sink.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    static constexpr std::size_t maxPacketsPerPump{65536};
    static constexpr std::size_t dumpBytes{20 * 1024 * 1024};
    static inline const std::filesystem::path dumpTrigger{"/tmp/wh-repeater-dump-rx"};
    static inline const std::filesystem::path dumpPath{"/tmp/wh-repeater-rx.ts"};

    void maybeStartDump();
    void dumpPacket(std::span<const std::byte> packet);

    std::optional<ActiveInput> active_;
    std::ofstream dump_;
    std::size_t dumpRemaining_{};
};

} // namespace whrepeater
