#include "whrepeater/pluto_sink.hpp"

#include <utility>

namespace whrepeater {

PlutoSink::PlutoSink(PlutoConfig config)
    : config_{std::move(config)}
{
}

void PlutoSink::write(std::span<const std::byte> packet)
{
    (void)packet;
    // The first implementation target is UDP TS into the Pluto firmware ingest port.
    // Later this can become a libiio-backed sink if the selected firmware wants it.
}

} // namespace whrepeater
