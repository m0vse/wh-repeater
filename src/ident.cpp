/*
 * ============================================================================
 *  wh-repeater - Ident Helper Implementation
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements the minimal ident timer helper used while full transport-stream service-ident insertion remains a future expansion point.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

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
