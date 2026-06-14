/*
 * ============================================================================
 *  wh-repeater - GStreamer Backend Probe
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements optional GStreamer runtime probing so the media child can
 *    decide whether the experimental GStreamer backend is usable on this Pi.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/gstreamer_backend.hpp"

#include <cstdlib>
#include <sstream>
#include <string_view>

#if defined(WH_REPEATER_HAVE_GSTREAMER)
extern "C" {
#include <gst/gst.h>
}
#endif

namespace whrepeater {
namespace {

#if defined(WH_REPEATER_HAVE_GSTREAMER)
bool hasElement(std::string_view name)
{
    GstElementFactory* factory = gst_element_factory_find(std::string{name}.c_str());
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

bool hasAnyElement(std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        if (hasElement(name)) {
            return true;
        }
    }
    return false;
}
#endif

} // namespace

void configureGStreamerRuntime()
{
#if defined(WH_REPEATER_HAVE_GSTREAMER)
    (void)std::getenv("GST_REGISTRY");
#endif
}

GStreamerBackendStatus probeGStreamerBackend()
{
    GStreamerBackendStatus status;
#if defined(WH_REPEATER_HAVE_GSTREAMER)
    status.built = true;
    configureGStreamerRuntime();
    gst_init(nullptr, nullptr);

    status.mpegTsMux = hasElement("mpegtsmux");
    status.rtmpSink = hasAnyElement({"rtmp2sink", "rtmpsink"});
    status.runtimeAvailable = status.mpegTsMux && status.rtmpSink;

    std::ostringstream detail;
    detail << "GStreamer built in; mpegtsmux=" << (status.mpegTsMux ? "yes" : "no")
           << " rtmp=" << (status.rtmpSink ? "yes" : "no");
    status.detail = detail.str();
#else
    status.built = false;
    status.detail = "GStreamer support not built into this binary";
#endif
    return status;
}

} // namespace whrepeater
