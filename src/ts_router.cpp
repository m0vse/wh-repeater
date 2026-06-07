#include "whrepeater/ts_router.hpp"

namespace whrepeater {

void TsRouter::select(std::optional<ActiveInput> input)
{
    active_ = std::move(input);
}

void TsRouter::pump(TsSink& sink)
{
    (void)sink;
    // The real router will read UDP TS packets from the active receiver's demod path
    // and forward 188-byte packets to the selected sink.
}

} // namespace whrepeater
