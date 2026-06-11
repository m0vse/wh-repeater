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

#include <iostream>
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

    maybeStartDump();
    auto packets = nim.drainTransportPackets(active_->receiver, maxPacketsPerPump);
    for (const auto& packet : packets) {
        dumpPacket(std::span<const std::byte>{packet.data(), packet.size()});
        sink.write(std::span<const std::byte>{packet.data(), packet.size()});
    }
}

void TsRouter::maybeStartDump()
{
    if (dump_.is_open() || !std::filesystem::exists(dumpTrigger)) {
        return;
    }

    std::error_code error;
    std::filesystem::remove(dumpTrigger, error);
    dump_.open(dumpPath, std::ios::binary | std::ios::trunc);
    if (!dump_) {
        std::cerr << "wh-repeater: open RX TS dump failed: " << dumpPath << '\n';
        return;
    }
    dumpRemaining_ = dumpBytes;
    std::cout << "wh-repeater: dumping received TS to " << dumpPath
              << " (" << dumpBytes << " bytes)\n";
}

void TsRouter::dumpPacket(std::span<const std::byte> packet)
{
    if (!dump_.is_open()) {
        return;
    }

    const auto bytes = std::min(packet.size(), dumpRemaining_);
    dump_.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(bytes));
    dumpRemaining_ -= bytes;
    if (dumpRemaining_ == 0 || !dump_) {
        dump_.close();
        std::cout << "wh-repeater: received TS dump complete: " << dumpPath << '\n';
    }
}

} // namespace whrepeater
