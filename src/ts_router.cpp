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
