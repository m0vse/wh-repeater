/*
 * ============================================================================
 *  wh-repeater - Transport Stream Router
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements aligned MPEG-TS packet routing from the selected input path
 *    toward the configured Pluto sink.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/ts_router.hpp"

#include "whrepeater/nim_controller.hpp"

#include <span>

namespace whrepeater {

void TsRouter::select(std::optional<ActiveInput> input)
{
    active_ = std::move(input);
}

void TsRouter::pump(NimController& nim, TsSink& sink)
{
    if (!active_.has_value()) {
        return;
    }

    auto packets = nim.drainTransportPackets(active_->receiver, maxPacketsPerPump);
    for (const auto& packet : packets) {
        sink.write(std::span<const std::byte>{packet.data(), packet.size()});
    }
}

} // namespace whrepeater
