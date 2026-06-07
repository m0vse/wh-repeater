#include "whrepeater/ident.hpp"

#include <utility>

namespace whrepeater {

IdentInserter::IdentInserter(IdentConfig config)
    : config_{std::move(config)}
{
}

void IdentInserter::update(std::optional<ActiveInput> input, std::chrono::steady_clock::time_point now)
{
    if (!config_.enabled || !input.has_value()) {
        return;
    }

    if (nextIdent_ == std::chrono::steady_clock::time_point{} || now >= nextIdent_) {
        nextIdent_ = now + config_.interval;
        // Initial ident support will inject TS metadata. Full video overlay belongs in
        // a decode/overlay/re-encode pipeline and should be an explicit later module.
    }
}

} // namespace whrepeater
