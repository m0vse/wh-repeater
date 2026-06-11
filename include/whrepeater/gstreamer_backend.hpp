/*
 * ============================================================================
 *  wh-repeater - GStreamer Backend Probe Interface
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Declares the optional GStreamer media backend availability checks used
 *    before selecting experimental GStreamer encode/decode paths.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include <string>

namespace whrepeater {

struct GStreamerBackendStatus {
    bool built{};
    bool runtimeAvailable{};
    bool hardwareH264Encoder{};
    bool hardwareH264Decoder{};
    bool mpegTsMux{};
    bool rtmpSink{};
    std::string detail;
};

void configureGStreamerRuntime();
GStreamerBackendStatus probeGStreamerBackend();

} // namespace whrepeater
