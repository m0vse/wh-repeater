/*
 * ============================================================================
 *  wh-repeater - Media Pipeline Implementation
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Builds the continuous generated MPEG-TS output, renders
 *    fallback/sleep/access slides, selects H.264 encoders, and optionally
 *    streams to RTMP.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/media_pipeline.hpp"

#include "whrepeater/gstreamer_backend.hpp"
#include "whrepeater/media_timing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <csignal>
#include <ctime>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(WH_REPEATER_HAVE_GSTREAMER)
extern "C" {
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
}
#endif

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace whrepeater {
namespace {

constexpr int minimumOutputWidth{320};
constexpr int minimumOutputHeight{240};
constexpr std::string_view slateFont{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
constexpr std::string_view clockFont{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"};
constexpr auto liveVideoAcquisitionTimeout = std::chrono::seconds{8};
constexpr double pi{3.14159265358979323846};

int outputWidth(const RepeaterConfig& config)
{
    return std::max(minimumOutputWidth, static_cast<int>(config.pluto.outputWidth) & ~1);
}

int outputHeight(const RepeaterConfig& config)
{
    return std::max(minimumOutputHeight, static_cast<int>(config.pluto.outputHeight) & ~1);
}

int outputFrameRate(const RepeaterConfig& config)
{
    return clampedOutputFrameRate(config.pluto.outputFrameRate);
}

int outputAudioChannels(const RepeaterConfig& config)
{
    return clampedOutputAudioChannels(config.pluto.outputAudioChannels);
}

std::string h264ProfileName(const RepeaterConfig& config)
{
    if (config.pluto.h264Profile == "baseline" || config.pluto.h264Profile == "high") {
        return config.pluto.h264Profile;
    }
    return "main";
}

std::string h264LevelName(const RepeaterConfig& config)
{
    if (config.pluto.h264Level == "3" || config.pluto.h264Level == "3.1" || config.pluto.h264Level == "4") {
        return config.pluto.h264Level;
    }
    if (outputHeight(config) > 720 || outputWidth(config) > 1280) {
        return "4";
    }
    return "3.1";
}

int h264AvProfile(const RepeaterConfig& config)
{
    const auto profile = h264ProfileName(config);
    if (profile == "baseline") {
        return FF_PROFILE_H264_BASELINE;
    }
    if (profile == "high") {
        return FF_PROFILE_H264_HIGH;
    }
    return FF_PROFILE_H264_MAIN;
}

int h264AvLevel(const RepeaterConfig& config)
{
    const auto level = h264LevelName(config);
    if (level == "3") {
        return 30;
    }
    if (level == "3.1") {
        return 31;
    }
    return 40;
}

int fallbackVideoBitrateKbps(const RepeaterConfig& config)
{
    const auto muxBudget = static_cast<int>(config.pluto.muxRateKbps * 6 / 10);
    return std::max(250, std::min(static_cast<int>(config.pluto.videoBitrateKbps), muxBudget));
}

std::string jsonString(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                out += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[(byte >> 4) & 0x0f]);
                out.push_back(hex[byte & 0x0f]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
    return out;
}

std::string formatTimecode(std::chrono::milliseconds position)
{
    auto totalMs = std::max<std::int64_t>(0, position.count());
    const auto hours = totalMs / 3'600'000;
    totalMs %= 3'600'000;
    const auto minutes = totalMs / 60'000;
    totalMs %= 60'000;
    const auto seconds = totalMs / 1000;
    const auto millis = totalMs % 1000;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds << '.'
        << std::setw(3) << millis;
    return out.str();
}

std::string avError(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

void checkAv(int status, std::string_view operation)
{
    if (status < 0) {
        throw std::runtime_error{std::string{operation} + ": " + avError(status)};
    }
}

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct AvFilterGraphDeleter {
    void operator()(AVFilterGraph* graph) const
    {
        avfilter_graph_free(&graph);
    }
};

struct AvFilterInOutDeleter {
    void operator()(AVFilterInOut* endpoint) const
    {
        avfilter_inout_free(&endpoint);
    }
};

using PacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct EncoderOpenResult {
    AVCodecContext* context{};
    const AVCodec* encoder{};
    std::string name;
};

bool isVaapiEncoder(std::string_view encoderName)
{
    return encoderName == "h264_vaapi";
}

AVPixelFormat softwarePixelFormatForEncoder(const AVCodecContext& codec)
{
    return codec.pix_fmt == AV_PIX_FMT_VAAPI ? AV_PIX_FMT_YUV420P : codec.pix_fmt;
}

void fillBlack(AVFrame& frame)
{
    for (int y = 0; y < frame.height; ++y) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, 24);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
        std::fill_n(frame.data[1] + y * frame.linesize[1], frame.width / 2, 128);
        std::fill_n(frame.data[2] + y * frame.linesize[2], frame.width / 2, 128);
    }
}

void fillBlue(AVFrame& frame)
{
    for (int y = 0; y < frame.height; ++y) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, 41);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
        std::fill_n(frame.data[1] + y * frame.linesize[1], frame.width / 2, 240);
        std::fill_n(frame.data[2] + y * frame.linesize[2], frame.width / 2, 110);
    }
}

void normaliseVideoFrameProperties(AVFrame& frame)
{
    frame.pict_type = AV_PICTURE_TYPE_NONE;
    frame.flags &= ~AV_FRAME_FLAG_KEY;
    frame.sample_aspect_ratio = AVRational{1, 1};
    frame.color_range = AVCOL_RANGE_UNSPECIFIED;
    frame.colorspace = AVCOL_SPC_UNSPECIFIED;
    frame.color_primaries = AVCOL_PRI_UNSPECIFIED;
    frame.color_trc = AVCOL_TRC_UNSPECIFIED;
    frame.chroma_location = AVCHROMA_LOC_UNSPECIFIED;
}

struct YuvColor {
    std::uint8_t y;
    std::uint8_t u;
    std::uint8_t v;
};

void fillRect(AVFrame& frame, int x0, int y0, int width, int height, YuvColor color)
{
    const int x1 = std::clamp(x0 + width, 0, frame.width);
    const int y1 = std::clamp(y0 + height, 0, frame.height);
    x0 = std::clamp(x0, 0, frame.width);
    y0 = std::clamp(y0, 0, frame.height);

    for (int y = y0; y < y1; ++y) {
        std::fill(frame.data[0] + y * frame.linesize[0] + x0,
                  frame.data[0] + y * frame.linesize[0] + x1,
                  color.y);
    }

    const int cx0 = x0 / 2;
    const int cx1 = (x1 + 1) / 2;
    const int cy0 = y0 / 2;
    const int cy1 = (y1 + 1) / 2;
    for (int y = cy0; y < cy1; ++y) {
        std::fill(frame.data[1] + y * frame.linesize[1] + cx0,
                  frame.data[1] + y * frame.linesize[1] + cx1,
                  color.u);
        std::fill(frame.data[2] + y * frame.linesize[2] + cx0,
                  frame.data[2] + y * frame.linesize[2] + cx1,
                  color.v);
    }
}

void fillTestcard(AVFrame& frame)
{
    constexpr YuvColor white{235, 128, 128};
    constexpr YuvColor yellow{210, 16, 146};
    constexpr YuvColor cyan{170, 166, 16};
    constexpr YuvColor green{145, 54, 34};
    constexpr YuvColor magenta{107, 202, 222};
    constexpr YuvColor red{81, 90, 240};
    constexpr YuvColor blue{41, 240, 110};
    constexpr YuvColor black{16, 128, 128};
    constexpr YuvColor negI{16, 198, 21};
    constexpr YuvColor posQ{16, 98, 235};
    constexpr YuvColor plugeDark{7, 128, 128};
    constexpr YuvColor plugeDim{12, 128, 128};
    constexpr YuvColor plugeBright{24, 128, 128};

    fillRect(frame, 0, 0, frame.width, frame.height, black);

    constexpr std::array<YuvColor, 7> topBars{{
        white, yellow, cyan, green, magenta, red, blue,
    }};
    constexpr std::array<YuvColor, 7> middleBars{{
        blue, black, magenta, black, cyan, black, white,
    }};

    const int topHeight = frame.height * 3 / 4;
    const int middleHeight = frame.height / 12;
    const int bottomY = topHeight + middleHeight;
    const int bottomHeight = frame.height - bottomY;
    const int barWidth = frame.width / static_cast<int>(topBars.size());

    for (std::size_t index = 0; index < topBars.size(); ++index) {
        const int x = static_cast<int>(index) * barWidth;
        const int width = index + 1 == topBars.size() ? frame.width - x : barWidth;
        fillRect(frame, x, 0, width, topHeight, topBars[index]);
    }

    for (std::size_t index = 0; index < middleBars.size(); ++index) {
        const int x = static_cast<int>(index) * barWidth;
        const int width = index + 1 == middleBars.size() ? frame.width - x : barWidth;
        fillRect(frame, x, topHeight, width, middleHeight, middleBars[index]);
    }

    const int bottomBlock = frame.width / 12;
    fillRect(frame, 0, bottomY, bottomBlock * 2, bottomHeight, negI);
    fillRect(frame, bottomBlock * 2, bottomY, bottomBlock * 2, bottomHeight, white);
    fillRect(frame, bottomBlock * 4, bottomY, bottomBlock * 2, bottomHeight, posQ);
    fillRect(frame, bottomBlock * 6, bottomY, bottomBlock * 2, bottomHeight, black);
    fillRect(frame, bottomBlock * 8, bottomY, bottomBlock, bottomHeight, plugeDark);
    fillRect(frame, bottomBlock * 9, bottomY, bottomBlock, bottomHeight, plugeDim);
    fillRect(frame, bottomBlock * 10, bottomY, bottomBlock, bottomHeight, plugeBright);
    fillRect(frame, bottomBlock * 11, bottomY, frame.width - bottomBlock * 11, bottomHeight, black);
}

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const auto ch : text) {
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

std::string escapeDrawText(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const auto ch : text) {
        switch (ch) {
        case '\\':
        case '\'':
        case ':':
        case ',':
            escaped.push_back('\\');
            break;
        default:
            break;
        }
        escaped.push_back(ch);
    }
    return escaped;
}

FramePtr allocateVideoFrame(const RepeaterConfig& config)
{
    FramePtr frame{av_frame_alloc()};
    if (!frame) {
        throw std::runtime_error{"allocate fallback frame failed"};
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = outputWidth(config);
    frame->height = outputHeight(config);
    checkAv(av_frame_get_buffer(frame.get(), 32), "allocate fallback frame buffer");
    checkAv(av_frame_make_writable(frame.get()), "make fallback frame writable");
    return frame;
}

std::string slateFilter(std::string_view text)
{
    const auto lines = splitLines(text);
    std::ostringstream filter;
    filter << "drawbox=x=90:y=82:w=iw-180:h=ih-164:color=black@0.62:t=fill"
           << ",drawbox=x=90:y=82:w=iw-180:h=7:color=0xf4d35e@0.90:t=fill"
           << ",drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(lines.empty() ? "" : lines[0])
           << "':x=(w-text_w)/2:y=126:fontsize=76:fontcolor=white";
    constexpr std::array<int, 7> bodyY{256, 336, 400, 456, 512, 568, 624};
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto y = index - 1 < bodyY.size() ? bodyY[index - 1] : 624 + static_cast<int>(index - bodyY.size()) * 48;
        const auto fontSize = index == 1 ? 46 : 31;
        const auto color = index == 1 ? "0xf4d35e" : "white";
        filter << ",drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(lines[index])
               << "':x=(w-text_w)/2:y=" << y << ":fontsize=" << fontSize << ":fontcolor=" << color;
    }
    return filter.str();
}

FramePtr renderFilteredFrame(const RepeaterConfig& config, std::string_view filter, int frameRate, void (*fill)(AVFrame&))
{
    auto inputFrame = allocateVideoFrame(config);
    fill(*inputFrame);

    AVFilterGraph* rawGraph = avfilter_graph_alloc();
    if (rawGraph == nullptr) {
        throw std::runtime_error{"allocate video filter graph failed"};
    }
    std::unique_ptr<AVFilterGraph, AvFilterGraphDeleter> graph{rawGraph};

    AVFilterContext* source{};
    AVFilterContext* sink{};
    const auto sourceArgs = "video_size=" + std::to_string(inputFrame->width) + "x" + std::to_string(inputFrame->height)
        + ":pix_fmt=" + std::to_string(AV_PIX_FMT_YUV420P)
        + ":time_base=1/" + std::to_string(frameRate)
        + ":pixel_aspect=1/1";
    checkAv(avfilter_graph_create_filter(&source,
                                          avfilter_get_by_name("buffer"),
                                          "video_source",
                                          sourceArgs.c_str(),
                                          nullptr,
                                          graph.get()),
            "create video buffer source");
    checkAv(avfilter_graph_create_filter(&sink,
                                          avfilter_get_by_name("buffersink"),
                                          "video_sink",
                                          nullptr,
                                          nullptr,
                                          graph.get()),
            "create video buffer sink");

    AVFilterInOut* rawInputs = avfilter_inout_alloc();
    AVFilterInOut* rawOutputs = avfilter_inout_alloc();
    if (rawInputs == nullptr || rawOutputs == nullptr) {
        avfilter_inout_free(&rawInputs);
        avfilter_inout_free(&rawOutputs);
        throw std::runtime_error{"allocate video filter graph endpoints failed"};
    }
    std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> inputs{rawInputs};
    std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> outputs{rawOutputs};
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    AVFilterInOut* inputPtr = inputs.release();
    AVFilterInOut* outputPtr = outputs.release();
    const auto parseStatus = avfilter_graph_parse_ptr(graph.get(), std::string{filter}.c_str(), &inputPtr, &outputPtr, nullptr);
    avfilter_inout_free(&inputPtr);
    avfilter_inout_free(&outputPtr);
    checkAv(parseStatus, "parse video drawtext filter");
    checkAv(avfilter_graph_config(graph.get(), nullptr), "configure video drawtext filter");
    checkAv(av_buffersrc_add_frame_flags(source, inputFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF), "send frame to filter");

    FramePtr outputFrame{av_frame_alloc()};
    if (!outputFrame) {
        throw std::runtime_error{"allocate rendered video frame failed"};
    }
    checkAv(av_buffersink_get_frame(sink, outputFrame.get()), "receive rendered video frame");
    return outputFrame;
}

FramePtr renderSlateFrame(const RepeaterConfig& config, std::string_view text, int frameRate)
{
    return renderFilteredFrame(config, slateFilter(text), frameRate, fillBlue);
}

std::string repeaterName(const RepeaterConfig& config)
{
    if (!config.pluto.watermarkText.empty()) {
        return config.pluto.watermarkText;
    }
    if (!config.pluto.callsign.empty()) {
        return config.pluto.callsign;
    }
    return "WH Repeater";
}

std::string identText(const RepeaterConfig& config)
{
    if (!config.ident.serviceName.empty()) {
        return config.ident.serviceName;
    }
    return repeaterName(config);
}

bool isAnalogueInput(const RepeaterConfig& config, const ActiveInput& input)
{
    return input.receiver == config.analogue.capture.receiver
        || (input.status.modulation.has_value() && *input.status.modulation == "SD analogue");
}

std::string identFilter(std::string_view text)
{
    std::ostringstream filter;
    filter << "drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(text)
           << "':x=46:y=44:fontsize=34:fontcolor=white"
           << ":box=1:boxcolor=0x0057b8@0.68:boxborderw=14";
    return filter.str();
}

std::string fallbackClockText()
{
    const auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);

    char buffer[9]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &utc);
    return buffer;
}

std::string fallbackClockFilter(std::string_view text)
{
    std::ostringstream filter;
    filter << "drawtext=fontfile='" << clockFont << "':text='" << escapeDrawText(text)
           << "':x=w-text_w-46:y=h-text_h-44:fontsize=34:fontcolor=white"
           << ":box=1:boxcolor=0x0057b8@0.68:boxborderw=14";
    return filter.str();
}

void appendInputTitle(std::ostringstream& text, const ActiveInput& input)
{
    if (input.receiver.value > 0) {
        text << "RX" << input.receiver.value;
        if (!input.target.label.empty()) {
            text << " " << input.target.label;
        }
        return;
    }

    if (!input.target.label.empty()) {
        text << input.target.label;
    } else {
        text << "Received stream";
    }
}

std::string streamInfoText(const ActiveInput& input, std::string_view codec, int width, int height, int frameRate)
{
    std::ostringstream text;
    appendInputTitle(text, input);
    text << "\n" << input.target.frequencyKhz << " kHz / " << input.target.symbolRateKs << " kS"
         << " | " << codec << " " << width << "x" << height << " " << frameRate << " fps";
    if (input.status.serviceName.has_value() && !input.status.serviceName->empty()) {
        text << "\n" << *input.status.serviceName;
        if (input.status.modulation.has_value() && !input.status.modulation->empty()) {
            text << " | " << *input.status.modulation;
        }
    } else if (input.status.modulation.has_value() && !input.status.modulation->empty()) {
        text << "\n" << *input.status.modulation;
    }
    return text.str();
}

std::optional<std::string> dictionaryText(const AVDictionary* metadata, std::initializer_list<std::string_view> keys)
{
    if (metadata == nullptr) {
        return std::nullopt;
    }
    for (const auto key : keys) {
        if (const auto* entry = av_dict_get(metadata, std::string{key}.c_str(), nullptr, 0);
            entry != nullptr && entry->value != nullptr && entry->value[0] != '\0') {
            return std::string{entry->value};
        }
    }
    return std::nullopt;
}

std::optional<std::string> programMetadataText(const AVFormatContext& inputFormat,
                                               const AVStream& stream,
                                               std::initializer_list<std::string_view> keys)
{
    for (unsigned int index = 0; index < inputFormat.nb_programs; ++index) {
        const auto* program = inputFormat.programs[index];
        if (program == nullptr) {
            continue;
        }
        for (unsigned int streamIndex = 0; streamIndex < program->nb_stream_indexes; ++streamIndex) {
            if (static_cast<int>(program->stream_index[streamIndex]) == stream.index) {
                if (auto value = dictionaryText(program->metadata, keys); value.has_value()) {
                    return value;
                }
            }
        }
    }
    return std::nullopt;
}

std::string streamInfoText(const ActiveInput& input,
                           std::string_view codec,
                           int width,
                           int height,
                           int frameRate,
                           const AVFormatContext& inputFormat,
                           const AVStream& stream)
{
    auto serviceName = programMetadataText(inputFormat, stream, {"service_name", "title"});
    if (!serviceName.has_value()) {
        serviceName = dictionaryText(inputFormat.metadata, {"service_name", "title"});
    }
    if (!serviceName.has_value()) {
        serviceName = input.status.serviceName;
    }
    auto providerName = programMetadataText(inputFormat, stream, {"service_provider", "provider_name"});
    if (!providerName.has_value()) {
        providerName = dictionaryText(inputFormat.metadata, {"service_provider", "provider_name"});
    }
    auto networkName = programMetadataText(inputFormat, stream, {"network_name"});
    if (!networkName.has_value()) {
        networkName = dictionaryText(inputFormat.metadata, {"network_name"});
    }

    std::ostringstream text;
    appendInputTitle(text, input);
    text << "\n" << input.target.frequencyKhz << " kHz / " << input.target.symbolRateKs << " kS"
         << " | " << codec << " " << width << "x" << height << " " << frameRate << " fps";
    if (serviceName.has_value() && !serviceName->empty()) {
        text << "\n" << *serviceName;
        if (providerName.has_value() && !providerName->empty()) {
            text << " | " << *providerName;
        }
    }
    if (networkName.has_value() && !networkName->empty()) {
        text << "\nNetwork: " << *networkName;
    } else if (input.status.modulation.has_value() && !input.status.modulation->empty()) {
        text << "\n" << *input.status.modulation;
    }
    return text.str();
}

std::string receivedStreamErrorText(const ActiveInput& input, std::string_view error)
{
    std::string shortError{error};
    constexpr std::size_t maxErrorLength{72};
    if (shortError.size() > maxErrorLength) {
        shortError.resize(maxErrorLength - 3);
        shortError += "...";
    }

    std::ostringstream text;
    appendInputTitle(text, input);
    text << "\n" << input.target.frequencyKhz << " kHz / " << input.target.symbolRateKs << " kS";
    if (input.status.modulation.has_value() && !input.status.modulation->empty()) {
        text << " | " << *input.status.modulation;
    }
    text << "\nReceived TS | " << (shortError.empty() ? "decode stopped" : shortError);
    return text.str();
}

std::string streamInfoFilter(std::string_view text, int frameRate)
{
    const auto lines = splitLines(text);
    std::ostringstream filter;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            filter << ",";
        }
        filter << "drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(lines[index])
               << "':x=w-text_w-46:y=" << (44 + static_cast<int>(index) * 40)
               << ":fontsize=24:fontcolor=white"
               << ":box=1:boxcolor=black@0.62:boxborderw=10"
               << ":enable='lt(n," << std::max(1, frameRate) * 10 << ")'";
    }
    return filter.str();
}

FramePtr renderIdentFrame(const RepeaterConfig& config, int frameRate)
{
    return renderFilteredFrame(config, identFilter(identText(config)), frameRate, fillBlack);
}

std::string testcardFilter(std::string_view text)
{
    std::ostringstream filter;
    filter << "drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(text)
           << "':x=(w-text_w)/2:y=(h-text_h)/2:fontsize=112:fontcolor=white"
           << ":box=1:boxcolor=black@0.35:boxborderw=28";
    return filter.str();
}

FramePtr renderTestcardFrame(const RepeaterConfig& config, int frameRate)
{
    return renderFilteredFrame(config, testcardFilter(identText(config)), frameRate, fillTestcard);
}

class OverlayRenderer {
public:
    OverlayRenderer(std::string filter, int width, int height, AVPixelFormat pixelFormat, int frameRate)
        : pixelFormat_{pixelFormat}
    {
        graph_.reset(avfilter_graph_alloc());
        if (!graph_) {
            throw std::runtime_error{"allocate overlay filter graph failed"};
        }

        const auto sourceArgs = "video_size=" + std::to_string(width) + "x" + std::to_string(height)
            + ":pix_fmt=" + std::to_string(pixelFormat_)
            + ":time_base=1/" + std::to_string(std::max(1, frameRate))
            + ":pixel_aspect=1/1";
        checkAv(avfilter_graph_create_filter(&source_,
                                             avfilter_get_by_name("buffer"),
                                             "ident_overlay_source",
                                             sourceArgs.c_str(),
                                             nullptr,
                                             graph_.get()),
                "create ident overlay source");
        checkAv(avfilter_graph_create_filter(&sink_,
                                             avfilter_get_by_name("buffersink"),
                                             "ident_overlay_sink",
                                             nullptr,
                                             nullptr,
                                             graph_.get()),
                "create ident overlay sink");
        const AVPixelFormat pixelFormats[] = {pixelFormat_, AV_PIX_FMT_NONE};
        checkAv(av_opt_set_int_list(sink_, "pix_fmts", pixelFormats, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN),
                "set ident overlay pixel format");

        AVFilterInOut* rawInputs = avfilter_inout_alloc();
        AVFilterInOut* rawOutputs = avfilter_inout_alloc();
        if (rawInputs == nullptr || rawOutputs == nullptr) {
            avfilter_inout_free(&rawInputs);
            avfilter_inout_free(&rawOutputs);
            throw std::runtime_error{"allocate ident overlay endpoints failed"};
        }
        std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> inputs{rawInputs};
        std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> outputs{rawOutputs};
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        AVFilterInOut* inputPtr = inputs.release();
        AVFilterInOut* outputPtr = outputs.release();
        const auto parseStatus = avfilter_graph_parse_ptr(graph_.get(), filter.c_str(), &inputPtr, &outputPtr, nullptr);
        avfilter_inout_free(&inputPtr);
        avfilter_inout_free(&outputPtr);
        checkAv(parseStatus, "parse overlay filter");
        checkAv(avfilter_graph_config(graph_.get(), nullptr), "configure overlay filter");
    }

    FramePtr render(AVFrame* frame)
    {
        checkAv(av_buffersrc_add_frame_flags(source_, frame, AV_BUFFERSRC_FLAG_KEEP_REF), "send frame to overlay");
        FramePtr output{av_frame_alloc()};
        if (!output) {
            throw std::runtime_error{"allocate overlay frame failed"};
        }
        checkAv(av_buffersink_get_frame(sink_, output.get()), "receive overlay frame");
        return output;
    }

private:
    AVPixelFormat pixelFormat_;
    std::unique_ptr<AVFilterGraph, AvFilterGraphDeleter> graph_;
    AVFilterContext* source_{};
    AVFilterContext* sink_{};
};

struct AsyncRenderedFrame {
    explicit AsyncRenderedFrame(std::string renderKey)
        : key{std::move(renderKey)}
    {
    }

    std::string key;
    std::mutex mutex;
    FramePtr frame;
    bool ready{};
};

class VideoFilter {
public:
    VideoFilter(std::string filter, AVFrame& frame, int frameRate)
        : filter_{std::move(filter)}
    {
        graph_.reset(avfilter_graph_alloc());
        if (!graph_) {
            throw std::runtime_error{"allocate video filter graph failed"};
        }

        const auto sampleAspect = frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0
            ? frame.sample_aspect_ratio
            : AVRational{1, 1};
        std::ostringstream sourceArgs;
        sourceArgs << "video_size=" << frame.width << "x" << frame.height
                   << ":pix_fmt=" << frame.format
                   << ":time_base=1/" << std::max(1, frameRate)
                   << ":pixel_aspect=" << sampleAspect.num << "/" << sampleAspect.den;

        checkAv(avfilter_graph_create_filter(&source_,
                                             avfilter_get_by_name("buffer"),
                                             "video_filter_source",
                                             sourceArgs.str().c_str(),
                                             nullptr,
                                             graph_.get()),
                "create video filter source");
        checkAv(avfilter_graph_create_filter(&sink_,
                                             avfilter_get_by_name("buffersink"),
                                             "video_filter_sink",
                                             nullptr,
                                             nullptr,
                                             graph_.get()),
                "create video filter sink");

        AVFilterInOut* rawInputs = avfilter_inout_alloc();
        AVFilterInOut* rawOutputs = avfilter_inout_alloc();
        if (rawInputs == nullptr || rawOutputs == nullptr) {
            avfilter_inout_free(&rawInputs);
            avfilter_inout_free(&rawOutputs);
            throw std::runtime_error{"allocate video filter endpoints failed"};
        }
        std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> inputs{rawInputs};
        std::unique_ptr<AVFilterInOut, AvFilterInOutDeleter> outputs{rawOutputs};
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        AVFilterInOut* inputPtr = inputs.release();
        AVFilterInOut* outputPtr = outputs.release();
        const auto parseStatus = avfilter_graph_parse_ptr(graph_.get(), filter_.c_str(), &inputPtr, &outputPtr, nullptr);
        avfilter_inout_free(&inputPtr);
        avfilter_inout_free(&outputPtr);
        checkAv(parseStatus, "parse video filter");
        checkAv(avfilter_graph_config(graph_.get(), nullptr), "configure video filter");
    }

    FramePtr process(AVFrame* frame)
    {
        checkAv(av_buffersrc_add_frame_flags(source_, frame, AV_BUFFERSRC_FLAG_KEEP_REF), "send frame to video filter");
        FramePtr output{av_frame_alloc()};
        if (!output) {
            throw std::runtime_error{"allocate filtered video frame failed"};
        }
        checkAv(av_buffersink_get_frame(sink_, output.get()), "receive filtered video frame");
        return output;
    }

private:
    std::string filter_;
    std::unique_ptr<AVFilterGraph, AvFilterGraphDeleter> graph_;
    AVFilterContext* source_{};
    AVFilterContext* sink_{};
};

std::string morseFor(char ch)
{
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";
    case '/': return "-..-.";
    default: return {};
    }
}

std::vector<bool> morseToneUnits(std::string_view text)
{
    std::vector<bool> units;
    bool hadSymbol = false;
    for (const auto ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!units.empty()) {
                units.resize(units.size() + 4, false);
            }
            continue;
        }

        const auto code = morseFor(ch);
        if (code.empty()) {
            continue;
        }
        if (hadSymbol) {
            units.resize(units.size() + 3, false);
        }
        for (std::size_t index = 0; index < code.size(); ++index) {
            const auto symbolUnits = code[index] == '-' ? 3U : 1U;
            units.resize(units.size() + symbolUnits, true);
            if (index + 1 != code.size()) {
                units.push_back(false);
            }
        }
        hadSymbol = true;
    }
    units.resize(units.size() + 7, false);
    return units;
}

std::string sleepingMessage(const RepeaterConfig& config)
{
    return std::string{"Power Saving Mode\n"}
        + "sleeping between " + config.beaconSchedule.endTime + " and " + config.beaconSchedule.startTime + "\n\n"
        + "The repeater can be woken during this time\n"
        + "with a video signal on any input.\n"
        + "Back to sleep after access.";
}

int plutoWritePacket(void* opaque, const std::uint8_t* buffer, int size)
{
    auto* sink = static_cast<PlutoSink*>(opaque);
    sink->writeMuxData(std::span<const std::byte>{reinterpret_cast<const std::byte*>(buffer), static_cast<std::size_t>(size)});
    return size;
}

void configureEncoderContext(AVCodecContext& codec,
                             const AVFormatContext& format,
                             const RepeaterConfig& config,
                             std::string_view encoderName,
                             int width,
                             int height,
                             int frameRate,
                             int videoBitrateKbps)
{
    codec.codec_type = AVMEDIA_TYPE_VIDEO;
    codec.width = width;
    codec.height = height;
    if (isVaapiEncoder(encoderName)) {
        codec.pix_fmt = AV_PIX_FMT_VAAPI;
    } else if (encoderName == "h264_qsv") {
        codec.pix_fmt = AV_PIX_FMT_NV12;
    } else {
        codec.pix_fmt = AV_PIX_FMT_YUV420P;
    }
    codec.time_base = AVRational{1, frameRate};
    codec.framerate = AVRational{frameRate, 1};
    codec.gop_size = frameRate * 2;
    codec.max_b_frames = 0;
    codec.bit_rate = static_cast<std::int64_t>(videoBitrateKbps) * 1000;
    codec.rc_max_rate = codec.bit_rate;
    codec.rc_buffer_size = isVaapiEncoder(encoderName)
        ? static_cast<int>(std::min<std::int64_t>(std::numeric_limits<int>::max(), codec.bit_rate * 2))
        : static_cast<int>(codec.bit_rate / frameRate);
    codec.profile = h264AvProfile(config);
    codec.level = h264AvLevel(config);
    if ((format.oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

std::vector<std::string> h264EncoderCandidates(const RepeaterConfig& config)
{
    (void)config;
    return {"h264_vaapi", "libx264"};
}

AVBufferRef* createVaapiDevice()
{
    AVBufferRef* rawDevice{};
    checkAv(av_hwdevice_ctx_create(&rawDevice, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0),
            "create VAAPI device");
    return rawDevice;
}

AVPixelFormat chooseVaapiPixelFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VAAPI) {
            return *format;
        }
    }
    return formats[0];
}

void configureVaapiHardwareFrames(AVCodecContext& codec, int width, int height)
{
    AVBufferRef* rawDevice = createVaapiDevice();
    std::unique_ptr<AVBufferRef, decltype([](AVBufferRef* ref) { av_buffer_unref(&ref); })> device{rawDevice};

    AVBufferRef* rawFrames = av_hwframe_ctx_alloc(device.get());
    if (rawFrames == nullptr) {
        throw std::runtime_error{"allocate VAAPI frame context failed"};
    }
    std::unique_ptr<AVBufferRef, decltype([](AVBufferRef* ref) { av_buffer_unref(&ref); })> frames{rawFrames};

    auto* frameContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
    frameContext->format = AV_PIX_FMT_VAAPI;
    frameContext->sw_format = AV_PIX_FMT_NV12;
    frameContext->width = width;
    frameContext->height = height;
    frameContext->initial_pool_size = 16;
    checkAv(av_hwframe_ctx_init(frames.get()), "initialise VAAPI frame context");

    codec.hw_device_ctx = av_buffer_ref(device.get());
    if (codec.hw_device_ctx == nullptr) {
        throw std::runtime_error{"reference VAAPI device failed"};
    }
    codec.hw_frames_ctx = av_buffer_ref(frames.get());
    if (codec.hw_frames_ctx == nullptr) {
        throw std::runtime_error{"reference VAAPI frames failed"};
    }
}

void configureEncoderOptions(AVDictionary*& options, std::string_view encoderName);

bool encoderOpenProbe(std::string_view encoderName,
                      const AVFormatContext& format,
                      const RepeaterConfig& config,
                      int width,
                      int height,
                      int frameRate,
                      int videoBitrateKbps,
                      std::chrono::milliseconds timeout)
{
    const auto* encoder = avcodec_find_encoder_by_name(std::string{encoderName}.c_str());
    if (encoder == nullptr) {
        return false;
    }

    const auto pid = ::fork();
    if (pid < 0) {
        std::cerr << "wh-repeater: encoder probe fork failed for " << encoderName
                  << ": " << std::strerror(errno) << '\n';
        return true;
    }
    if (pid == 0) {
        ::signal(SIGALRM, SIG_DFL);
        ::alarm(static_cast<unsigned int>(std::max<std::int64_t>(1, timeout.count() / 1000)));
        AVCodecContext* codec = avcodec_alloc_context3(encoder);
        if (codec == nullptr) {
            _exit(2);
        }
        configureEncoderContext(*codec, format, config, encoderName, width, height, frameRate, videoBitrateKbps);
        if (isVaapiEncoder(encoderName)) {
            try {
                configureVaapiHardwareFrames(*codec, width, height);
            } catch (...) {
                avcodec_free_context(&codec);
                _exit(2);
            }
        }
        AVDictionary* options = nullptr;
        configureEncoderOptions(options, encoderName);
        const auto status = avcodec_open2(codec, encoder, &options);
        av_dict_free(&options);
        avcodec_free_context(&codec);
        _exit(status >= 0 ? 0 : 2);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (waited < 0 && errno != EINTR) {
            std::cerr << "wh-repeater: encoder probe wait failed for " << encoderName
                      << ": " << std::strerror(errno) << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, WNOHANG);
    std::cerr << "wh-repeater: encoder probe timed out for " << encoderName << '\n';
    return false;
}

void configureEncoderOptions(AVDictionary*& options, std::string_view encoderName)
{
    if (encoderName == "libx264") {
        av_dict_set(&options, "preset", "veryfast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
    } else if (encoderName == "h264_qsv") {
        av_dict_set(&options, "preset", "veryfast", 0);
        av_dict_set(&options, "forced_idr", "1", 0);
    } else if (isVaapiEncoder(encoderName)) {
        av_dict_set(&options, "aud", "1", 0);
    }
}

EncoderOpenResult openH264Encoder(const AVFormatContext& format,
                                  const RepeaterConfig& config,
                                  int width,
                                  int height,
                                  int frameRate,
                                  int videoBitrateKbps)
{
    std::string errors;

    for (const auto& encoderName : h264EncoderCandidates(config)) {
        const auto* encoder = avcodec_find_encoder_by_name(encoderName.c_str());
        if (encoder == nullptr) {
            errors += encoderName + ": not available; ";
            continue;
        }
        if (!encoderOpenProbe(encoderName, format, config, width, height, frameRate, videoBitrateKbps, std::chrono::seconds{5})) {
            errors += encoderName + ": probe failed or timed out; ";
            continue;
        }

        AVCodecContext* codec = avcodec_alloc_context3(encoder);
        if (codec == nullptr) {
            errors += encoderName + ": allocate context failed; ";
            continue;
        }
        configureEncoderContext(*codec, format, config, encoderName, width, height, frameRate, videoBitrateKbps);
        if (isVaapiEncoder(encoderName)) {
            try {
                configureVaapiHardwareFrames(*codec, width, height);
            } catch (const std::exception& ex) {
                errors += encoderName + ": " + ex.what() + "; ";
                avcodec_free_context(&codec);
                continue;
            }
        }

        AVDictionary* options = nullptr;
        configureEncoderOptions(options, encoderName);
        const auto status = avcodec_open2(codec, encoder, &options);
        av_dict_free(&options);
        if (status >= 0) {
            return EncoderOpenResult{codec, encoder, encoderName};
        }

        errors += encoderName + ": " + avError(status) + "; ";
        avcodec_free_context(&codec);
    }

    throw std::runtime_error{"open H.264 encoder failed: " + errors};
}

EncoderOpenResult openFallbackEncoder(const RepeaterConfig& config, const AVFormatContext& format, int frameRate, int videoBitrateKbps)
{
    return openH264Encoder(format, config, outputWidth(config), outputHeight(config), frameRate, videoBitrateKbps);
}

#if defined(WH_REPEATER_HAVE_GSTREAMER)
struct GstObjectDeleter {
    void operator()(GstObject* object) const
    {
        if (object != nullptr) {
            gst_object_unref(object);
        }
    }
};

struct GstElementStateDeleter {
    void operator()(GstElement* element) const
    {
        if (element != nullptr) {
            gst_element_set_state(element, GST_STATE_NULL);
            gst_object_unref(element);
        }
    }
};

struct GstSampleDeleter {
    void operator()(GstSample* sample) const
    {
        if (sample != nullptr) {
            gst_sample_unref(sample);
        }
    }
};

struct GstMessageDeleter {
    void operator()(GstMessage* message) const
    {
        if (message != nullptr) {
            gst_message_unref(message);
        }
    }
};

using GstElementPtr = std::unique_ptr<GstElement, GstElementStateDeleter>;
using GstObjectPtr = std::unique_ptr<GstObject, GstObjectDeleter>;
using GstSamplePtr = std::unique_ptr<GstSample, GstSampleDeleter>;
using GstMessagePtr = std::unique_ptr<GstMessage, GstMessageDeleter>;

std::string gstQuoted(std::string_view value)
{
    std::string output{"\""};
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') {
            output.push_back('\\');
        }
        output.push_back(ch);
    }
    output.push_back('"');
    return output;
}

void requireGstElement(std::string_view name)
{
    GstElementFactory* factory = gst_element_factory_find(std::string{name}.c_str());
    if (factory == nullptr) {
        throw std::runtime_error{"GStreamer element not available: " + std::string{name}};
    }
    gst_object_unref(factory);
}

bool gstElementAvailable(std::string_view name)
{
    GstElementFactory* factory = gst_element_factory_find(std::string{name}.c_str());
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

std::string gstH264EncoderElement(const RepeaterConfig& config, int frameRate, int videoBitrate)
{
    (void)config;
    (void)frameRate;
    (void)videoBitrate;
    throw std::runtime_error{"GStreamer H.264 output is disabled after removal of legacy V4L2 codec support"};
}

std::string gstRawOutputPipeline(const RepeaterConfig& config)
{
    configureGStreamerRuntime();
    requireGstElement("appsrc");
    requireGstElement("queue");
    requireGstElement("h264parse");
    requireGstElement("mpegtsmux");
    requireGstElement("appsink");
    requireGstElement("audioconvert");
    requireGstElement("audioresample");
    requireGstElement("avenc_aac");
    requireGstElement("aacparse");
    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        requireGstElement("tee");
        requireGstElement("flvmux");
        if (!gstElementAvailable("rtmp2sink") && !gstElementAvailable("rtmpsink")) {
            throw std::runtime_error{"GStreamer RTMP sink not available"};
        }
    }

    const auto width = outputWidth(config);
    const auto height = outputHeight(config);
    const auto frameRate = outputFrameRate(config);
    const auto videoBitrate = std::max(250, static_cast<int>(fallbackVideoBitrateKbps(config))) * 1000;
    const auto audioBitrate = std::max(16000, static_cast<int>(config.pluto.audioBitrateKbps) * 1000);
    const auto muxBitrate = std::max(250000, static_cast<int>(config.pluto.muxRateKbps) * 1000);
    const auto encoder = gstH264EncoderElement(config, frameRate, videoBitrate);

    std::ostringstream pipeline;
    pipeline
        << "mpegtsmux name=mux alignment=7 bitrate=" << muxBitrate << " pat-interval=9000 pmt-interval=9000 pcr-interval=3600 "
        << "! appsink name=ts_sink emit-signals=false sync=false max-buffers=16 drop=false "
        << "appsrc name=video_src is-live=true format=time do-timestamp=false block=true "
        << "caps=video/x-raw,format=I420,width=" << width << ",height=" << height << ",framerate=" << frameRate << "/1 "
        << "! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
        << "! " << encoder;

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        pipeline
            << "! tee name=video_t "
            << "video_t. ! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    } else {
        pipeline
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    }

    pipeline
        << "appsrc name=audio_src is-live=true format=time do-timestamp=false block=true "
        << "caps=audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=1 "
        << "! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
        << "! audioconvert ! audioresample "
        << "! avenc_aac bitrate=" << audioBitrate << " ";

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        const auto rtmpElement = gstElementAvailable("rtmp2sink") ? "rtmp2sink" : "rtmpsink";
        pipeline
            << "! tee name=audio_t "
            << "audio_t. ! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. "
            << "flvmux name=flv streamable=true "
            << "! " << rtmpElement << " location=" << gstQuoted(config.streaming.rtmp.url) << " sync=false async=false "
            << "video_t. ! queue leaky=downstream max-size-buffers=30 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! flv. "
            << "audio_t. ! queue leaky=downstream max-size-buffers=50 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=raw ! flv. ";
    } else {
        pipeline << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. ";
    }

    return pipeline.str();
}

std::string gstLiveTsPipeline(const RepeaterConfig& config)
{
    configureGStreamerRuntime();
    requireGstElement("appsrc");
    requireGstElement("tsdemux");
    requireGstElement("decodebin");
    requireGstElement("queue");
    requireGstElement("videoconvert");
    requireGstElement("videoscale");
    requireGstElement("videorate");
    requireGstElement("textoverlay");
    requireGstElement("h264parse");
    requireGstElement("mpegtsmux");
    requireGstElement("appsink");
    requireGstElement("audioconvert");
    requireGstElement("audioresample");
    requireGstElement("avenc_aac");
    requireGstElement("aacparse");
    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        requireGstElement("tee");
        requireGstElement("flvmux");
        if (!gstElementAvailable("rtmp2sink") && !gstElementAvailable("rtmpsink")) {
            throw std::runtime_error{"GStreamer RTMP sink not available"};
        }
    }

    const auto width = outputWidth(config);
    const auto height = outputHeight(config);
    const auto frameRate = outputFrameRate(config);
    const auto videoBitrate = std::max(250, static_cast<int>(config.pluto.videoBitrateKbps)) * 1000;
    const auto audioBitrate = std::max(16000, static_cast<int>(config.pluto.audioBitrateKbps) * 1000);
    const auto muxBitrate = std::max(250000, static_cast<int>(config.pluto.muxRateKbps) * 1000);
    const auto encoder = gstH264EncoderElement(config, frameRate, videoBitrate);

    std::ostringstream pipeline;
    pipeline
        << "appsrc name=ts_src is-live=true format=bytes do-timestamp=false block=true "
        << "caps=video/mpegts,systemstream=true,packetsize=188 "
        << "! queue max-size-buffers=128 max-size-bytes=0 max-size-time=0 "
        << "! tsdemux name=demux "
        << "mpegtsmux name=mux alignment=7 bitrate=" << muxBitrate << " pat-interval=9000 pmt-interval=9000 pcr-interval=3600 "
        << "! appsink name=ts_sink emit-signals=false sync=false max-buffers=16 drop=false "
        << "demux. ! queue max-size-buffers=64 max-size-bytes=0 max-size-time=0 "
        << "! decodebin ! videoconvert ! videoscale add-borders=true ! videorate "
        << "! video/x-raw,format=I420,width=" << width << ",height=" << height << ",framerate=" << frameRate << "/1 "
        << "! textoverlay text=" << gstQuoted(identText(config))
        << " halignment=left valignment=top shaded-background=true font-desc=" << gstQuoted("Inter Semi Bold 22")
        << "! " << encoder;

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        pipeline
            << "! tee name=video_t "
            << "video_t. ! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    } else {
        pipeline
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    }

    pipeline
        << "demux. ! queue max-size-buffers=64 max-size-bytes=0 max-size-time=0 "
        << "! decodebin ! audioconvert ! audioresample "
        << "! audio/x-raw,rate=48000,channels=1 "
        << "! avenc_aac bitrate=" << audioBitrate << " ";

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        const auto rtmpElement = gstElementAvailable("rtmp2sink") ? "rtmp2sink" : "rtmpsink";
        pipeline
            << "! tee name=audio_t "
            << "audio_t. ! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. "
            << "flvmux name=flv streamable=true "
            << "! " << rtmpElement << " location=" << gstQuoted(config.streaming.rtmp.url) << " sync=false async=false "
            << "video_t. ! queue leaky=downstream max-size-buffers=30 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! flv. "
            << "audio_t. ! queue leaky=downstream max-size-buffers=50 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=raw ! flv. ";
    } else {
        pipeline << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. ";
    }

    return pipeline.str();
}

std::string gstAnalogueCapturePipeline(const RepeaterConfig& config)
{
    configureGStreamerRuntime();
    requireGstElement("v4l2src");
    requireGstElement("audiotestsrc");
    requireGstElement("queue");
    requireGstElement("videoconvert");
    requireGstElement("videoscale");
    requireGstElement("videorate");
    requireGstElement("textoverlay");
    requireGstElement("h264parse");
    requireGstElement("mpegtsmux");
    requireGstElement("appsink");
    requireGstElement("audioconvert");
    requireGstElement("audioresample");
    requireGstElement("avenc_aac");
    requireGstElement("aacparse");
    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        requireGstElement("tee");
        requireGstElement("flvmux");
        if (!gstElementAvailable("rtmp2sink") && !gstElementAvailable("rtmpsink")) {
            throw std::runtime_error{"GStreamer RTMP sink not available"};
        }
    }

    const auto& capture = config.analogue.capture;
    const auto width = outputWidth(config);
    const auto height = outputHeight(config);
    const auto frameRate = outputFrameRate(config);
    const auto captureRateNum = std::max(1U, capture.captureFrameRateNumerator);
    const auto captureRateDen = std::max(1U, capture.captureFrameRateDenominator);
    const auto videoBitrate = std::max(250, static_cast<int>(config.pluto.videoBitrateKbps)) * 1000;
    const auto audioBitrate = std::max(16000, static_cast<int>(config.pluto.audioBitrateKbps) * 1000);
    const auto muxBitrate = std::max(250000, static_cast<int>(config.pluto.muxRateKbps) * 1000);
    const auto encoder = gstH264EncoderElement(config, frameRate, videoBitrate);

    std::ostringstream pipeline;
    pipeline
        << "mpegtsmux name=mux alignment=7 bitrate=" << muxBitrate << " pat-interval=9000 pmt-interval=9000 pcr-interval=3600 "
        << "! appsink name=ts_sink emit-signals=false sync=false max-buffers=16 drop=false "
        << "v4l2src device=" << gstQuoted(capture.captureDevice) << " do-timestamp=true "
        << "! video/x-raw,width=" << capture.captureWidth << ",height=" << capture.captureHeight
        << ",framerate=" << captureRateNum << "/" << captureRateDen << " "
        << "! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
        << "! videoconvert ! videoscale add-borders=true ! videorate "
        << "! video/x-raw,format=I420,width=" << width << ",height=" << height << ",framerate=" << frameRate << "/1 "
        << "! textoverlay text=" << gstQuoted(identText(config))
        << " halignment=left valignment=top shaded-background=true font-desc=" << gstQuoted("Inter Semi Bold 22")
        << "! " << encoder;

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        pipeline
            << "! tee name=video_t "
            << "video_t. ! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    } else {
        pipeline
            << "! h264parse config-interval=-1 "
            << "! video/x-h264,stream-format=byte-stream,alignment=au ! mux. ";
    }

    pipeline
        << "audiotestsrc wave=silence is-live=true "
        << "! audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=1 "
        << "! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
        << "! audioconvert ! audioresample "
        << "! avenc_aac bitrate=" << audioBitrate << " ";

    if (config.streaming.rtmp.enabled && !config.streaming.rtmp.url.empty()) {
        const auto rtmpElement = gstElementAvailable("rtmp2sink") ? "rtmp2sink" : "rtmpsink";
        pipeline
            << "! tee name=audio_t "
            << "audio_t. ! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. "
            << "flvmux name=flv streamable=true "
            << "! " << rtmpElement << " location=" << gstQuoted(config.streaming.rtmp.url) << " sync=false async=false "
            << "video_t. ! queue leaky=downstream max-size-buffers=30 max-size-bytes=0 max-size-time=0 "
            << "! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! flv. "
            << "audio_t. ! queue leaky=downstream max-size-buffers=50 max-size-bytes=0 max-size-time=0 "
            << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=raw ! flv. ";
    } else {
        pipeline << "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts ! mux. ";
    }

    return pipeline.str();
}
#endif

class BlockingTsInput {
public:
    void append(std::span<const std::byte> data)
    {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto chunkSize = std::min(maxAppendChunk, data.size() - offset);
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [this, chunkSize] {
                return stopped_ || buffer_.size() + chunkSize <= maxBufferedBytes;
            });
            if (stopped_) {
                return;
            }
            buffer_.insert(buffer_.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + chunkSize));
            offset += chunkSize;
            lock.unlock();
            ready_.notify_one();
        }
    }

    int read(std::uint8_t* buffer, int size)
    {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] {
            return stopped_ || !buffer_.empty();
        });
        if (stopped_ && buffer_.empty()) {
            return AVERROR_EOF;
        }

        const auto count = std::min<std::size_t>(static_cast<std::size_t>(size), buffer_.size());
        for (std::size_t index = 0; index < count; ++index) {
            buffer[index] = static_cast<std::uint8_t>(buffer_.front());
            buffer_.pop_front();
        }
        lock.unlock();
        ready_.notify_all();
        return static_cast<int>(count);
    }

    void stop()
    {
        {
            std::lock_guard lock{mutex_};
            stopped_ = true;
        }
        ready_.notify_all();
    }

private:
    static constexpr std::size_t maxBufferedBytes{32 * 1024 * 1024};
    static constexpr std::size_t maxAppendChunk{1 * 1024 * 1024};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::byte> buffer_;
    bool stopped_{false};
};

int readLiveTsPacket(void* opaque, std::uint8_t* buffer, int size)
{
    return static_cast<BlockingTsInput*>(opaque)->read(buffer, size);
}

struct SwsContextDeleter {
    void operator()(SwsContext* context) const
    {
        sws_freeContext(context);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const
    {
        swr_free(&context);
    }
};

struct AvAudioFifoDeleter {
    void operator()(AVAudioFifo* fifo) const
    {
        av_audio_fifo_free(fifo);
    }
};

struct AvFormatInputDeleter {
    void operator()(AVFormatContext* context) const
    {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

using InputFormatPtr = std::unique_ptr<AVFormatContext, AvFormatInputDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AvAudioFifoDeleter>;

FramePtr convertFrameFormat(AVFrame* frame, AVPixelFormat format, SwsContextPtr& scaler)
{
    FramePtr converted{av_frame_alloc()};
    if (!converted) {
        throw std::runtime_error{"allocate converted encoder frame failed"};
    }
    converted->format = format;
    converted->width = frame->width;
    converted->height = frame->height;
    converted->pts = frame->pts;
    checkAv(av_frame_get_buffer(converted.get(), 32), "allocate converted encoder frame buffer");
    checkAv(av_frame_make_writable(converted.get()), "make converted encoder frame writable");

    if (!scaler) {
        scaler.reset(sws_getContext(frame->width,
                                    frame->height,
                                    static_cast<AVPixelFormat>(frame->format),
                                    frame->width,
                                    frame->height,
                                    format,
                                    SWS_FAST_BILINEAR,
                                    nullptr,
                                    nullptr,
                                    nullptr));
        if (!scaler) {
            throw std::runtime_error{"create encoder upload scaler failed"};
        }
    }
    sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, converted->data, converted->linesize);
    normaliseVideoFrameProperties(*converted);
    return converted;
}

FramePtr uploadVaapiFrame(AVCodecContext& codec, AVFrame* frame, SwsContextPtr& scaler)
{
    if (codec.hw_frames_ctx == nullptr) {
        throw std::runtime_error{"VAAPI encoder has no frame context"};
    }

    FramePtr converted;
    AVFrame* uploadSource = frame;
    if (static_cast<AVPixelFormat>(frame->format) != AV_PIX_FMT_NV12) {
        converted = convertFrameFormat(frame, AV_PIX_FMT_NV12, scaler);
        uploadSource = converted.get();
    }

    FramePtr hardware{av_frame_alloc()};
    if (!hardware) {
        throw std::runtime_error{"allocate VAAPI encoder frame failed"};
    }
    checkAv(av_hwframe_get_buffer(codec.hw_frames_ctx, hardware.get(), 0), "allocate VAAPI encoder frame");
    checkAv(av_hwframe_transfer_data(hardware.get(), uploadSource, 0), "upload VAAPI encoder frame");
    hardware->pts = frame->pts;
    hardware->duration = frame->duration;
    hardware->sample_aspect_ratio = frame->sample_aspect_ratio;
    return hardware;
}

bool isImageFile(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".bmp";
}

std::vector<std::filesystem::path> slideFilesIn(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error)) {
        return files;
    }

    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        if (error) {
            break;
        }
        if (entry.is_regular_file(error) && isImageFile(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool isDecember()
{
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return local.tm_mon == 11;
}

std::string frameRateText(const AnalogueCaptureConfig& capture)
{
    const auto numerator = std::max(1U, capture.captureFrameRateNumerator);
    const auto denominator = std::max(1U, capture.captureFrameRateDenominator);
    if (denominator == 1) {
        return std::to_string(numerator);
    }
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

int roundedFrameRate(const AnalogueCaptureConfig& capture)
{
    const auto numerator = std::max(1U, capture.captureFrameRateNumerator);
    const auto denominator = std::max(1U, capture.captureFrameRateDenominator);
    return std::clamp(static_cast<int>(std::llround(static_cast<double>(numerator) / static_cast<double>(denominator))), 1, 60);
}

AVPixelFormat normalisedPixelFormat(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
    default: return format;
    }
}

FramePtr decodeSlideFrame(const RepeaterConfig& config, const std::filesystem::path& path)
{
    AVFormatContext* rawInput{};
    checkAv(avformat_open_input(&rawInput, path.string().c_str(), nullptr, nullptr), "open slideshow image");
    InputFormatPtr input{rawInput};
    checkAv(avformat_find_stream_info(input.get(), nullptr), "probe slideshow image");

    const auto streamIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    checkAv(streamIndex, "find slideshow image stream");
    auto* stream = input->streams[streamIndex];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error{"no decoder for slideshow image " + path.string()};
    }

    CodecContextPtr decoderContext{avcodec_alloc_context3(decoder)};
    if (!decoderContext) {
        throw std::runtime_error{"allocate slideshow image decoder failed"};
    }
    checkAv(avcodec_parameters_to_context(decoderContext.get(), stream->codecpar), "copy slideshow image parameters");
    checkAv(avcodec_open2(decoderContext.get(), decoder, nullptr), "open slideshow image decoder");

    PacketPtr packet{av_packet_alloc()};
    FramePtr decoded{av_frame_alloc()};
    if (!packet || !decoded) {
        throw std::runtime_error{"allocate slideshow decode buffers failed"};
    }

    bool gotFrame = false;
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            checkAv(avcodec_send_packet(decoderContext.get(), packet.get()), "send slideshow image packet");
            const auto receiveStatus = avcodec_receive_frame(decoderContext.get(), decoded.get());
            if (receiveStatus == 0) {
                gotFrame = true;
                av_packet_unref(packet.get());
                break;
            }
            if (receiveStatus != AVERROR(EAGAIN) && receiveStatus != AVERROR_EOF) {
                checkAv(receiveStatus, "receive slideshow image frame");
            }
        }
        av_packet_unref(packet.get());
    }
    if (!gotFrame) {
        checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush slideshow image decoder");
        gotFrame = avcodec_receive_frame(decoderContext.get(), decoded.get()) == 0;
    }
    if (!gotFrame) {
        throw std::runtime_error{"decode slideshow image failed: " + path.string()};
    }

    auto output = allocateVideoFrame(config);
    fillBlack(*output);

    const auto width = outputWidth(config);
    const auto height = outputHeight(config);
    const auto scale = std::min(static_cast<double>(width) / static_cast<double>(decoded->width),
                                static_cast<double>(height) / static_cast<double>(decoded->height));
    auto dstWidth = std::max(2, static_cast<int>(std::floor(decoded->width * scale))) & ~1;
    auto dstHeight = std::max(2, static_cast<int>(std::floor(decoded->height * scale))) & ~1;
    dstWidth = std::min(dstWidth, width);
    dstHeight = std::min(dstHeight, height);
    const auto dstX = ((width - dstWidth) / 2) & ~1;
    const auto dstY = ((height - dstHeight) / 2) & ~1;

    SwsContextPtr scaler{sws_getContext(decoded->width,
                                        decoded->height,
                                        normalisedPixelFormat(static_cast<AVPixelFormat>(decoded->format)),
                                        dstWidth,
                                        dstHeight,
                                        AV_PIX_FMT_YUV420P,
                                        SWS_BICUBIC,
                                        nullptr,
                                        nullptr,
                                        nullptr)};
    if (!scaler) {
        throw std::runtime_error{"create slideshow image scaler failed"};
    }

    std::array<std::uint8_t*, 4> dstData{
        output->data[0] + dstY * output->linesize[0] + dstX,
        output->data[1] + (dstY / 2) * output->linesize[1] + (dstX / 2),
        output->data[2] + (dstY / 2) * output->linesize[2] + (dstX / 2),
        nullptr,
    };
    std::array<int, 4> dstLinesize{output->linesize[0], output->linesize[1], output->linesize[2], 0};
    sws_scale(scaler.get(), decoded->data, decoded->linesize, 0, decoded->height, dstData.data(), dstLinesize.data());
    return output;
}

std::optional<std::string> readTextFile(const std::filesystem::path& path)
{
    std::ifstream input{path};
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

std::string htmlEscape(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

void replaceAll(std::string& text, std::string_view from, std::string_view to)
{
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string fileUri(const std::filesystem::path& path)
{
    return "file://" + path.string();
}

std::optional<std::filesystem::path> findSlateTemplateDirectory()
{
    constexpr std::array<std::string_view, 3> candidates{{
        "/etc/wh-pc-gateway/slates",
        "/etc/wh-repeater/slates",
        "slates/default",
    }};
    for (const auto candidate : candidates) {
        std::error_code error;
        const std::filesystem::path path{candidate};
        if (std::filesystem::is_regular_file(path / "style.css", error)) {
            return path;
        }
    }
    return std::nullopt;
}

bool runSlateRenderer(const std::filesystem::path& html,
                      const std::filesystem::path& png,
                      int width,
                      int height)
{
    const auto widthText = std::to_string(width);
    const auto heightText = std::to_string(height);
    const auto pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        execl("/usr/local/bin/render-slate-html",
              "render-slate-html",
              html.c_str(),
              png.c_str(),
              widthText.c_str(),
              heightText.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    int status{};
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string slateLine(const std::vector<std::string>& lines, std::size_t index)
{
    return index < lines.size() ? lines[index] : "";
}

FramePtr renderHtmlTemplateFrame(const RepeaterConfig& config,
                                 std::string_view templateName,
                                 const std::vector<std::pair<std::string_view, std::string>>& variables)
{
    const auto templateDirectory = findSlateTemplateDirectory();
    if (!templateDirectory.has_value()) {
        throw std::runtime_error{"no HTML slate template directory found"};
    }

    const auto templatePath = *templateDirectory / std::string{templateName};
    auto html = readTextFile(templatePath);
    if (!html.has_value()) {
        throw std::runtime_error{"HTML slate template not found: " + templatePath.string()};
    }
    replaceAll(*html, "href=\"style.css\"", "href=\"" + fileUri(*templateDirectory / "style.css") + "\"");
    for (const auto& [name, value] : variables) {
        replaceAll(*html, "{{" + std::string{name} + "}}", htmlEscape(value));
    }

    static std::atomic<std::uint64_t> serial{0};
    const auto unique = std::to_string(::getpid()) + "-" + std::to_string(serial.fetch_add(1));
    const auto base = std::filesystem::temp_directory_path() / ("wh-slate-" + unique);
    const auto htmlPath = base.string() + ".html";
    const auto pngPath = base.string() + ".png";

    {
        std::ofstream output{htmlPath};
        if (!output) {
            throw std::runtime_error{"create temporary HTML slate failed"};
        }
        output << *html;
    }

    struct TempCleanup {
        std::filesystem::path html;
        std::filesystem::path png;
        ~TempCleanup()
        {
            std::error_code error;
            std::filesystem::remove(html, error);
            std::filesystem::remove(png, error);
        }
    } cleanup{htmlPath, pngPath};

    if (!runSlateRenderer(htmlPath, pngPath, outputWidth(config), outputHeight(config))) {
        throw std::runtime_error{"HTML slate renderer failed"};
    }
    return decodeSlideFrame(config, pngPath);
}

FramePtr renderHtmlSlateFrame(const RepeaterConfig& config, std::string_view text, int frameRate)
{
    const auto lines = splitLines(text);
    const auto title = slateLine(lines, 0).empty() ? identText(config) : slateLine(lines, 0);
    const auto status = slateLine(lines, 1);
    std::string templateName{"diagnostic.html"};
    std::vector<std::pair<std::string_view, std::string>> variables{
        {"callsign", title},
        {"sleep_start", config.beaconSchedule.startTime},
        {"sleep_end", config.beaconSchedule.endTime},
        {"receiver_suffix", ""},
        {"input_label", slateLine(lines, 2)},
        {"frequency_khz", ""},
        {"symbol_rate_ks", ""},
        {"service_line", slateLine(lines, 4)},
        {"video_format", slateLine(lines, 5)},
        {"source_line", slateLine(lines, 2)},
        {"rf_line", slateLine(lines, 3)},
        {"modulation", slateLine(lines, 5)},
        {"diagnostic_title", status},
        {"diagnostic_line_1", slateLine(lines, 2)},
        {"diagnostic_line_2", slateLine(lines, 3)},
        {"diagnostic_line_3", slateLine(lines, 4)},
    };

    if (title == "Power Saving Mode") {
        templateName = "sleep.html";
    } else if (status.find("has just been accessed") != std::string::npos) {
        templateName = "post-access.html";
    } else if (status.find("Signal received") != std::string::npos) {
        templateName = "signal-received.html";
        if (status.size() > std::string_view{"Signal received"}.size()) {
            for (auto& [name, value] : variables) {
                if (name == "receiver_suffix") {
                    value = status.substr(std::string_view{"Signal received"}.size());
                }
            }
        }
        if (lines.size() > 3) {
            for (auto& [name, value] : variables) {
                if (name == "rf_line") {
                    value = slateLine(lines, 3);
                }
            }
        }
    }

    try {
        return renderHtmlTemplateFrame(config, templateName, variables);
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: HTML slate render failed, using built-in slate: " << ex.what() << '\n';
        return renderSlateFrame(config, text, frameRate);
    }
}

FramePtr renderHtmlTestcardFrame(const RepeaterConfig& config, int frameRate)
{
    try {
        return renderHtmlTemplateFrame(config,
                                       "testcard.html",
                                       {{"callsign", identText(config)}});
    } catch (const std::exception& ex) {
        std::cerr << "wh-repeater: HTML testcard render failed, using built-in testcard: " << ex.what() << '\n';
        return renderTestcardFrame(config, frameRate);
    }
}

std::string codecDescription(AVCodecID codecId)
{
    switch (codecId) {
    case AV_CODEC_ID_H264: return "H.264";
    case AV_CODEC_ID_HEVC: return "H.265/HEVC";
    case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2 video";
    default:
        if (const auto* name = avcodec_get_name(codecId); name != nullptr) {
            return name;
        }
        return "unknown";
    }
}

const AVCodec* findVideoDecoder(AVCodecID codecId)
{
    return avcodec_find_decoder(codecId);
}

int roundedFrameRate(AVRational rate)
{
    if (rate.num <= 0 || rate.den <= 0) {
        return 0;
    }
    return std::clamp(static_cast<int>(std::llround(av_q2d(rate))), 1, 50);
}

double sourceFrameRate(const AVStream& stream)
{
    const auto rate = stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0
        ? stream.avg_frame_rate
        : stream.r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        return 25.0;
    }
    return std::clamp(av_q2d(rate), 1.0, 120.0);
}

bool interlacedFieldOrder(AVFieldOrder order)
{
    return order == AV_FIELD_TT
        || order == AV_FIELD_BB
        || order == AV_FIELD_TB
        || order == AV_FIELD_BT;
}

int streamFrameRate(const AVStream& stream, const AVCodecContext& decoder)
{
    int frameRate = roundedFrameRate(decoder.framerate);
    if (frameRate <= 0) {
        const AVRational rate = stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0
            ? stream.avg_frame_rate
            : stream.r_frame_rate;
        frameRate = roundedFrameRate(rate);
    }
    if (frameRate <= 0) {
        frameRate = 25;
    }

    const auto fieldOrder = decoder.field_order != AV_FIELD_UNKNOWN
        ? decoder.field_order
        : stream.codecpar->field_order;
    if (interlacedFieldOrder(fieldOrder) && frameRate <= 30) {
        frameRate *= 2;
    }
    return std::clamp(frameRate, 1, 50);
}

AVRational validSampleAspect(AVRational frameAspect, AVRational streamAspect)
{
    if (frameAspect.num > 0 && frameAspect.den > 0) {
        return frameAspect;
    }
    if (streamAspect.num > 0 && streamAspect.den > 0) {
        return streamAspect;
    }
    return AVRational{1, 1};
}

int evenDimension(int value)
{
    return std::max(16, value & ~1);
}

std::vector<std::uint8_t> extractAnnexBH264ParameterSets(const std::uint8_t* data, int size)
{
    std::vector<std::uint8_t> result;
    if (data == nullptr || size <= 0) {
        return result;
    }

    auto startCodeAt = [data, size](int offset, int& prefixSize) {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
            prefixSize = 3;
            return true;
        }
        if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) {
            prefixSize = 4;
            return true;
        }
        return false;
    };

    auto findStartCode = [&startCodeAt, size](int from, int& prefixSize) {
        for (int index = std::max(0, from); index + 3 <= size; ++index) {
            if (startCodeAt(index, prefixSize)) {
                return index;
            }
        }
        return -1;
    };

    bool haveSps = false;
    bool havePps = false;
    int prefixSize = 0;
    for (int start = findStartCode(0, prefixSize); start >= 0;) {
        const auto nalStart = start + prefixSize;
        int nextPrefix = 0;
        const auto nextStart = findStartCode(nalStart, nextPrefix);
        const auto nalEnd = nextStart >= 0 ? nextStart : size;
        if (nalStart < nalEnd) {
            const auto nalType = data[nalStart] & 0x1f;
            if ((nalType == 7 && !haveSps) || (nalType == 8 && !havePps)) {
                static constexpr std::array<std::uint8_t, 4> annexBPrefix{0, 0, 0, 1};
                result.insert(result.end(), annexBPrefix.begin(), annexBPrefix.end());
                result.insert(result.end(), data + nalStart, data + nalEnd);
                haveSps = haveSps || nalType == 7;
                havePps = havePps || nalType == 8;
                if (haveSps && havePps) {
                    break;
                }
            }
        }
        start = nextStart;
        prefixSize = nextPrefix;
    }

    if (!haveSps || !havePps) {
        result.clear();
    }
    return result;
}

class PersistentRtmpMuxer {
public:
    explicit PersistentRtmpMuxer(const RepeaterConfig& config)
        : config_{config}
    {
    }

    ~PersistentRtmpMuxer()
    {
        close(false);
    }

    PersistentRtmpMuxer(const PersistentRtmpMuxer&) = delete;
    PersistentRtmpMuxer& operator=(const PersistentRtmpMuxer&) = delete;

    void configureVideo(const AVCodecContext& codec)
    {
        std::lock_guard lock{mutex_};
        if (!enabled()) {
            return;
        }
        if (videoConfigured_) {
            validateVideoProfileLocked(codec);
            return;
        }
        videoCodecParameters_.reset(avcodec_parameters_alloc());
        if (!videoCodecParameters_) {
            std::cerr << "wh-repeater: allocate RTMP video parameters failed\n";
            return;
        }
        if (avcodec_parameters_from_context(videoCodecParameters_.get(), &codec) < 0) {
            std::cerr << "wh-repeater: copy RTMP video parameters failed\n";
            videoCodecParameters_.reset();
            return;
        }
        videoTimeBase_ = codec.time_base;
        videoConfigured_ = true;
        std::cout << "media pipeline configured RTMP H.264 video at "
                  << codec.width << 'x' << codec.height
                  << " extradata=" << videoCodecParameters_->extradata_size << " bytes\n";
        openLocked();
    }

    void configureAudio(const AVCodecContext& codec)
    {
        std::lock_guard lock{mutex_};
        if (!enabled()) {
            return;
        }
        if (audioConfigured_) {
            validateAudioProfileLocked(codec);
            return;
        }
        audioCodecParameters_.reset(avcodec_parameters_alloc());
        if (!audioCodecParameters_) {
            std::cerr << "wh-repeater: allocate RTMP audio parameters failed\n";
            return;
        }
        if (avcodec_parameters_from_context(audioCodecParameters_.get(), &codec) < 0) {
            std::cerr << "wh-repeater: copy RTMP audio parameters failed\n";
            audioCodecParameters_.reset();
            return;
        }
        audioTimeBase_ = codec.time_base;
        audioConfigured_ = true;
        std::cout << "media pipeline configured RTMP AAC audio at " << codec.sample_rate << " Hz\n";
        openLocked();
    }

    void writeVideoPacket(const AVPacket* packet)
    {
        writePacket(packet, videoTimeBase_, videoTimestampOffset_, lastVideoDts_, true);
    }

    void writeAudioPacket(const AVPacket* packet)
    {
        writePacket(packet, audioTimeBase_, audioTimestampOffset_, lastAudioDts_, false);
    }

private:
    struct AvCodecParametersDeleter {
        void operator()(AVCodecParameters* parameters) const
        {
            avcodec_parameters_free(&parameters);
        }
    };

    using CodecParametersPtr = std::unique_ptr<AVCodecParameters, AvCodecParametersDeleter>;

    bool enabled() const
    {
        return config_.streaming.rtmp.enabled && !config_.streaming.rtmp.url.empty();
    }

    void validateVideoProfileLocked(const AVCodecContext& codec) const
    {
        if (!videoCodecParameters_) {
            return;
        }
        if (videoCodecParameters_->codec_id != codec.codec_id
            || videoCodecParameters_->width != codec.width
            || videoCodecParameters_->height != codec.height
            || videoCodecParameters_->format != codec.pix_fmt
            || videoTimeBase_.num != codec.time_base.num
            || videoTimeBase_.den != codec.time_base.den) {
            std::cerr << "wh-repeater: RTMP video profile mismatch ignored; output must remain "
                      << videoCodecParameters_->width << "x" << videoCodecParameters_->height
                      << " codec=" << avcodec_get_name(videoCodecParameters_->codec_id)
                      << " time_base=" << videoTimeBase_.num << '/' << videoTimeBase_.den
                      << '\n';
        }
    }

    void validateAudioProfileLocked(const AVCodecContext& codec) const
    {
        if (!audioCodecParameters_) {
            return;
        }
        const auto channelLayoutMatches = av_channel_layout_compare(&audioCodecParameters_->ch_layout, &codec.ch_layout) == 0;
        if (audioCodecParameters_->codec_id != codec.codec_id
            || audioCodecParameters_->sample_rate != codec.sample_rate
            || audioCodecParameters_->format != codec.sample_fmt
            || !channelLayoutMatches
            || audioTimeBase_.num != codec.time_base.num
            || audioTimeBase_.den != codec.time_base.den) {
            std::cerr << "wh-repeater: RTMP audio profile mismatch ignored; output must remain "
                      << audioCodecParameters_->sample_rate << " Hz codec="
                      << avcodec_get_name(audioCodecParameters_->codec_id)
                      << " time_base=" << audioTimeBase_.num << '/' << audioTimeBase_.den
                      << '\n';
        }
    }

    void openLocked()
    {
        if (!enabled() || !videoConfigured_ || headerWritten_) {
            return;
        }
        if (videoCodecParameters_ != nullptr
            && videoCodecParameters_->codec_id == AV_CODEC_ID_H264
            && videoCodecParameters_->extradata_size <= 0) {
            return;
        }
        if (std::chrono::steady_clock::now() < nextConnectAttempt_) {
            return;
        }

        AVFormatContext* rawFormat{};
        const auto allocStatus = avformat_alloc_output_context2(&rawFormat, nullptr, "flv", config_.streaming.rtmp.url.c_str());
        if (allocStatus < 0 || rawFormat == nullptr) {
            markConnectFailed("allocate RTMP output failed", allocStatus);
            return;
        }
        rtmpFormat_ = rawFormat;

        videoStream_ = avformat_new_stream(rtmpFormat_, nullptr);
        if (videoStream_ == nullptr || avcodec_parameters_copy(videoStream_->codecpar, videoCodecParameters_.get()) < 0) {
            std::cerr << "wh-repeater: create RTMP video stream failed\n";
            closeLocked(false);
            scheduleReconnect();
            return;
        }
        videoStream_->time_base = videoTimeBase_;

        if (audioConfigured_) {
            audioStream_ = avformat_new_stream(rtmpFormat_, nullptr);
            if (audioStream_ == nullptr || avcodec_parameters_copy(audioStream_->codecpar, audioCodecParameters_.get()) < 0) {
                std::cerr << "wh-repeater: create RTMP audio stream failed\n";
                audioStream_ = nullptr;
            } else {
                audioStream_->time_base = audioTimeBase_;
            }
        }

        if ((rtmpFormat_->oformat->flags & AVFMT_NOFILE) == 0) {
            AVDictionary* ioOptions = nullptr;
            av_dict_set(&ioOptions, "rw_timeout", "750000", 0);
            av_dict_set(&ioOptions, "rtmp_live", "live", 0);
            av_dict_set(&ioOptions, "rtmp_buffer", "100", 0);
            av_dict_set(&ioOptions, "tcp_nodelay", "1", 0);
            const auto openStatus = avio_open2(&rtmpFormat_->pb, config_.streaming.rtmp.url.c_str(), AVIO_FLAG_WRITE, nullptr, &ioOptions);
            av_dict_free(&ioOptions);
            if (openStatus < 0) {
                markConnectFailed("RTMP connect failed", openStatus);
                closeLocked(false);
                return;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
        av_dict_set(&options, "muxdelay", "0", 0);
        av_dict_set(&options, "muxpreload", "0", 0);
        rtmpFormat_->max_delay = 0;
        const auto headerStatus = avformat_write_header(rtmpFormat_, &options);
        av_dict_free(&options);
        if (headerStatus < 0) {
            markConnectFailed("write RTMP header failed", headerStatus);
            closeLocked(false);
            return;
        }

        headerWritten_ = true;
        std::cout << "media pipeline streaming RTMP to " << config_.streaming.rtmp.url << '\n';
    }

    void writePacket(const AVPacket* packet,
                     AVRational sourceTimeBase,
                     std::int64_t& timestampOffset,
                     std::int64_t& lastDts,
                     bool video)
    {
        std::lock_guard lock{mutex_};
        if (!enabled()) {
            return;
        }
        if (video) {
            seedH264ExtradataLocked(packet);
        }
        if (!headerWritten_) {
            openLocked();
        }
        auto* stream = video ? videoStream_ : audioStream_;
        if (!headerWritten_ || rtmpFormat_ == nullptr || stream == nullptr) {
            return;
        }

        PacketPtr copy{av_packet_clone(packet)};
        if (!copy) {
            std::cerr << "wh-repeater: clone RTMP packet failed\n";
            return;
        }
        av_packet_rescale_ts(copy.get(), sourceTimeBase, stream->time_base);
        const auto timestamp = copy->dts != AV_NOPTS_VALUE ? copy->dts : copy->pts;
        if (timestamp != AV_NOPTS_VALUE && lastDts == AV_NOPTS_VALUE && timestamp + timestampOffset < 0) {
            timestampOffset = -timestamp;
        }
        if (timestamp != AV_NOPTS_VALUE && lastDts != AV_NOPTS_VALUE && timestamp + timestampOffset <= lastDts) {
            const auto duration = copy->duration > 0 ? copy->duration : 1;
            timestampOffset = lastDts + duration - timestamp;
        }
        if (timestampOffset != 0) {
            if (copy->pts != AV_NOPTS_VALUE) {
                copy->pts += timestampOffset;
            }
            if (copy->dts != AV_NOPTS_VALUE) {
                copy->dts += timestampOffset;
            }
        }
        if (copy->dts != AV_NOPTS_VALUE) {
            lastDts = copy->dts;
        } else if (copy->pts != AV_NOPTS_VALUE) {
            lastDts = copy->pts;
        }
        copy->stream_index = stream->index;
        auto& logged = video ? loggedFirstVideoPacket_ : loggedFirstAudioPacket_;
        if (!logged) {
            std::cout << "media pipeline writing first RTMP " << (video ? "video" : "audio")
                      << " packet size=" << copy->size
                      << " pts=" << copy->pts
                      << " dts=" << copy->dts;
            if (video) {
                std::cout << " key=" << ((copy->flags & AV_PKT_FLAG_KEY) != 0 ? "yes" : "no");
            }
            std::cout << '\n';
            logged = true;
        }
        const auto writeStarted = std::chrono::steady_clock::now();
        const auto status = av_interleaved_write_frame(rtmpFormat_, copy.get());
        const auto writeDuration = std::chrono::steady_clock::now() - writeStarted;
        if (status < 0) {
            std::cerr << "wh-repeater: RTMP " << (video ? "video" : "audio")
                      << " write failed: " << avError(status) << '\n';
            closeLocked(false);
            scheduleReconnect();
        } else {
            if (writeDuration > std::chrono::milliseconds{20}) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextSlowWriteLog_) {
                    std::cerr << "wh-repeater: RTMP " << (video ? "video" : "audio")
                              << " write took "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(writeDuration).count()
                              << " ms\n";
                    nextSlowWriteLog_ = now + std::chrono::seconds{5};
                }
            }
        }
    }

    void seedH264ExtradataLocked(const AVPacket* packet)
    {
        if (packet == nullptr
            || videoCodecParameters_ == nullptr
            || videoCodecParameters_->codec_id != AV_CODEC_ID_H264
            || videoCodecParameters_->extradata_size > 0
            || (packet->flags & AV_PKT_FLAG_KEY) == 0) {
            return;
        }

        const auto extradata = extractAnnexBH264ParameterSets(packet->data, packet->size);
        if (extradata.empty()) {
            return;
        }
        auto* buffer = static_cast<std::uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (buffer == nullptr) {
            std::cerr << "wh-repeater: allocate RTMP H.264 parameter sets failed\n";
            return;
        }
        std::memcpy(buffer, extradata.data(), extradata.size());
        av_freep(&videoCodecParameters_->extradata);
        videoCodecParameters_->extradata = buffer;
        videoCodecParameters_->extradata_size = static_cast<int>(extradata.size());
        std::cout << "media pipeline seeded RTMP H.264 parameter sets from keyframe extradata="
                  << videoCodecParameters_->extradata_size << " bytes\n";
    }

    void markConnectFailed(std::string_view operation, int status)
    {
        std::cerr << "wh-repeater: " << operation << ": " << avError(status) << '\n';
        scheduleReconnect();
    }

    void scheduleReconnect()
    {
        nextConnectAttempt_ = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    }

    void close(bool writeTrailer = true)
    {
        std::lock_guard lock{mutex_};
        closeLocked(writeTrailer);
    }

    void closeLocked(bool writeTrailer)
    {
        if (headerWritten_ && writeTrailer && rtmpFormat_ != nullptr) {
            av_write_trailer(rtmpFormat_);
        }
        headerWritten_ = false;
        videoStream_ = nullptr;
        audioStream_ = nullptr;
        loggedFirstVideoPacket_ = false;
        loggedFirstAudioPacket_ = false;
        if (rtmpFormat_ != nullptr) {
            if ((rtmpFormat_->oformat->flags & AVFMT_NOFILE) == 0 && rtmpFormat_->pb != nullptr) {
                avio_closep(&rtmpFormat_->pb);
            }
            avformat_free_context(rtmpFormat_);
            rtmpFormat_ = nullptr;
        }
    }

    const RepeaterConfig& config_;
    std::mutex mutex_;
    CodecParametersPtr videoCodecParameters_;
    CodecParametersPtr audioCodecParameters_;
    AVRational videoTimeBase_{};
    AVRational audioTimeBase_{};
    AVFormatContext* rtmpFormat_{nullptr};
    AVStream* videoStream_{nullptr};
    AVStream* audioStream_{nullptr};
    std::chrono::steady_clock::time_point nextConnectAttempt_{};
    std::int64_t videoTimestampOffset_{0};
    std::int64_t audioTimestampOffset_{0};
    std::int64_t lastVideoDts_{AV_NOPTS_VALUE};
    std::int64_t lastAudioDts_{AV_NOPTS_VALUE};
    std::chrono::steady_clock::time_point nextSlowWriteLog_{};
    bool videoConfigured_{false};
    bool audioConfigured_{false};
    bool headerWritten_{false};
    bool loggedFirstVideoPacket_{false};
    bool loggedFirstAudioPacket_{false};
};

class EncodedOutputSink {
public:
    virtual ~EncodedOutputSink() = default;
    [[nodiscard]] virtual AVPixelFormat pixelFormat() const = 0;
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
    [[nodiscard]] int frameRate() const
    {
        return std::max(1, frameRateValue());
    }
    [[nodiscard]] virtual int frameRateValue() const = 0;
    [[nodiscard]] virtual AVCodecContext* audioCodec() const = 0;
    [[nodiscard]] virtual std::int64_t nextVideoPts() const = 0;
    [[nodiscard]] virtual std::int64_t nextAudioPts() const = 0;
    virtual void setStreamInfo(std::optional<std::string> streamInfo) = 0;
    virtual void beginSubmittedSource(std::string_view, bool)
    {
    }
    virtual void endSubmittedSource(std::string_view)
    {
    }
    virtual void setSubmittedSourceHasAudio(bool)
    {
    }
    virtual void submitVideoFrame(AVFrame* frame, std::string_view source) = 0;
    virtual void submitAudioFrame(AVFrame* frame, std::string_view source) = 0;
};

class H264OutputMuxer {
public:
    H264OutputMuxer(const RepeaterConfig& config,
                    PlutoSink& output,
                    PersistentRtmpMuxer* rtmp,
                    int width,
                    int height,
                    int frameRate,
                    std::optional<std::string> streamInfo,
                    bool softwareEncoderOnly = false)
        : config_{config}
        , output_{output}
        , rtmp_{rtmp}
        , width_{width}
        , height_{height}
        , frameRate_{frameRate}
        , streamInfo_{std::move(streamInfo)}
        , softwareEncoderOnly_{softwareEncoderOnly}
    {
        initialise();
    }

    ~H264OutputMuxer()
    {
        try {
            flush();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: live transcode flush failed: " << ex.what() << '\n';
        }
        if (headerWritten_) {
            av_write_trailer(format_);
        }
        if (codec_) {
            avcodec_free_context(&codec_);
        }
        if (audioCodec_) {
            avcodec_free_context(&audioCodec_);
        }
        if (format_) {
            if (format_->pb != nullptr) {
                avio_context_free(&format_->pb);
            }
            avformat_free_context(format_);
        }
        av_free(avioBuffer_);
    }

    H264OutputMuxer(const H264OutputMuxer&) = delete;
    H264OutputMuxer& operator=(const H264OutputMuxer&) = delete;

    [[nodiscard]] AVPixelFormat pixelFormat() const
    {
        return softwarePixelFormatForEncoder(*codec_);
    }

    [[nodiscard]] int width() const
    {
        return codec_->width;
    }

    [[nodiscard]] int height() const
    {
        return codec_->height;
    }

    [[nodiscard]] AVRational timeBase() const
    {
        return codec_->time_base;
    }

    [[nodiscard]] int frameRate() const
    {
        return frameRate_;
    }

    [[nodiscard]] AVCodecContext* audioCodec() const
    {
        return audioCodec_;
    }

    void encode(AVFrame* frame)
    {
        FramePtr generatedFrame;
        auto* safeFrame = fixedVideoFrame(frame, "live", generatedFrame);
        auto overlayFrame = identOverlay_->render(safeFrame);
        if (streamInfoOverlay_ != nullptr && overlayFrame->pts < static_cast<std::int64_t>(std::max(1, frameRate_) * 10)) {
            overlayFrame = streamInfoOverlay_->render(overlayFrame.get());
        }
        FramePtr hardwareFrame;
        AVFrame* encoderFrame = overlayFrame.get();
        if (codec_->pix_fmt == AV_PIX_FMT_VAAPI) {
            hardwareFrame = uploadVaapiFrame(*codec_, overlayFrame.get(), encoderUploadScaler_);
            encoderFrame = hardwareFrame.get();
        }
        checkAv(avcodec_send_frame(codec_, encoderFrame), "send transcode frame");
        drainPackets();
    }

    void encodeAudio(AVFrame* frame)
    {
        if (audioCodec_ == nullptr) {
            return;
        }
        FramePtr generatedFrame;
        auto* safeFrame = fixedAudioFrame(frame, "live", generatedFrame);
        checkAv(avcodec_send_frame(audioCodec_, safeFrame), "send live audio frame");
        drainAudioPackets();
    }

    void flush()
    {
        if (codec_ == nullptr || flushed_) {
            return;
        }
        const auto status = avcodec_send_frame(codec_, nullptr);
        if (status >= 0) {
            drainPackets();
        }
        if (audioCodec_ != nullptr) {
            const auto audioStatus = avcodec_send_frame(audioCodec_, nullptr);
            if (audioStatus >= 0) {
                drainAudioPackets();
            }
        }
        flushed_ = true;
    }

private:
    FramePtr generatedVideoFrame(std::int64_t pts)
    {
        FramePtr frame{av_frame_alloc()};
        if (!frame) {
            throw std::runtime_error{"allocate generated live frame failed"};
        }
        frame->format = softwarePixelFormatForEncoder(*codec_);
        frame->width = codec_->width;
        frame->height = codec_->height;
        frame->pts = pts;
        checkAv(av_frame_get_buffer(frame.get(), 32), "allocate generated live frame buffer");
        checkAv(av_frame_make_writable(frame.get()), "make generated live frame writable");
        fillBlue(*frame);
        normaliseVideoFrameProperties(*frame);
        return frame;
    }

    AVFrame* fixedVideoFrame(AVFrame* frame, std::string_view source, FramePtr& generatedFrame)
    {
        const auto pts = nextOutputVideoPts(lastVideoPts_);

        const bool valid = frame != nullptr
            && frame->format == softwarePixelFormatForEncoder(*codec_)
            && frame->width == codec_->width
            && frame->height == codec_->height
            && frame->data[0] != nullptr
            && frame->linesize[0] > 0;

        lastVideoPts_ = pts;
        if (!valid) {
            std::cerr << "wh-repeater: " << source
                      << " frame rejected before encoder; sending generated frame\n";
            generatedFrame = generatedVideoFrame(pts);
            return generatedFrame.get();
        }
        frame->pts = pts;
        normaliseVideoFrameProperties(*frame);
        return frame;
    }

    FramePtr generatedAudioFrame(std::int64_t pts, int samples)
    {
        FramePtr frame{av_frame_alloc()};
        if (!frame) {
            throw std::runtime_error{"allocate generated live audio frame failed"};
        }
        frame->format = audioCodec_->sample_fmt;
        frame->sample_rate = audioCodec_->sample_rate;
        frame->nb_samples = std::max(1, samples);
        frame->pts = pts;
        checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec_->ch_layout), "copy generated live audio layout");
        checkAv(av_frame_get_buffer(frame.get(), 0), "allocate generated live audio buffer");
        checkAv(av_frame_make_writable(frame.get()), "make generated live audio writable");
        for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
            if (frame->extended_data[channel] != nullptr) {
                std::memset(frame->extended_data[channel], 0, static_cast<std::size_t>(frame->linesize[0]));
            }
        }
        return frame;
    }

    AVFrame* fixedAudioFrame(AVFrame* frame, std::string_view source, FramePtr& generatedFrame)
    {
        const auto expectedSamples = audioCodec_->frame_size > 0 ? audioCodec_->frame_size : 1024;
        const auto pts = lastAudioPts_ == AV_NOPTS_VALUE ? 0 : lastAudioPts_ + expectedSamples;

        const bool valid = frame != nullptr
            && frame->format == audioCodec_->sample_fmt
            && frame->sample_rate == audioCodec_->sample_rate
            && frame->nb_samples == expectedSamples
            && av_channel_layout_compare(&frame->ch_layout, &audioCodec_->ch_layout) == 0
            && frame->extended_data != nullptr
            && frame->extended_data[0] != nullptr;

        lastAudioPts_ = pts;
        if (!valid) {
            std::cerr << "wh-repeater: " << source
                      << " audio frame rejected before encoder; sending silence\n";
            generatedFrame = generatedAudioFrame(pts, expectedSamples);
            return generatedFrame.get();
        }
        frame->pts = pts;
        return frame;
    }

    void initialise()
    {
        checkAv(avformat_alloc_output_context2(&format_, nullptr, "mpegts", nullptr), "allocate live MPEG-TS muxer");
        avioBuffer_ = static_cast<std::uint8_t*>(av_malloc(32768));
        if (avioBuffer_ == nullptr) {
            throw std::runtime_error{"allocate live MPEG-TS IO buffer failed"};
        }
        format_->pb = avio_alloc_context(avioBuffer_, 32768, 1, &output_, nullptr, plutoWritePacket, nullptr);
        if (format_->pb == nullptr) {
            throw std::runtime_error{"allocate live MPEG-TS AVIO failed"};
        }
        avioBuffer_ = nullptr;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;

        stream_ = avformat_new_stream(format_, nullptr);
        if (stream_ == nullptr) {
            throw std::runtime_error{"create live video stream failed"};
        }

        const auto frameRate = std::clamp(frameRate_, 1, 50);
        const auto videoBitrateKbps = fallbackVideoBitrateKbps(config_);
        if (softwareEncoderOnly_) {
            throw std::runtime_error{"software H.264 encode requested but hardware encode is mandatory"};
        }
        auto encoder = openH264Encoder(*format_, config_, width_, height_, frameRate, videoBitrateKbps);
        codec_ = encoder.context;
        identOverlay_ = std::make_unique<OverlayRenderer>(identFilter(identText(config_)),
                                                          codec_->width,
                                                          codec_->height,
                                                          softwarePixelFormatForEncoder(*codec_),
                                                          frameRate);
        if (streamInfo_.has_value() && !streamInfo_->empty()) {
            streamInfoOverlay_ = std::make_unique<OverlayRenderer>(streamInfoFilter(*streamInfo_, frameRate),
                                                                   codec_->width,
                                                                   codec_->height,
                                                                   softwarePixelFormatForEncoder(*codec_),
                                                                   frameRate);
        }
        std::cout << "media pipeline transcoding received video to H.264 with encoder "
                  << encoder.name << " at " << width_ << "x" << height_ << '\n';
        checkAv(avcodec_parameters_from_context(stream_->codecpar, codec_), "copy live encoder parameters");
        stream_->time_base = codec_->time_base;
        initialiseAudio();

        AVDictionary* options = nullptr;
        av_dict_set(&options, "muxdelay", "0", 0);
        av_dict_set(&options, "muxpreload", "0", 0);
        av_dict_set(&options, "pcr_period", "40", 0);
        av_dict_set(&options, "mpegts_flags", "initial_discontinuity+system_b", 0);
        format_->max_delay = 0;
        const auto headerStatus = avformat_write_header(format_, &options);
        av_dict_free(&options);
        checkAv(headerStatus, "write live MPEG-TS header");
        headerWritten_ = true;
        if (rtmp_ != nullptr) {
            if (audioCodec_ != nullptr) {
                rtmp_->configureAudio(*audioCodec_);
            }
            rtmp_->configureVideo(*codec_);
        }
    }

    void initialiseAudio()
    {
        const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (encoder == nullptr) {
            std::cerr << "wh-repeater: AAC encoder unavailable; live audio disabled\n";
            return;
        }

        audioStream_ = avformat_new_stream(format_, nullptr);
        if (audioStream_ == nullptr) {
            throw std::runtime_error{"create live audio stream failed"};
        }

        audioCodec_ = avcodec_alloc_context3(encoder);
        if (audioCodec_ == nullptr) {
            throw std::runtime_error{"allocate live AAC encoder failed"};
        }
        audioCodec_->codec_type = AVMEDIA_TYPE_AUDIO;
        audioCodec_->sample_rate = defaultOutputAudioSampleRate;
        audioCodec_->bit_rate = static_cast<std::int64_t>(std::max(32U, config_.pluto.audioBitrateKbps)) * 1000;
        audioCodec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&audioCodec_->ch_layout, outputAudioChannels(config_));
        audioCodec_->time_base = AVRational{1, defaultOutputAudioSampleRate};
        if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audioCodec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        checkAv(avcodec_open2(audioCodec_, encoder, nullptr), "open live AAC encoder");
        checkAv(avcodec_parameters_from_context(audioStream_->codecpar, audioCodec_), "copy live audio parameters");
        audioStream_->time_base = audioCodec_->time_base;
        audioFrameSamples_ = audioCodec_->frame_size > 0 ? audioCodec_->frame_size : 1024;
    }

    void drainPackets()
    {
        PacketPtr packet{av_packet_alloc()};
        if (!packet) {
            throw std::runtime_error{"allocate live packet failed"};
        }
        for (;;) {
            const auto status = avcodec_receive_packet(codec_, packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            checkAv(status, "receive live packet");
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->dts = packet->pts;
            }
            if (rtmp_ != nullptr) {
                rtmp_->writeVideoPacket(packet.get());
            }
            writeTransportPacket(packet.get());
            av_packet_unref(packet.get());
        }
    }

    void drainAudioPackets()
    {
        PacketPtr packet{av_packet_alloc()};
        if (!packet) {
            throw std::runtime_error{"allocate live audio packet failed"};
        }
        for (;;) {
            const auto status = avcodec_receive_packet(audioCodec_, packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            checkAv(status, "receive live audio packet");
            if (rtmp_ != nullptr) {
                rtmp_->writeAudioPacket(packet.get());
            }
            writeTransportAudioPacket(packet.get());
            av_packet_unref(packet.get());
        }
    }

    void writeTransportPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, codec_->time_base, stream_->time_base);
        packet->duration = av_rescale_q(1, codec_->time_base, stream_->time_base);
        packet->stream_index = stream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write live MPEG-TS packet");
    }

    void writeTransportAudioPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, audioCodec_->time_base, audioStream_->time_base);
        packet->stream_index = audioStream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write live MPEG-TS audio packet");
    }

    const RepeaterConfig& config_;
    PlutoSink& output_;
    PersistentRtmpMuxer* rtmp_{};
    int width_{};
    int height_{};
    int frameRate_{};
    std::optional<std::string> streamInfo_;
    bool softwareEncoderOnly_{false};
    AVFormatContext* format_{nullptr};
    AVCodecContext* codec_{nullptr};
    AVCodecContext* audioCodec_{nullptr};
    AVStream* stream_{nullptr};
    AVStream* audioStream_{nullptr};
    std::unique_ptr<OverlayRenderer> identOverlay_;
    std::unique_ptr<OverlayRenderer> streamInfoOverlay_;
    SwsContextPtr encoderUploadScaler_;
    std::uint8_t* avioBuffer_{nullptr};
    bool headerWritten_{false};
    bool flushed_{false};
    int audioFrameSamples_{1024};
    std::int64_t lastVideoPts_{AV_NOPTS_VALUE};
    std::int64_t lastAudioPts_{AV_NOPTS_VALUE};
};

class LiveTranscoder {
public:
    LiveTranscoder(RepeaterConfig config, EncodedOutputSink& output, std::optional<ActiveInput> active)
        : config_{std::move(config)}
        , output_{output}
        , active_{std::move(active)}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~LiveTranscoder()
    {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LiveTranscoder(const LiveTranscoder&) = delete;
    LiveTranscoder& operator=(const LiveTranscoder&) = delete;

    void append(std::span<const std::byte> data)
    {
        input_.append(data);
    }

    void stop()
    {
        stopping_.store(true);
        input_.stop();
    }

    [[nodiscard]] bool finished() const
    {
        return finished_.load();
    }

    [[nodiscard]] bool ready() const
    {
        return ready_.load();
    }

    [[nodiscard]] std::string error() const
    {
        return error_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point lastVideoFrameAt() const
    {
        const auto value = lastVideoFrameSteadyMs_.load();
        if (value <= 0) {
            return {};
        }
        return std::chrono::steady_clock::time_point{std::chrono::milliseconds{value}};
    }

    [[nodiscard]] std::optional<std::string> streamInfo() const
    {
        std::lock_guard lock{streamInfoMutex_};
        return streamInfo_;
    }

private:
    static std::int64_t steadyClockMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void run()
    {
        try {
            transcodeLoop();
        } catch (const std::exception& ex) {
            error_ = ex.what();
            std::cerr << "wh-repeater: live transcode stopped: " << ex.what() << '\n';
        }
        finished_.store(true);
    }

    void transcodeLoop()
    {
        AVFormatContext* rawInput = avformat_alloc_context();
        if (rawInput == nullptr) {
            throw std::runtime_error{"allocate live input context failed"};
        }
        InputFormatPtr inputFormat{rawInput};

        auto* avioBuffer = static_cast<std::uint8_t*>(av_malloc(32768));
        if (avioBuffer == nullptr) {
            throw std::runtime_error{"allocate live input AVIO buffer failed"};
        }
        rawInput->pb = avio_alloc_context(avioBuffer, 32768, 0, &input_, readLiveTsPacket, nullptr, nullptr);
        if (rawInput->pb == nullptr) {
            av_free(avioBuffer);
            throw std::runtime_error{"allocate live input AVIO failed"};
        }
        rawInput->interrupt_callback.callback = interruptLiveInput;
        rawInput->interrupt_callback.opaque = this;
        rawInput->flags |= AVFMT_FLAG_CUSTOM_IO;

        const auto* demuxer = av_find_input_format("mpegts");
        AVFormatContext* openTarget = inputFormat.get();
        AVDictionary* inputOptions = nullptr;
        av_dict_set(&inputOptions, "probesize", "20000000", 0);
        av_dict_set(&inputOptions, "analyzeduration", "5000000", 0);
        const auto openStatus = avformat_open_input(&openTarget, nullptr, demuxer, &inputOptions);
        av_dict_free(&inputOptions);
        checkAv(openStatus, "open received MPEG-TS");
        inputFormat.release();
        inputFormat.reset(openTarget);

        CodecContextPtr decoderContext;
        CodecContextPtr audioDecoderContext;
        int videoStreamIndex = AVERROR_STREAM_NOT_FOUND;
        int audioStreamIndex = AVERROR_STREAM_NOT_FOUND;
        auto frame = FramePtr{av_frame_alloc()};
        auto convertedFrame = FramePtr{av_frame_alloc()};
        auto packet = PacketPtr{av_packet_alloc()};
        if (!frame || !convertedFrame || !packet) {
            throw std::runtime_error{"allocate live transcode buffers failed"};
        }

        SwsContextPtr scaler;
        SwrContextPtr audioResampler;
        AudioFifoPtr audioFifo;
        auto audioFrame = FramePtr{av_frame_alloc()};
        auto convertedAudioFrame = FramePtr{av_frame_alloc()};
        std::int64_t audioPts = AV_NOPTS_VALUE;
        std::int64_t frameIndex = 0;
        auto videoDecodeStartedAt = std::chrono::steady_clock::time_point{};
        auto lastDecodedFrameAt = std::chrono::steady_clock::time_point{};
        auto nextStreamInfoRefreshAt = std::chrono::steady_clock::now();
        while (true) {
            const auto readStatus = av_read_frame(inputFormat.get(), packet.get());
            if (readStatus == AVERROR_EOF) {
                break;
            }
            if (readStatus == AVERROR(EAGAIN) || readStatus == AVERROR_INVALIDDATA) {
                continue;
            }
            checkAv(readStatus, "read received MPEG-TS packet");

            if (!decoderContext && packet->stream_index >= 0) {
                maybeInitialiseVideoDecoder(inputFormat.get(),
                                            packet->stream_index,
                                            decoderContext,
                                            convertedFrame.get(),
                                            videoStreamIndex);
            }
            if (decoderContext && videoStreamIndex >= 0
                && std::chrono::steady_clock::now() >= nextStreamInfoRefreshAt) {
                refreshStreamInfo(*inputFormat, *inputFormat->streams[videoStreamIndex]);
                nextStreamInfoRefreshAt = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
            }
            if (decoderContext && !audioDecoderContext && packet->stream_index >= 0) {
                maybeInitialiseAudioDecoder(inputFormat.get(), packet->stream_index, audioDecoderContext, audioStreamIndex);
            }

            if (decoderContext && packet->stream_index == videoStreamIndex) {
                if (videoDecodeStartedAt == std::chrono::steady_clock::time_point{}) {
                    videoDecodeStartedAt = std::chrono::steady_clock::now();
                }
                const auto decodedFrames = decodePacket(decoderContext.get(),
                                                        packet.get(),
                                                        frame.get(),
                                                        convertedFrame.get(),
                                                        scaler,
                                                        frameIndex);
                if (decodedFrames > 0) {
                    lastDecodedFrameAt = std::chrono::steady_clock::now();
                }
                enforceLiveVideoProgress(videoDecodeStartedAt, lastDecodedFrameAt);
            } else if (audioDecoderContext && packet->stream_index == audioStreamIndex) {
                decodeAudioPacket(audioDecoderContext.get(),
                                  packet.get(),
                                  audioFrame.get(),
                                  convertedAudioFrame.get(),
                                  audioResampler,
                                  audioFifo,
                                  audioPts);
            }
            av_packet_unref(packet.get());
        }

        if (decoderContext) {
            checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush live decoder");
            const auto decodedFrames = drainDecoder(decoderContext.get(),
                                                    frame.get(),
                                                    convertedFrame.get(),
                                                    scaler,
                                                    frameIndex);
            if (decodedFrames > 0) {
                lastDecodedFrameAt = std::chrono::steady_clock::now();
            }
        }
        if (audioDecoderContext) {
            checkAv(avcodec_send_packet(audioDecoderContext.get(), nullptr), "flush live audio decoder");
            drainAudioDecoder(audioDecoderContext.get(),
                              audioFrame.get(),
                              convertedAudioFrame.get(),
                              audioResampler,
                              audioFifo,
                              audioPts);
            flushAudioResampler(audioResampler, convertedAudioFrame.get(), audioFifo, audioPts);
            discardPartialAudioFifo(audioFifo);
        }
    }

    static int interruptLiveInput(void* opaque)
    {
        auto* self = static_cast<LiveTranscoder*>(opaque);
        return self != nullptr && self->stopping_.load() ? 1 : 0;
    }

    bool maybeInitialiseVideoDecoder(AVFormatContext* inputFormat,
                                     int streamIndex,
                                     CodecContextPtr& decoderContext,
                                     AVFrame* convertedFrame,
                                     int& videoStreamIndex)
    {
        if (streamIndex < 0 || streamIndex >= static_cast<int>(inputFormat->nb_streams)) {
            return false;
        }
        auto* stream = inputFormat->streams[streamIndex];
        if (stream == nullptr || stream->codecpar == nullptr) {
            return false;
        }
        const auto codecId = stream->codecpar->codec_id;
        logLiveStreamOnce(*stream);
        const auto knownVideo = codecId == AV_CODEC_ID_H264
            || codecId == AV_CODEC_ID_HEVC
            || codecId == AV_CODEC_ID_MPEG2VIDEO;
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && !knownVideo) {
            return false;
        }

        const auto* decoder = avcodec_find_decoder(codecId);
        if (decoder == nullptr) {
            std::cerr << "wh-repeater: no decoder available for received " << codecDescription(codecId) << '\n';
            return false;
        }

        CodecContextPtr context{avcodec_alloc_context3(decoder)};
        if (!context) {
            throw std::runtime_error{"allocate live decoder failed"};
        }
        checkAv(avcodec_parameters_to_context(context.get(), stream->codecpar), "copy live decoder parameters");
        checkAv(avcodec_open2(context.get(), decoder, nullptr), "open live video decoder");

        const auto decodedWidth = evenDimension(stream->codecpar->width > 0 ? stream->codecpar->width : outputWidth(config_));
        const auto decodedHeight = evenDimension(stream->codecpar->height > 0 ? stream->codecpar->height : outputHeight(config_));
        const auto frameRate = streamFrameRate(*stream, *context);
        std::cout << "media pipeline detected received " << codecDescription(codecId)
                  << " video, decoding with " << (decoder->name == nullptr ? "default" : decoder->name)
                  << " at " << decodedWidth << "x" << decodedHeight << " " << frameRate << " fps\n";

        liveVideoDetails_ = LiveVideoDetails{
            .codec = codecDescription(codecId),
            .width = decodedWidth,
            .height = decodedHeight,
            .frameRate = frameRate,
        };
        refreshStreamInfo(*inputFormat, *stream);
        convertedFrame->format = output_.pixelFormat();
        convertedFrame->width = output_.width();
        convertedFrame->height = output_.height();
        checkAv(av_frame_get_buffer(convertedFrame, 32), "allocate live converted frame buffer");

        videoStreamIndex = streamIndex;
        decoderContext = std::move(context);
        return true;
    }

    void refreshStreamInfo(const AVFormatContext& inputFormat, const AVStream& stream)
    {
        std::optional<std::string> streamInfo;
        if (active_.has_value() && liveVideoDetails_.has_value()) {
            streamInfo = streamInfoText(*active_,
                                        liveVideoDetails_->codec,
                                        liveVideoDetails_->width,
                                        liveVideoDetails_->height,
                                        liveVideoDetails_->frameRate,
                                        inputFormat,
                                        stream);
        }

        {
            std::lock_guard lock{streamInfoMutex_};
            if (streamInfo_ != streamInfo) {
                streamInfo_ = streamInfo;
            }
        }
    }

    bool maybeInitialiseAudioDecoder(AVFormatContext* inputFormat,
                                     int streamIndex,
                                     CodecContextPtr& audioDecoderContext,
                                     int& audioStreamIndex)
    {
        if (streamIndex < 0 || streamIndex >= static_cast<int>(inputFormat->nb_streams)) {
            return false;
        }
        auto* stream = inputFormat->streams[streamIndex];
        if (stream == nullptr || stream->codecpar == nullptr) {
            return false;
        }
        const auto codecId = stream->codecpar->codec_id;
        logLiveStreamOnce(*stream);
        const auto knownAudio = codecId == AV_CODEC_ID_MP2
            || codecId == AV_CODEC_ID_MP3
            || codecId == AV_CODEC_ID_AAC;
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO && !knownAudio) {
            return false;
        }
        if (stream->codecpar->sample_rate <= 0 || stream->codecpar->ch_layout.nb_channels <= 0) {
            return false;
        }

        const auto* audioDecoder = avcodec_find_decoder(codecId);
        if (audioDecoder == nullptr) {
            std::cerr << "wh-repeater: no decoder available for received audio stream\n";
            return false;
        }

        CodecContextPtr context{avcodec_alloc_context3(audioDecoder)};
        if (!context) {
            throw std::runtime_error{"allocate live audio decoder failed"};
        }
        checkAv(avcodec_parameters_to_context(context.get(), stream->codecpar), "copy live audio parameters");
        checkAv(avcodec_open2(context.get(), audioDecoder, nullptr), "open live audio decoder");
        std::cout << "media pipeline detected received "
                  << (avcodec_get_name(stream->codecpar->codec_id) == nullptr ? "audio" : avcodec_get_name(stream->codecpar->codec_id))
                  << " audio at " << context->sample_rate << " Hz\n";

        audioStreamIndex = streamIndex;
        audioDecoderContext = std::move(context);
        return true;
    }

    void logLiveStreamOnce(const AVStream& stream)
    {
        const auto index = static_cast<int>(stream.index);
        if (std::ranges::find(loggedLiveStreams_, index) != loggedLiveStreams_.end()) {
            return;
        }
        loggedLiveStreams_.push_back(index);
        const auto* type = av_get_media_type_string(stream.codecpar->codec_type);
        std::cout << "media pipeline live stream index=" << index
                  << " pid=0x" << std::hex << stream.id << std::dec
                  << " type=" << (type == nullptr ? "unknown" : type)
                  << " codec=" << avcodec_get_name(stream.codecpar->codec_id)
                  << '\n';
    }

    void decodeAudioPacket(AVCodecContext* decoder,
                           AVPacket* packet,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           std::int64_t& audioPts)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, audioPts);
            const auto retryStatus = avcodec_send_packet(decoder, packet);
            if (retryStatus == AVERROR_INVALIDDATA) {
                return;
            }
            checkAv(retryStatus, "send received audio packet after drain");
        } else if (sendStatus == AVERROR_INVALIDDATA) {
            return;
        } else {
            checkAv(sendStatus, "send received audio packet");
        }
        drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, audioPts);
    }

    void drainAudioDecoder(AVCodecContext* decoder,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           std::int64_t& audioPts)
    {
        for (;;) {
            if (stopping_.load()) {
                break;
            }
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            if (receiveStatus == AVERROR_INVALIDDATA) {
                continue;
            }
            checkAv(receiveStatus, "receive decoded live audio frame");
            if (!ready_.load()) {
                av_frame_unref(frame);
                continue;
            }
            queueAudioFrame(frame, convertedFrame, resampler, fifo, audioPts);
            av_frame_unref(frame);
        }
    }

    void queueAudioFrame(AVFrame* frame,
                         AVFrame* convertedFrame,
                         SwrContextPtr& resampler,
                         AudioFifoPtr& fifo,
                         std::int64_t& audioPts)
    {
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr) {
            return;
        }
        if (audioPts == AV_NOPTS_VALUE) {
            audioPts = output_.nextAudioPts();
        }
        if (!resampler) {
            AVChannelLayout inputLayout{};
            if (frame->ch_layout.nb_channels > 0) {
                checkAv(av_channel_layout_copy(&inputLayout, &frame->ch_layout), "copy live audio input layout");
            } else {
                av_channel_layout_default(&inputLayout, 1);
            }

            SwrContext* rawResampler{};
            checkAv(swr_alloc_set_opts2(&rawResampler,
                                        &audioCodec->ch_layout,
                                        audioCodec->sample_fmt,
                                        audioCodec->sample_rate,
                                        &inputLayout,
                                        static_cast<AVSampleFormat>(frame->format),
                                        frame->sample_rate,
                                        0,
                                        nullptr),
                    "allocate live audio resampler");
            av_channel_layout_uninit(&inputLayout);
            resampler.reset(rawResampler);
            checkAv(swr_init(resampler.get()), "initialise live audio resampler");

            fifo.reset(av_audio_fifo_alloc(audioCodec->sample_fmt, audioCodec->ch_layout.nb_channels, audioCodec->frame_size));
            if (!fifo) {
                throw std::runtime_error{"allocate live audio FIFO failed"};
            }
            audioPts = output_.nextAudioPts();
        }

        const auto delay = swr_get_delay(resampler.get(), frame->sample_rate);
        const auto maxOutputSamples = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples,
                                                                      audioCodec->sample_rate,
                                                                      frame->sample_rate,
                                                                      AV_ROUND_UP));
        av_frame_unref(convertedFrame);
        convertedFrame->format = audioCodec->sample_fmt;
        convertedFrame->sample_rate = audioCodec->sample_rate;
        convertedFrame->nb_samples = std::max(1, maxOutputSamples);
        checkAv(av_channel_layout_copy(&convertedFrame->ch_layout, &audioCodec->ch_layout), "copy live converted audio layout");
        checkAv(av_frame_get_buffer(convertedFrame, 0), "allocate live converted audio frame");

        const auto convertedSamples = swr_convert(resampler.get(),
                                                  convertedFrame->extended_data,
                                                  convertedFrame->nb_samples,
                                                  const_cast<const std::uint8_t**>(frame->extended_data),
                                                  frame->nb_samples);
        checkAv(convertedSamples, "resample live audio frame");
        convertedFrame->nb_samples = convertedSamples;

        checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + convertedSamples), "grow live audio FIFO");
        if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(convertedFrame->extended_data), convertedSamples) < convertedSamples) {
            throw std::runtime_error{"write live audio FIFO failed"};
        }
        drainAudioFifo(fifo, audioPts, false);
    }

    void drainAudioFifo(AudioFifoPtr& fifo,
                        std::int64_t& audioPts,
                        bool flush)
    {
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr || !fifo) {
            return;
        }
        const auto frameSamples = audioCodec->frame_size > 0 ? audioCodec->frame_size : 1024;
        while (av_audio_fifo_size(fifo.get()) >= frameSamples || (flush && av_audio_fifo_size(fifo.get()) > 0)) {
            const auto samples = flush ? std::min(frameSamples, av_audio_fifo_size(fifo.get())) : frameSamples;
            FramePtr frame{av_frame_alloc()};
            if (!frame) {
                throw std::runtime_error{"allocate live encoded audio frame failed"};
            }
            frame->format = audioCodec->sample_fmt;
            frame->sample_rate = audioCodec->sample_rate;
            frame->nb_samples = samples;
            frame->pts = audioPts;
            checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec->ch_layout), "copy live encoded audio layout");
            checkAv(av_frame_get_buffer(frame.get(), 0), "allocate live encoded audio frame buffer");
            if (av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->extended_data), samples) < samples) {
                throw std::runtime_error{"read live audio FIFO failed"};
            }
            audioPts += samples;
            if (stopping_.load()) {
                return;
            }
            output_.submitAudioFrame(frame.get(), "live");
        }
    }

    void flushAudioResampler(SwrContextPtr& resampler,
                             AVFrame* convertedFrame,
                             AudioFifoPtr& fifo,
                             std::int64_t& audioPts)
    {
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr || !resampler || !fifo) {
            return;
        }

        for (;;) {
            const auto delay = swr_get_delay(resampler.get(), audioCodec->sample_rate);
            if (delay <= 0) {
                break;
            }
            const auto maxOutputSamples = static_cast<int>(av_rescale_rnd(delay,
                                                                          audioCodec->sample_rate,
                                                                          audioCodec->sample_rate,
                                                                          AV_ROUND_UP));
            if (maxOutputSamples <= 0) {
                break;
            }

            av_frame_unref(convertedFrame);
            convertedFrame->format = audioCodec->sample_fmt;
            convertedFrame->sample_rate = audioCodec->sample_rate;
            convertedFrame->nb_samples = maxOutputSamples;
            checkAv(av_channel_layout_copy(&convertedFrame->ch_layout, &audioCodec->ch_layout), "copy flushed live audio layout");
            checkAv(av_frame_get_buffer(convertedFrame, 0), "allocate flushed live audio frame");

            const auto convertedSamples = swr_convert(resampler.get(),
                                                      convertedFrame->extended_data,
                                                      convertedFrame->nb_samples,
                                                      nullptr,
                                                      0);
            checkAv(convertedSamples, "flush live audio resampler");
            if (convertedSamples <= 0) {
                break;
            }

            checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + convertedSamples),
                    "grow flushed live audio FIFO");
            if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(convertedFrame->extended_data), convertedSamples) < convertedSamples) {
                throw std::runtime_error{"write flushed live audio FIFO failed"};
            }
            drainAudioFifo(fifo, audioPts, false);
        }
    }

    void discardPartialAudioFifo(AudioFifoPtr& fifo)
    {
        if (fifo && av_audio_fifo_size(fifo.get()) > 0) {
            std::cerr << "wh-repeater: dropping partial live audio frame at source boundary\n";
        }
    }

    int decodePacket(AVCodecContext* decoder,
                     AVPacket* packet,
                     AVFrame* frame,
                     AVFrame* convertedFrame,
                     SwsContextPtr& scaler,
                     std::int64_t& frameIndex)
    {
        int decodedFrames = 0;
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            decodedFrames += drainDecoder(decoder, frame, convertedFrame, scaler, frameIndex);
            const auto retryStatus = avcodec_send_packet(decoder, packet);
            if (retryStatus == AVERROR(EAGAIN)) {
                return decodedFrames;
            }
            if (retryStatus == AVERROR_INVALIDDATA) {
                return decodedFrames;
            }
            checkAv(retryStatus, "send received packet after drain");
        } else if (sendStatus == AVERROR_INVALIDDATA) {
            return decodedFrames;
        } else {
            checkAv(sendStatus, "send received packet");
        }
        decodedFrames += drainDecoder(decoder, frame, convertedFrame, scaler, frameIndex);
        return decodedFrames;
    }

    int drainDecoder(AVCodecContext* decoder,
                     AVFrame* frame,
                     AVFrame* convertedFrame,
                     SwsContextPtr& scaler,
                     std::int64_t& frameIndex)
    {
        int decodedFrames = 0;
        for (;;) {
            if (stopping_.load()) {
                break;
            }
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            if (receiveStatus == AVERROR_INVALIDDATA) {
                continue;
            }
            checkAv(receiveStatus, "receive decoded live frame");
            lastVideoFrameSteadyMs_.store(steadyClockMs());
            ready_.store(true);
            if (stopping_.load()) {
                av_frame_unref(frame);
                break;
            }
            output_.submitVideoFrame(convertFrame(frame, convertedFrame, scaler, frameIndex++), "live");
            lastVideoFrameSteadyMs_.store(steadyClockMs());
            ++decodedFrames;
            av_frame_unref(frame);
        }
        return decodedFrames;
    }

    void enforceLiveVideoProgress(std::chrono::steady_clock::time_point videoDecodeStartedAt,
                                  std::chrono::steady_clock::time_point lastDecodedFrameAt) const
    {
        if (videoDecodeStartedAt == std::chrono::steady_clock::time_point{}) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (lastDecodedFrameAt == std::chrono::steady_clock::time_point{}) {
            return;
        }
        constexpr auto liveVideoStallTimeout = std::chrono::seconds{3};
        if (ready_.load() && now - lastDecodedFrameAt > liveVideoStallTimeout) {
            return;
        }
    }

    AVFrame* convertFrame(AVFrame* frame,
                          AVFrame* convertedFrame,
                          SwsContextPtr& scaler,
                          std::int64_t pts)
    {
        pts = output_.nextVideoPts();
        if (frame->format == output_.pixelFormat()
            && frame->width == output_.width()
            && frame->height == output_.height()) {
            frame->pts = pts;
            normaliseVideoFrameProperties(*frame);
            return frame;
        }

        checkAv(av_frame_make_writable(convertedFrame), "make live converted frame writable");
        if (!scaler) {
            scaler.reset(sws_getContext(frame->width,
                                        frame->height,
                                        static_cast<AVPixelFormat>(frame->format),
                                        output_.width(),
                                        output_.height(),
                                        output_.pixelFormat(),
                                        SWS_FAST_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr));
            if (!scaler) {
                throw std::runtime_error{"create live video scaler failed"};
            }
        }
        sws_scale(scaler.get(),
                  frame->data,
                  frame->linesize,
                  0,
                  frame->height,
                  convertedFrame->data,
                  convertedFrame->linesize);
        convertedFrame->pts = pts;
        normaliseVideoFrameProperties(*convertedFrame);
        return convertedFrame;
    }

    RepeaterConfig config_;
    EncodedOutputSink& output_;
    std::optional<ActiveInput> active_;
    BlockingTsInput input_;
    std::atomic_bool stopping_{false};
    std::atomic_bool ready_{false};
    std::atomic_bool finished_{false};
    std::atomic<std::int64_t> lastVideoFrameSteadyMs_{0};
    std::string error_;
    mutable std::mutex streamInfoMutex_;
    std::optional<std::string> streamInfo_;
    struct LiveVideoDetails {
        std::string codec;
        int width{};
        int height{};
        int frameRate{};
    };
    std::optional<LiveVideoDetails> liveVideoDetails_;
    std::vector<int> loggedLiveStreams_;
    std::thread thread_;
};

class FallbackVideoTranscoder {
public:
    FallbackVideoTranscoder(RepeaterConfig config,
                            EncodedOutputSink& output,
                            std::filesystem::path path,
                            std::function<void(std::optional<std::string>)> statusCallback)
        : config_{std::move(config)}
        , output_{output}
        , path_{std::move(path)}
        , statusCallback_{std::move(statusCallback)}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~FallbackVideoTranscoder()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FallbackVideoTranscoder(const FallbackVideoTranscoder&) = delete;
    FallbackVideoTranscoder& operator=(const FallbackVideoTranscoder&) = delete;

    [[nodiscard]] bool finished() const
    {
        return finished_.load();
    }

    void seekTo(std::chrono::milliseconds position)
    {
        std::lock_guard lock{seekMutex_};
        pendingSeek_ = std::max(std::chrono::milliseconds{0}, position);
    }

private:
    static int interruptPlayback(void* opaque)
    {
        auto* self = static_cast<FallbackVideoTranscoder*>(opaque);
        return self != nullptr && self->stopping_.load() ? 1 : 0;
    }

    void run()
    {
        try {
            transcodeLoop();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: fallback video stopped: " << ex.what() << '\n';
        }
        output_.endSubmittedSource("fallback-video");
        publishStopped();
        finished_.store(true);
    }

    std::optional<std::chrono::milliseconds> takePendingSeek()
    {
        std::lock_guard lock{seekMutex_};
        auto seek = pendingSeek_;
        pendingSeek_.reset();
        return seek;
    }

    [[nodiscard]] std::chrono::milliseconds duration() const
    {
        return duration_;
    }

    void publishStopped()
    {
        if (statusCallback_) {
            statusCallback_(std::nullopt);
        }
    }

    void publishStatus(std::chrono::milliseconds position)
    {
        if (!statusCallback_) {
            return;
        }
        const auto durationMs = duration().count();
        std::ostringstream status;
        status << "{"
               << "\"playing\":true,"
               << "\"path\":" << jsonString(path_.string()) << ","
               << "\"name\":" << jsonString(path_.filename().string()) << ","
               << "\"positionMs\":" << std::max<std::int64_t>(0, position.count()) << ","
               << "\"durationMs\":";
        if (durationMs > 0) {
            status << durationMs;
        } else {
            status << "null";
        }
        status << ",\"timecode\":" << jsonString(formatTimecode(position))
               << ",\"durationTimecode\":";
        if (durationMs > 0) {
            status << jsonString(formatTimecode(std::chrono::milliseconds{durationMs}));
        } else {
            status << "null";
        }
        status << "}";
        statusCallback_(status.str());
    }

    void transcodeLoop()
    {
        if (path_.empty()) {
            throw std::runtime_error{"no fallback video path configured"};
        }
        if (!std::filesystem::exists(path_)) {
            throw std::runtime_error{"fallback video does not exist: " + path_.string()};
        }

        AVFormatContext* rawInput = avformat_alloc_context();
        if (rawInput == nullptr) {
            throw std::runtime_error{"allocate fallback video input context failed"};
        }
        rawInput->interrupt_callback.callback = interruptPlayback;
        rawInput->interrupt_callback.opaque = this;
        const auto openStatus = avformat_open_input(&rawInput, path_.c_str(), nullptr, nullptr);
        if (openStatus < 0) {
            avformat_close_input(&rawInput);
            checkAv(openStatus, "open fallback video " + path_.string());
        }
        InputFormatPtr inputFormat{rawInput};
        checkAv(avformat_find_stream_info(inputFormat.get(), nullptr), "probe fallback video streams");
        duration_ = inputFormat->duration > 0
            ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds{inputFormat->duration})
            : std::chrono::milliseconds{0};

        const auto videoStreamIndex = av_find_best_stream(inputFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        checkAv(videoStreamIndex, "find fallback video stream");
        auto* videoStream = inputFormat->streams[videoStreamIndex];
        const auto codecId = videoStream->codecpar->codec_id;
        const auto* softwareDecoder = avcodec_find_decoder(codecId);
        if (softwareDecoder == nullptr) {
            throw std::runtime_error{"no decoder available for fallback " + codecDescription(codecId)};
        }

        const auto* decoder = softwareDecoder;
        bool usingHardwareDecoder = false;
        bool usingVaapiHardwareDecoder = false;
        if (config_.fallback.hardwareDecode) {
            if (config_.mode == "pc-gateway"
                && codecId == AV_CODEC_ID_H264
                && videoStream->codecpar != nullptr
                && videoStream->codecpar->width == outputWidth(config_)
                && videoStream->codecpar->height == outputHeight(config_)) {
                try {
                    AVBufferRef* probeDevice = createVaapiDevice();
                    av_buffer_unref(&probeDevice);
                    usingHardwareDecoder = true;
                    usingVaapiHardwareDecoder = true;
                    std::cout << "media pipeline fallback VAAPI decode candidate accepted: H.264 "
                              << videoStream->codecpar->width << "x" << videoStream->codecpar->height << '\n';
                } catch (const std::exception& ex) {
                    std::cout << "media pipeline fallback VAAPI decode skipped: " << ex.what() << '\n';
                }
            }
            if (!usingVaapiHardwareDecoder) {
                std::cout << "media pipeline fallback hardware decode skipped: VAAPI exact-size H.264 is the only supported hardware file-decode path\n";
            }
        }

        const auto openVideoDecoder = [&](const AVCodec* selectedDecoder, std::string_view label, bool vaapiDecode) {
            CodecContextPtr context{avcodec_alloc_context3(selectedDecoder)};
            if (!context) {
                throw std::runtime_error{"allocate fallback " + std::string{label} + " video decoder failed"};
            }
            checkAv(avcodec_parameters_to_context(context.get(), videoStream->codecpar),
                    "copy fallback " + std::string{label} + " video parameters");
            if (vaapiDecode) {
                context->get_format = chooseVaapiPixelFormat;
                AVBufferRef* rawDevice = createVaapiDevice();
                context->hw_device_ctx = av_buffer_ref(rawDevice);
                av_buffer_unref(&rawDevice);
                if (context->hw_device_ctx == nullptr) {
                    throw std::runtime_error{"reference fallback VAAPI decode device failed"};
                }
            }
            const auto status = avcodec_open2(context.get(), selectedDecoder, nullptr);
            if (status < 0) {
                throw std::runtime_error{"open fallback " + std::string{label} + " video decoder "
                                         + selectedDecoder->name + ": " + avError(status)};
            }
            return context;
        };

        CodecContextPtr decoderContext;
        try {
            decoderContext = openVideoDecoder(decoder, usingHardwareDecoder ? "hardware" : "software", usingVaapiHardwareDecoder);
        } catch (const std::exception& ex) {
            if (!usingHardwareDecoder) {
                throw;
            }
            std::cerr << "wh-repeater: fallback hardware decode failed before playback; using software decode: "
                      << ex.what() << '\n';
            decoder = softwareDecoder;
            usingHardwareDecoder = false;
            usingVaapiHardwareDecoder = false;
            decoderContext = openVideoDecoder(decoder, "software", false);
        }

        auto audioStreamIndex = av_find_best_stream(inputFormat.get(), AVMEDIA_TYPE_AUDIO, -1, videoStreamIndex, nullptr, 0);
        CodecContextPtr audioDecoderContext;
        AVStream* audioStream{};
        if (audioStreamIndex >= 0) {
            audioStream = inputFormat->streams[audioStreamIndex];
            const auto* audioDecoder = avcodec_find_decoder(audioStream->codecpar->codec_id);
            if (audioDecoder != nullptr) {
                audioDecoderContext.reset(avcodec_alloc_context3(audioDecoder));
                if (!audioDecoderContext) {
                    throw std::runtime_error{"allocate fallback audio decoder failed"};
                }
                checkAv(avcodec_parameters_to_context(audioDecoderContext.get(), audioStream->codecpar), "copy fallback audio parameters");
                checkAv(avcodec_open2(audioDecoderContext.get(), audioDecoder, nullptr), "open fallback audio decoder");
            } else {
                std::cerr << "wh-repeater: no decoder available for fallback audio stream\n";
                audioStreamIndex = AVERROR_STREAM_NOT_FOUND;
            }
        }
        output_.beginSubmittedSource("fallback-video", audioDecoderContext != nullptr);
        output_.setSubmittedSourceHasAudio(audioDecoderContext != nullptr);

        const auto decodedWidth = evenDimension(decoderContext->width > 0 ? decoderContext->width : videoStream->codecpar->width);
        const auto decodedHeight = evenDimension(decoderContext->height > 0 ? decoderContext->height : videoStream->codecpar->height);
        const auto streamInfo = std::string{"Fallback video\n"} + path_.filename().string() + "\n"
            + codecDescription(codecId) + " " + std::to_string(decodedWidth) + "x" + std::to_string(decodedHeight)
            + (usingHardwareDecoder ? "\nhardware decode" : "\nsoftware decode");
        output_.setStreamInfo(streamInfo);

        std::cout << "media pipeline playing fallback video " << path_
                  << " as " << outputWidth(config_) << "x" << outputHeight(config_)
                  << " " << outputFrameRate(config_) << " fps using " << decoder->name << " decoder\n";

        auto frame = FramePtr{av_frame_alloc()};
        auto convertedFrame = FramePtr{av_frame_alloc()};
        auto audioFrame = FramePtr{av_frame_alloc()};
        auto convertedAudioFrame = FramePtr{av_frame_alloc()};
        auto packet = PacketPtr{av_packet_alloc()};
        if (!frame || !convertedFrame || !audioFrame || !convertedAudioFrame || !packet) {
            throw std::runtime_error{"allocate fallback video buffers failed"};
        }

        convertedFrame->format = output_.pixelFormat();
        convertedFrame->width = output_.width();
        convertedFrame->height = output_.height();
        checkAv(av_frame_get_buffer(convertedFrame.get(), 32), "allocate fallback video converted frame");

        SwsContextPtr scaler;
        SwrContextPtr audioResampler;
        AudioFifoPtr audioFifo;
        const auto playbackDeadlineFor = [this](std::chrono::milliseconds position) -> std::optional<std::chrono::steady_clock::time_point> {
            if (duration_.count() <= 0) {
                return std::nullopt;
            }
            const auto remaining = std::max(std::chrono::milliseconds{0}, duration_ - position);
            return std::chrono::steady_clock::now() + remaining + std::chrono::seconds{1};
        };
        auto playbackDeadline = playbackDeadlineFor(std::chrono::milliseconds{0});
        std::int64_t frameIndex = 0;
        std::int64_t decodedVideoFrames = 0;
        std::int64_t audioPts = 0;
        std::int64_t outputVideoBasePts = output_.nextVideoPts();
        std::int64_t outputAudioBasePts = output_.nextAudioPts();
        std::optional<double> firstVideoSeconds;
        std::int64_t lastVideoPts = -1;
        std::chrono::milliseconds playbackPositionBase{0};
        const auto fallbackSourceFrameRate = sourceFrameRate(*videoStream);
        const auto streamSampleAspect = validSampleAspect(videoStream->sample_aspect_ratio,
                                                          videoStream->codecpar->sample_aspect_ratio);
        while (!stopping_.load()) {
            if (auto seek = takePendingSeek(); seek.has_value()) {
                const auto seekMicros = static_cast<std::int64_t>(seek->count()) * 1000;
                const auto seekStatus = av_seek_frame(inputFormat.get(), -1, seekMicros, AVSEEK_FLAG_BACKWARD);
                if (seekStatus < 0) {
                    std::cerr << "wh-repeater: fallback video seek failed: " << avError(seekStatus) << '\n';
                } else {
                    avcodec_flush_buffers(decoderContext.get());
                    if (audioDecoderContext) {
                        avcodec_flush_buffers(audioDecoderContext.get());
                    }
                    audioResampler.reset();
                    audioFifo.reset();
                    firstVideoSeconds.reset();
                    lastVideoPts = -1;
                    decodedVideoFrames = 0;
                    frameIndex = 0;
                    audioPts = 0;
                    playbackPositionBase = *seek;
                    output_.beginSubmittedSource("fallback-video", audioDecoderContext != nullptr);
                    output_.setSubmittedSourceHasAudio(audioDecoderContext != nullptr);
                    outputVideoBasePts = output_.nextVideoPts();
                    outputAudioBasePts = output_.nextAudioPts();
                    playbackDeadline = playbackDeadlineFor(*seek);
                    publishStatus(*seek);
                }
            }
            if (playbackDeadline.has_value() && std::chrono::steady_clock::now() >= *playbackDeadline) {
                break;
            }
            const auto readStatus = av_read_frame(inputFormat.get(), packet.get());
            if (readStatus == AVERROR_EOF) {
                break;
            }
            if (readStatus == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            checkAv(readStatus, "read fallback video packet");
            if (packet->stream_index == videoStreamIndex) {
                decodePacket(decoderContext.get(),
                             packet.get(),
                             frame.get(),
                             convertedFrame.get(),
                             scaler,
                             output_,
                             *videoStream,
                             streamSampleAspect,
                             fallbackSourceFrameRate,
                             firstVideoSeconds,
                             lastVideoPts,
                             decodedVideoFrames,
                             frameIndex,
                             outputVideoBasePts,
                             playbackPositionBase);
            } else if (audioDecoderContext && packet->stream_index == audioStreamIndex) {
                decodeAudioPacket(audioDecoderContext.get(),
                                  packet.get(),
                                  audioFrame.get(),
                                  convertedAudioFrame.get(),
                                  audioResampler,
                                  audioFifo,
                                  output_,
                                  audioPts,
                                  outputAudioBasePts);
            }
            av_packet_unref(packet.get());
        }

        checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush fallback video decoder");
        drainDecoder(decoderContext.get(),
                     frame.get(),
                     convertedFrame.get(),
                     scaler,
                     output_,
                     *videoStream,
                     streamSampleAspect,
                     fallbackSourceFrameRate,
                     firstVideoSeconds,
                     lastVideoPts,
                     decodedVideoFrames,
                     frameIndex,
                     outputVideoBasePts,
                     playbackPositionBase);
        if (audioDecoderContext) {
            checkAv(avcodec_send_packet(audioDecoderContext.get(), nullptr), "flush fallback audio decoder");
            drainAudioDecoder(audioDecoderContext.get(),
                              audioFrame.get(),
                              convertedAudioFrame.get(),
                              audioResampler,
                              audioFifo,
                              output_,
                              audioPts,
                              outputAudioBasePts);
            flushAudioResampler(audioResampler, convertedAudioFrame.get(), audioFifo, output_, audioPts, outputAudioBasePts);
            flushAudioFifo(audioFifo, output_, audioPts, outputAudioBasePts);
        }
    }

    void decodePacket(AVCodecContext* decoder,
                      AVPacket* packet,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      EncodedOutputSink& outputMuxer,
                      const AVStream& videoStream,
                      AVRational streamSampleAspect,
                      double fallbackSourceFrameRate,
                      std::optional<double>& firstVideoSeconds,
                      std::int64_t& lastVideoPts,
                      std::int64_t& decodedVideoFrames,
                      std::int64_t& frameIndex,
                      std::int64_t outputVideoBasePts,
                      std::chrono::milliseconds playbackPositionBase)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainDecoder(decoder,
                         frame,
                         convertedFrame,
                         scaler,
                         outputMuxer,
                         videoStream,
                         streamSampleAspect,
                         fallbackSourceFrameRate,
                         firstVideoSeconds,
                         lastVideoPts,
                         decodedVideoFrames,
                         frameIndex,
                         outputVideoBasePts,
                         playbackPositionBase);
            checkAv(avcodec_send_packet(decoder, packet), "send fallback video packet after drain");
        } else {
            checkAv(sendStatus, "send fallback video packet");
        }
        drainDecoder(decoder,
                     frame,
                     convertedFrame,
                     scaler,
                     outputMuxer,
                     videoStream,
                     streamSampleAspect,
                     fallbackSourceFrameRate,
                     firstVideoSeconds,
                     lastVideoPts,
                     decodedVideoFrames,
                     frameIndex,
                     outputVideoBasePts,
                     playbackPositionBase);
    }

    void drainDecoder(AVCodecContext* decoder,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      EncodedOutputSink& outputMuxer,
                      const AVStream& videoStream,
                      AVRational streamSampleAspect,
                      double fallbackSourceFrameRate,
                      std::optional<double>& firstVideoSeconds,
                      std::int64_t& lastVideoPts,
                      std::int64_t& decodedVideoFrames,
                      std::int64_t& frameIndex,
                      std::int64_t outputVideoBasePts,
                      std::chrono::milliseconds playbackPositionBase)
    {
        for (;;) {
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            checkAv(receiveStatus, "receive fallback video frame");
            auto pts = sourceVideoPts(*frame,
                                      videoStream,
                                      outputMuxer,
                                      fallbackSourceFrameRate,
                                      decodedVideoFrames,
                                      firstVideoSeconds,
                                      frameIndex);
            ++decodedVideoFrames;
            if (pts <= lastVideoPts) {
                av_frame_unref(frame);
                continue;
            }
            lastVideoPts = pts;
            waitForOutputVideoLead(outputMuxer, pts, outputVideoBasePts);
            if (stopping_.load()) {
                av_frame_unref(frame);
                break;
            }
            outputMuxer.submitVideoFrame(convertFrame(frame, convertedFrame, scaler, outputMuxer, streamSampleAspect, pts), "fallback-video");
            const auto positionMs = static_cast<std::int64_t>(
                std::llround(static_cast<double>(pts) * 1000.0 / static_cast<double>(outputMuxer.frameRate())));
            publishStatus(playbackPositionBase + std::chrono::milliseconds{std::max<std::int64_t>(0, positionMs)});
            frameIndex = pts + 1;
            av_frame_unref(frame);
            if (stopping_.load()) {
                break;
            }
        }
    }

    std::int64_t sourceVideoPts(AVFrame& frame,
                                const AVStream& videoStream,
                                const EncodedOutputSink& outputMuxer,
                                double fallbackSourceFrameRate,
                                std::int64_t decodedVideoFrames,
                                std::optional<double>& firstVideoSeconds,
                                std::int64_t fallbackFrameIndex)
    {
        std::int64_t pts = fallbackFrameIndex;
        const auto sourceTimestamp = frame.best_effort_timestamp != AV_NOPTS_VALUE
            ? frame.best_effort_timestamp
            : frame.pts;
        if (sourceTimestamp != AV_NOPTS_VALUE && videoStream.time_base.num > 0 && videoStream.time_base.den > 0) {
            const auto seconds = static_cast<double>(sourceTimestamp) * av_q2d(videoStream.time_base);
            if (!firstVideoSeconds.has_value()) {
                firstVideoSeconds = seconds;
            }
            pts = static_cast<std::int64_t>(std::llround((seconds - *firstVideoSeconds)
                                                        * static_cast<double>(outputMuxer.frameRate())));
        } else if (fallbackSourceFrameRate > 0.0) {
            pts = static_cast<std::int64_t>(std::llround(static_cast<double>(decodedVideoFrames)
                                                        * static_cast<double>(outputMuxer.frameRate())
                                                        / fallbackSourceFrameRate));
        }
        return std::max<std::int64_t>(0, pts);
    }

    void waitForOutputVideoLead(const EncodedOutputSink& outputMuxer,
                                std::int64_t pts,
                                std::int64_t outputBasePts)
    {
        // Fallback-video playback is paced by output PTS, not queue depth or
        // decoder/file speed. Keep this aligned with docs/media-stream-contract.md.
        const auto maxLeadFrames = std::int64_t{2};
        while (!stopping_.load() && pts >= (outputMuxer.nextVideoPts() - outputBasePts) + maxLeadFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    AVFrame* convertFrame(AVFrame* frame,
                          AVFrame* convertedFrame,
                          SwsContextPtr& scaler,
                          const EncodedOutputSink& outputMuxer,
                          AVRational streamSampleAspect,
                          std::int64_t pts)
    {
        if (static_cast<AVPixelFormat>(frame->format) == AV_PIX_FMT_VAAPI) {
            av_frame_unref(convertedFrame);
            convertedFrame->format = outputMuxer.pixelFormat();
            convertedFrame->width = frame->width;
            convertedFrame->height = frame->height;
            checkAv(av_frame_get_buffer(convertedFrame, 32), "allocate fallback VAAPI download frame");
            checkAv(av_hwframe_transfer_data(convertedFrame, frame, 0), "download fallback VAAPI frame");
            convertedFrame->pts = frame->pts;
            convertedFrame->sample_aspect_ratio = frame->sample_aspect_ratio;
            normaliseVideoFrameProperties(*convertedFrame);
            frame = convertedFrame;
        }

        const auto sampleAspect = validSampleAspect(frame->sample_aspect_ratio, streamSampleAspect);
        const auto sourceFormat = normalisedPixelFormat(static_cast<AVPixelFormat>(frame->format));
        if (frame->width == outputMuxer.width()
            && frame->height == outputMuxer.height()
            && sourceFormat == outputMuxer.pixelFormat()
            && sampleAspect.num == sampleAspect.den) {
            frame->pts = pts;
            return frame;
        }

        checkAv(av_frame_make_writable(convertedFrame), "make fallback video converted frame writable");
        fillBlack(*convertedFrame);

        const auto displayWidth = static_cast<double>(frame->width)
            * static_cast<double>(sampleAspect.num) / static_cast<double>(sampleAspect.den);
        const auto scale = std::min(static_cast<double>(outputMuxer.width()) / displayWidth,
                                    static_cast<double>(outputMuxer.height()) / static_cast<double>(frame->height));
        auto dstWidth = std::max(2, static_cast<int>(std::floor(displayWidth * scale))) & ~1;
        auto dstHeight = std::max(2, static_cast<int>(std::floor(frame->height * scale))) & ~1;
        dstWidth = std::min(dstWidth, outputMuxer.width());
        dstHeight = std::min(dstHeight, outputMuxer.height());
        const auto dstX = ((outputMuxer.width() - dstWidth) / 2) & ~1;
        const auto dstY = ((outputMuxer.height() - dstHeight) / 2) & ~1;

        if (!scaler) {
            scaler.reset(sws_getContext(frame->width,
                                        frame->height,
                                        sourceFormat,
                                        dstWidth,
                                        dstHeight,
                                        outputMuxer.pixelFormat(),
                                        SWS_FAST_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr));
            if (!scaler) {
                throw std::runtime_error{"create fallback video scaler failed"};
            }
        }
        std::array<std::uint8_t*, 4> dstData{
            convertedFrame->data[0] + dstY * convertedFrame->linesize[0] + dstX,
            convertedFrame->data[1] + (dstY / 2) * convertedFrame->linesize[1] + (dstX / 2),
            convertedFrame->data[2] + (dstY / 2) * convertedFrame->linesize[2] + (dstX / 2),
            nullptr,
        };
        std::array<int, 4> dstLinesize{
            convertedFrame->linesize[0],
            convertedFrame->linesize[1],
            convertedFrame->linesize[2],
            0,
        };
        sws_scale(scaler.get(),
                  frame->data,
                  frame->linesize,
                  0,
                  frame->height,
                  dstData.data(),
                  dstLinesize.data());
        convertedFrame->pts = pts;
        return convertedFrame;
    }

    void decodeAudioPacket(AVCodecContext* decoder,
                           AVPacket* packet,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           EncodedOutputSink& outputMuxer,
                           std::int64_t& audioPts,
                           std::int64_t outputAudioBasePts)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, outputMuxer, audioPts, outputAudioBasePts);
            checkAv(avcodec_send_packet(decoder, packet), "send fallback audio packet after drain");
        } else {
            checkAv(sendStatus, "send fallback audio packet");
        }
        drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, outputMuxer, audioPts, outputAudioBasePts);
    }

    void drainAudioDecoder(AVCodecContext* decoder,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           EncodedOutputSink& outputMuxer,
                           std::int64_t& audioPts,
                           std::int64_t outputAudioBasePts)
    {
        for (;;) {
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            checkAv(receiveStatus, "receive fallback audio frame");
            queueAudioFrame(frame, convertedFrame, resampler, fifo, outputMuxer, audioPts, outputAudioBasePts);
            av_frame_unref(frame);
        }
    }

    void queueAudioFrame(AVFrame* frame,
                         AVFrame* convertedFrame,
                         SwrContextPtr& resampler,
                         AudioFifoPtr& fifo,
                         EncodedOutputSink& outputMuxer,
                         std::int64_t& audioPts,
                         std::int64_t outputAudioBasePts)
    {
        auto* audioCodec = outputMuxer.audioCodec();
        if (audioCodec == nullptr) {
            return;
        }
        if (!resampler) {
            AVChannelLayout inputLayout{};
            if (frame->ch_layout.nb_channels > 0) {
                checkAv(av_channel_layout_copy(&inputLayout, &frame->ch_layout), "copy fallback audio input layout");
            } else {
                av_channel_layout_default(&inputLayout, 1);
            }

            SwrContext* rawResampler{};
            checkAv(swr_alloc_set_opts2(&rawResampler,
                                        &audioCodec->ch_layout,
                                        audioCodec->sample_fmt,
                                        audioCodec->sample_rate,
                                        &inputLayout,
                                        static_cast<AVSampleFormat>(frame->format),
                                        frame->sample_rate,
                                        0,
                                        nullptr),
                    "allocate fallback audio resampler");
            av_channel_layout_uninit(&inputLayout);
            resampler.reset(rawResampler);
            checkAv(swr_init(resampler.get()), "initialise fallback audio resampler");

            fifo.reset(av_audio_fifo_alloc(audioCodec->sample_fmt, audioCodec->ch_layout.nb_channels, audioCodec->frame_size));
            if (!fifo) {
                throw std::runtime_error{"allocate fallback audio FIFO failed"};
            }
        }

        const auto delay = swr_get_delay(resampler.get(), frame->sample_rate);
        const auto maxOutputSamples = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples,
                                                                      audioCodec->sample_rate,
                                                                      frame->sample_rate,
                                                                      AV_ROUND_UP));
        av_frame_unref(convertedFrame);
        convertedFrame->format = audioCodec->sample_fmt;
        convertedFrame->sample_rate = audioCodec->sample_rate;
        convertedFrame->nb_samples = std::max(1, maxOutputSamples);
        checkAv(av_channel_layout_copy(&convertedFrame->ch_layout, &audioCodec->ch_layout), "copy fallback converted audio layout");
        checkAv(av_frame_get_buffer(convertedFrame, 0), "allocate fallback converted audio frame");

        const auto convertedSamples = swr_convert(resampler.get(),
                                                  convertedFrame->extended_data,
                                                  convertedFrame->nb_samples,
                                                  const_cast<const std::uint8_t**>(frame->extended_data),
                                                  frame->nb_samples);
        checkAv(convertedSamples, "resample fallback audio frame");
        convertedFrame->nb_samples = convertedSamples;

        checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + convertedSamples), "grow fallback audio FIFO");
        if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(convertedFrame->extended_data), convertedSamples) < convertedSamples) {
            throw std::runtime_error{"write fallback audio FIFO failed"};
        }
        drainAudioFifo(fifo, outputMuxer, audioPts, outputAudioBasePts, false);
    }

    void drainAudioFifo(AudioFifoPtr& fifo,
                        EncodedOutputSink& outputMuxer,
                        std::int64_t& audioPts,
                        std::int64_t outputAudioBasePts,
                        bool flush)
    {
        auto* audioCodec = outputMuxer.audioCodec();
        if (audioCodec == nullptr || !fifo) {
            return;
        }
        const auto frameSamples = audioCodec->frame_size > 0 ? audioCodec->frame_size : 1024;
        while (av_audio_fifo_size(fifo.get()) >= frameSamples || (flush && av_audio_fifo_size(fifo.get()) > 0)) {
            const auto samples = flush ? std::min(frameSamples, av_audio_fifo_size(fifo.get())) : frameSamples;
            FramePtr frame{av_frame_alloc()};
            if (!frame) {
                throw std::runtime_error{"allocate fallback encoded audio frame failed"};
            }
            frame->format = audioCodec->sample_fmt;
            frame->sample_rate = audioCodec->sample_rate;
            frame->nb_samples = samples;
            frame->pts = audioPts;
            checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec->ch_layout), "copy fallback encoded audio layout");
            checkAv(av_frame_get_buffer(frame.get(), 0), "allocate fallback encoded audio frame buffer");
            if (av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->extended_data), samples) < samples) {
                throw std::runtime_error{"read fallback audio FIFO failed"};
            }
            audioPts += samples;
            waitForOutputAudioLead(outputMuxer, frame->pts, outputAudioBasePts, *audioCodec);
            if (stopping_.load()) {
                return;
            }
            outputMuxer.submitAudioFrame(frame.get(), "fallback-video");
        }
    }

    void waitForOutputAudioLead(const EncodedOutputSink& outputMuxer,
                                std::int64_t pts,
                                std::int64_t outputBasePts,
                                const AVCodecContext& audioCodec)
    {
        const auto maxLeadSamples = std::max<std::int64_t>(audioCodec.sample_rate,
                                                           (audioCodec.frame_size > 0 ? audioCodec.frame_size : 1024) * 2);
        while (!stopping_.load() && pts >= (outputMuxer.nextAudioPts() - outputBasePts) + maxLeadSamples) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    void flushAudioFifo(AudioFifoPtr& fifo,
                        EncodedOutputSink& outputMuxer,
                        std::int64_t& audioPts,
                        std::int64_t outputAudioBasePts)
    {
        drainAudioFifo(fifo, outputMuxer, audioPts, outputAudioBasePts, true);
    }

    void flushAudioResampler(SwrContextPtr& resampler,
                             AVFrame* convertedFrame,
                             AudioFifoPtr& fifo,
                             EncodedOutputSink& outputMuxer,
                             std::int64_t& audioPts,
                             std::int64_t outputAudioBasePts)
    {
        auto* audioCodec = outputMuxer.audioCodec();
        if (audioCodec == nullptr || !resampler || !fifo) {
            return;
        }

        for (;;) {
            const auto delay = swr_get_delay(resampler.get(), audioCodec->sample_rate);
            if (delay <= 0) {
                break;
            }
            const auto maxOutputSamples = static_cast<int>(av_rescale_rnd(delay,
                                                                          audioCodec->sample_rate,
                                                                          audioCodec->sample_rate,
                                                                          AV_ROUND_UP));
            if (maxOutputSamples <= 0) {
                break;
            }

            av_frame_unref(convertedFrame);
            convertedFrame->format = audioCodec->sample_fmt;
            convertedFrame->sample_rate = audioCodec->sample_rate;
            convertedFrame->nb_samples = maxOutputSamples;
            checkAv(av_channel_layout_copy(&convertedFrame->ch_layout, &audioCodec->ch_layout), "copy flushed fallback audio layout");
            checkAv(av_frame_get_buffer(convertedFrame, 0), "allocate flushed fallback audio frame");

            const auto convertedSamples = swr_convert(resampler.get(),
                                                      convertedFrame->extended_data,
                                                      convertedFrame->nb_samples,
                                                      nullptr,
                                                      0);
            checkAv(convertedSamples, "flush fallback audio resampler");
            if (convertedSamples <= 0) {
                break;
            }

            checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + convertedSamples),
                    "grow flushed fallback audio FIFO");
            if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(convertedFrame->extended_data), convertedSamples) < convertedSamples) {
                throw std::runtime_error{"write flushed fallback audio FIFO failed"};
            }
            drainAudioFifo(fifo, outputMuxer, audioPts, outputAudioBasePts, false);
        }
    }

    RepeaterConfig config_;
    EncodedOutputSink& output_;
    std::filesystem::path path_;
    std::function<void(std::optional<std::string>)> statusCallback_;
    std::chrono::milliseconds duration_{0};
    std::mutex seekMutex_;
    std::optional<std::chrono::milliseconds> pendingSeek_;
    std::atomic_bool stopping_{false};
    std::atomic_bool finished_{false};
    std::thread thread_;
};

class AnalogueCaptureTranscoder {
public:
    AnalogueCaptureTranscoder(RepeaterConfig config, EncodedOutputSink& output, std::optional<ActiveInput> active)
        : config_{std::move(config)}
        , output_{output}
        , active_{std::move(active)}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~AnalogueCaptureTranscoder()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    AnalogueCaptureTranscoder(const AnalogueCaptureTranscoder&) = delete;
    AnalogueCaptureTranscoder& operator=(const AnalogueCaptureTranscoder&) = delete;

    [[nodiscard]] bool finished() const
    {
        return finished_.load();
    }

private:
    static int interruptCapture(void* opaque)
    {
        auto* self = static_cast<AnalogueCaptureTranscoder*>(opaque);
        return self != nullptr && self->stopping_.load() ? 1 : 0;
    }

    void run()
    {
        try {
            captureLoop();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: analogue capture stopped: " << ex.what() << '\n';
        }
        std::cerr << "wh-repeater: analogue capture worker exited\n";
        finished_.store(true);
    }

    void captureLoop()
    {
        const auto& capture = config_.analogue.capture;
        const auto* inputFormat = av_find_input_format("video4linux2");
        if (inputFormat == nullptr) {
            throw std::runtime_error{"FFmpeg video4linux2 input unavailable"};
        }

        AVDictionary* options = nullptr;
        const auto videoSize = std::to_string(capture.captureWidth) + "x" + std::to_string(capture.captureHeight);
        const auto captureFrameRate = frameRateText(capture);
        av_dict_set(&options, "standard", capture.captureStandard.c_str(), 0);
        av_dict_set(&options, "channel", "0", 0);
        av_dict_set(&options, "video_size", videoSize.c_str(), 0);
        av_dict_set(&options, "framerate", captureFrameRate.c_str(), 0);
        av_dict_set(&options, "input_format", capture.captureInputFormat.c_str(), 0);
        av_dict_set(&options, "timeout", "2000000", 0);

        AVFormatContext* rawInput = avformat_alloc_context();
        if (rawInput == nullptr) {
            av_dict_free(&options);
            throw std::runtime_error{"allocate analogue capture input context failed"};
        }
        rawInput->interrupt_callback.callback = interruptCapture;
        rawInput->interrupt_callback.opaque = this;
        const auto openStatus = avformat_open_input(&rawInput, capture.captureDevice.c_str(), inputFormat, &options);
        av_dict_free(&options);
        if (openStatus < 0) {
            avformat_close_input(&rawInput);
            checkAv(openStatus, "open analogue capture device " + capture.captureDevice);
        }
        InputFormatPtr input{rawInput};
        checkAv(avformat_find_stream_info(input.get(), nullptr), "probe analogue capture stream");

        const auto videoStreamIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        checkAv(videoStreamIndex, "find analogue video stream");
        auto* videoStream = input->streams[videoStreamIndex];
        const auto codecId = videoStream->codecpar->codec_id;
        const auto* decoder = findVideoDecoder(codecId);
        if (decoder == nullptr) {
            throw std::runtime_error{"no decoder available for analogue " + codecDescription(codecId)};
        }

        CodecContextPtr decoderContext{avcodec_alloc_context3(decoder)};
        if (!decoderContext) {
            throw std::runtime_error{"allocate analogue decoder failed"};
        }
        checkAv(avcodec_parameters_to_context(decoderContext.get(), videoStream->codecpar), "copy analogue video parameters");
        checkAv(avcodec_open2(decoderContext.get(), decoder, nullptr), "open analogue video decoder");

        const auto width = evenDimension(decoderContext->width > 0 ? decoderContext->width : videoStream->codecpar->width);
        const auto height = evenDimension(decoderContext->height > 0 ? decoderContext->height : videoStream->codecpar->height);
        const auto* sourcePixelFormat = av_get_pix_fmt_name(static_cast<AVPixelFormat>(decoderContext->pix_fmt));
        const auto frameRate = roundedFrameRate(capture);
        auto streamInfo = active_.has_value()
            ? std::optional<std::string>{streamInfoText(*active_, "SD analogue", width, height, frameRate)}
            : std::optional<std::string>{std::string{"SD analogue\n"} + videoSize + " " + captureFrameRate + " fps"};
        output_.setStreamInfo(std::move(streamInfo));
        output_.beginSubmittedSource("analogue", capture.audioEnabled);

        std::cout << "media pipeline capturing analogue from " << capture.captureDevice
                  << " as " << capture.captureStandard << " " << capture.captureInputFormat << " "
                  << videoSize << " " << captureFrameRate << " fps\n";
        std::cout << "media pipeline analogue source decoded as " << width << "x" << height
                  << " " << (sourcePixelFormat != nullptr ? sourcePixelFormat : "unknown") << '\n';
        std::thread audioThread;
        if (capture.audioEnabled) {
            audioThread = std::thread{[this] {
                try {
                    audioCaptureLoop();
                } catch (const std::exception& ex) {
                    std::cerr << "wh-repeater: analogue audio capture stopped: " << ex.what() << '\n';
                }
            }};
        }
        struct AudioThreadJoiner {
            std::thread& thread;
            ~AudioThreadJoiner()
            {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        } audioJoiner{audioThread};

        auto packet = PacketPtr{av_packet_alloc()};
        auto frame = FramePtr{av_frame_alloc()};
        auto convertedFrame = FramePtr{av_frame_alloc()};
        if (!packet || !frame || !convertedFrame) {
            throw std::runtime_error{"allocate analogue capture buffers failed"};
        }

        convertedFrame->format = output_.pixelFormat();
        convertedFrame->width = output_.width();
        convertedFrame->height = output_.height();
        checkAv(av_frame_get_buffer(convertedFrame.get(), 32), "allocate analogue converted frame");

        SwsContextPtr scaler;
        const auto captureStartedAt = std::chrono::steady_clock::now();
        std::int64_t lastFramePts = -1;
        while (!stopping_.load()) {
            const auto readStatus = av_read_frame(input.get(), packet.get());
            if (readStatus == AVERROR_EOF) {
                break;
            }
            checkAv(readStatus, "read analogue frame");
            if (packet->stream_index == videoStreamIndex) {
                decodePacket(decoderContext.get(), packet.get(), frame.get(), convertedFrame.get(), scaler, output_, captureStartedAt, lastFramePts);
            }
            av_packet_unref(packet.get());
        }

        checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush analogue decoder");
        drainDecoder(decoderContext.get(), frame.get(), convertedFrame.get(), scaler, output_, captureStartedAt, lastFramePts);
        output_.endSubmittedSource("analogue");
    }

    void audioCaptureLoop()
    {
        const auto& capture = config_.analogue.capture;
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr) {
            return;
        }

        const auto* inputFormat = av_find_input_format("alsa");
        if (inputFormat == nullptr) {
            throw std::runtime_error{"FFmpeg ALSA input unavailable"};
        }

        AVDictionary* options = nullptr;
        const auto sampleRate = std::to_string(capture.audioSampleRate);
        const auto channels = std::to_string(capture.audioChannels);
        av_dict_set(&options, "sample_rate", sampleRate.c_str(), 0);
        av_dict_set(&options, "channels", channels.c_str(), 0);

        AVFormatContext* rawInput = avformat_alloc_context();
        if (rawInput == nullptr) {
            av_dict_free(&options);
            throw std::runtime_error{"allocate analogue audio input context failed"};
        }
        rawInput->interrupt_callback.callback = interruptCapture;
        rawInput->interrupt_callback.opaque = this;
        const auto openStatus = avformat_open_input(&rawInput, capture.audioDevice.c_str(), inputFormat, &options);
        av_dict_free(&options);
        if (openStatus < 0) {
            avformat_close_input(&rawInput);
            checkAv(openStatus, "open analogue audio device " + capture.audioDevice);
        }
        InputFormatPtr input{rawInput};
        checkAv(avformat_find_stream_info(input.get(), nullptr), "probe analogue audio stream");

        const auto audioStreamIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        checkAv(audioStreamIndex, "find analogue audio stream");
        auto* audioStream = input->streams[audioStreamIndex];
        const auto* decoder = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (decoder == nullptr) {
            throw std::runtime_error{"no decoder available for analogue audio " + codecDescription(audioStream->codecpar->codec_id)};
        }

        CodecContextPtr decoderContext{avcodec_alloc_context3(decoder)};
        if (!decoderContext) {
            throw std::runtime_error{"allocate analogue audio decoder failed"};
        }
        checkAv(avcodec_parameters_to_context(decoderContext.get(), audioStream->codecpar), "copy analogue audio parameters");
        checkAv(avcodec_open2(decoderContext.get(), decoder, nullptr), "open analogue audio decoder");

        std::cout << "media pipeline capturing analogue audio from " << capture.audioDevice
                  << " at " << capture.audioSampleRate << " Hz " << capture.audioChannels << " channel(s)\n";

        auto packet = PacketPtr{av_packet_alloc()};
        auto frame = FramePtr{av_frame_alloc()};
        auto convertedFrame = FramePtr{av_frame_alloc()};
        if (!packet || !frame || !convertedFrame) {
            throw std::runtime_error{"allocate analogue audio buffers failed"};
        }
        SwrContextPtr resampler;
        AudioFifoPtr fifo;
        bool audioDelayPrimed{};
        while (!stopping_.load()) {
            const auto readStatus = av_read_frame(input.get(), packet.get());
            if (readStatus == AVERROR_EOF) {
                break;
            }
            checkAv(readStatus, "read analogue audio frame");
            if (packet->stream_index == audioStreamIndex) {
                decodeAudioPacket(decoderContext.get(), packet.get(), frame.get(), convertedFrame.get(), resampler, fifo, audioDelayPrimed);
            }
            av_packet_unref(packet.get());
        }

        if (!stopping_.load()) {
            checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush analogue audio decoder");
            drainAudioDecoder(decoderContext.get(), frame.get(), convertedFrame.get(), resampler, fifo, audioDelayPrimed);
        }
    }

    void decodePacket(AVCodecContext* decoder,
                      AVPacket* packet,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      EncodedOutputSink& outputMuxer,
                      std::chrono::steady_clock::time_point captureStartedAt,
                      std::int64_t& lastFramePts)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainDecoder(decoder, frame, convertedFrame, scaler, outputMuxer, captureStartedAt, lastFramePts);
            checkAv(avcodec_send_packet(decoder, packet), "send analogue packet after drain");
        } else {
            checkAv(sendStatus, "send analogue packet");
        }
        drainDecoder(decoder, frame, convertedFrame, scaler, outputMuxer, captureStartedAt, lastFramePts);
    }

    void decodeAudioPacket(AVCodecContext* decoder,
                           AVPacket* packet,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           bool& audioDelayPrimed)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, audioDelayPrimed);
            checkAv(avcodec_send_packet(decoder, packet), "send analogue audio packet after drain");
        } else {
            checkAv(sendStatus, "send analogue audio packet");
        }
        drainAudioDecoder(decoder, frame, convertedFrame, resampler, fifo, audioDelayPrimed);
    }

    void drainAudioDecoder(AVCodecContext* decoder,
                           AVFrame* frame,
                           AVFrame* convertedFrame,
                           SwrContextPtr& resampler,
                           AudioFifoPtr& fifo,
                           bool& audioDelayPrimed)
    {
        for (;;) {
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            checkAv(receiveStatus, "receive analogue audio frame");
            queueAudioFrame(frame, convertedFrame, resampler, fifo, audioDelayPrimed);
            av_frame_unref(frame);
        }
    }

    void queueAudioFrame(AVFrame* frame,
                         AVFrame* convertedFrame,
                         SwrContextPtr& resampler,
                         AudioFifoPtr& fifo,
                         bool& audioDelayPrimed)
    {
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr) {
            return;
        }
        if (!resampler) {
            AVChannelLayout inputLayout{};
            if (frame->ch_layout.nb_channels > 0) {
                checkAv(av_channel_layout_copy(&inputLayout, &frame->ch_layout), "copy analogue audio input layout");
            } else {
                av_channel_layout_default(&inputLayout, std::max(1, frame->ch_layout.nb_channels));
            }

            SwrContext* rawResampler{};
            checkAv(swr_alloc_set_opts2(&rawResampler,
                                        &audioCodec->ch_layout,
                                        audioCodec->sample_fmt,
                                        audioCodec->sample_rate,
                                        &inputLayout,
                                        static_cast<AVSampleFormat>(frame->format),
                                        frame->sample_rate,
                                        0,
                                        nullptr),
                    "allocate analogue audio resampler");
            av_channel_layout_uninit(&inputLayout);
            resampler.reset(rawResampler);
            checkAv(swr_init(resampler.get()), "initialise analogue audio resampler");

            fifo.reset(av_audio_fifo_alloc(audioCodec->sample_fmt, audioCodec->ch_layout.nb_channels, audioCodec->frame_size));
            if (!fifo) {
                throw std::runtime_error{"allocate analogue audio FIFO failed"};
            }
        }
        if (!audioDelayPrimed) {
            primeAnalogueAudioDelay(fifo, *audioCodec);
            audioDelayPrimed = true;
        }

        const auto delay = swr_get_delay(resampler.get(), frame->sample_rate);
        const auto maxOutputSamples = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples,
                                                                      audioCodec->sample_rate,
                                                                      frame->sample_rate,
                                                                      AV_ROUND_UP));
        av_frame_unref(convertedFrame);
        convertedFrame->format = audioCodec->sample_fmt;
        convertedFrame->sample_rate = audioCodec->sample_rate;
        convertedFrame->nb_samples = std::max(1, maxOutputSamples);
        checkAv(av_channel_layout_copy(&convertedFrame->ch_layout, &audioCodec->ch_layout), "copy analogue converted audio layout");
        checkAv(av_frame_get_buffer(convertedFrame, 0), "allocate analogue converted audio frame");

        const auto convertedSamples = swr_convert(resampler.get(),
                                                  convertedFrame->extended_data,
                                                  convertedFrame->nb_samples,
                                                  const_cast<const std::uint8_t**>(frame->extended_data),
                                                  frame->nb_samples);
        checkAv(convertedSamples, "resample analogue audio frame");
        convertedFrame->nb_samples = convertedSamples;

        checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + convertedSamples), "grow analogue audio FIFO");
        if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(convertedFrame->extended_data), convertedSamples) < convertedSamples) {
            throw std::runtime_error{"write analogue audio FIFO failed"};
        }
        drainAudioFifo(fifo);
    }

    void primeAnalogueAudioDelay(AudioFifoPtr& fifo, const AVCodecContext& audioCodec)
    {
        if (!fifo || config_.analogue.capture.audioDelayMs == 0) {
            return;
        }
        const auto samples = static_cast<int>(av_rescale(config_.analogue.capture.audioDelayMs,
                                                         audioCodec.sample_rate,
                                                         1000));
        if (samples <= 0) {
            return;
        }
        FramePtr silence{av_frame_alloc()};
        if (!silence) {
            throw std::runtime_error{"allocate analogue audio delay frame failed"};
        }
        silence->format = audioCodec.sample_fmt;
        silence->sample_rate = audioCodec.sample_rate;
        silence->nb_samples = samples;
        checkAv(av_channel_layout_copy(&silence->ch_layout, &audioCodec.ch_layout), "copy analogue audio delay layout");
        checkAv(av_frame_get_buffer(silence.get(), 0), "allocate analogue audio delay buffer");
        checkAv(av_frame_make_writable(silence.get()), "make analogue audio delay frame writable");
        checkAv(av_samples_set_silence(silence->extended_data,
                                       0,
                                       samples,
                                       audioCodec.ch_layout.nb_channels,
                                       audioCodec.sample_fmt),
                "fill analogue audio delay silence");
        checkAv(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + samples), "grow analogue audio delay FIFO");
        if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(silence->extended_data), samples) < samples) {
            throw std::runtime_error{"write analogue audio delay FIFO failed"};
        }
        std::cout << "media pipeline delaying analogue audio by " << config_.analogue.capture.audioDelayMs << " ms\n";
    }

    void drainAudioFifo(AudioFifoPtr& fifo)
    {
        auto* audioCodec = output_.audioCodec();
        if (audioCodec == nullptr || !fifo) {
            return;
        }
        const auto frameSamples = audioCodec->frame_size > 0 ? audioCodec->frame_size : 1024;
        while (av_audio_fifo_size(fifo.get()) >= frameSamples) {
            if (stopping_.load()) {
                return;
            }
            FramePtr frame{av_frame_alloc()};
            if (!frame) {
                throw std::runtime_error{"allocate analogue output audio frame failed"};
            }
            frame->format = audioCodec->sample_fmt;
            frame->sample_rate = audioCodec->sample_rate;
            frame->nb_samples = frameSamples;
            checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec->ch_layout), "copy analogue output audio layout");
            checkAv(av_frame_get_buffer(frame.get(), 0), "allocate analogue output audio frame buffer");
            if (av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->extended_data), frameSamples) < frameSamples) {
                throw std::runtime_error{"read analogue audio FIFO failed"};
            }
            output_.submitAudioFrame(frame.get(), "analogue");
        }
    }

    void drainDecoder(AVCodecContext* decoder,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      EncodedOutputSink& outputMuxer,
                      std::chrono::steady_clock::time_point captureStartedAt,
                      std::int64_t& lastFramePts)
    {
        for (;;) {
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            checkAv(receiveStatus, "receive analogue frame");
            const auto elapsed = std::chrono::steady_clock::now() - captureStartedAt;
            auto pts = static_cast<std::int64_t>(std::llround(
                std::chrono::duration<double>{elapsed}.count() * static_cast<double>(outputMuxer.frameRate())));
            if (pts <= lastFramePts) {
                pts = lastFramePts + 1;
            }
            lastFramePts = pts;
            outputMuxer.submitVideoFrame(convertFrame(frame, convertedFrame, scaler, outputMuxer, pts), "analogue");
            av_frame_unref(frame);
        }
    }

    AVFrame* convertFrame(AVFrame* frame,
                          AVFrame* convertedFrame,
                          SwsContextPtr& scaler,
                          const EncodedOutputSink& outputMuxer,
                          std::int64_t pts)
    {
        if (frame->format == outputMuxer.pixelFormat()
            && frame->width == outputMuxer.width()
            && frame->height == outputMuxer.height()) {
            frame->pts = pts;
            return frame;
        }

        checkAv(av_frame_make_writable(convertedFrame), "make analogue converted frame writable");
        fillBlack(*convertedFrame);

        const auto scale = std::min(static_cast<double>(outputMuxer.width()) / static_cast<double>(frame->width),
                                    static_cast<double>(outputMuxer.height()) / static_cast<double>(frame->height));
        auto dstWidth = std::max(2, static_cast<int>(std::floor(frame->width * scale))) & ~1;
        auto dstHeight = std::max(2, static_cast<int>(std::floor(frame->height * scale))) & ~1;
        dstWidth = std::min(dstWidth, outputMuxer.width());
        dstHeight = std::min(dstHeight, outputMuxer.height());
        const auto dstX = ((outputMuxer.width() - dstWidth) / 2) & ~1;
        const auto dstY = ((outputMuxer.height() - dstHeight) / 2) & ~1;

        if (!scaler) {
            scaler.reset(sws_getContext(frame->width,
                                        frame->height,
                                        normalisedPixelFormat(static_cast<AVPixelFormat>(frame->format)),
                                        dstWidth,
                                        dstHeight,
                                        outputMuxer.pixelFormat(),
                                        SWS_FAST_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr));
            if (!scaler) {
                throw std::runtime_error{"create analogue scaler failed"};
            }
        }
        std::array<std::uint8_t*, 4> dstData{
            convertedFrame->data[0] + dstY * convertedFrame->linesize[0] + dstX,
            convertedFrame->data[1] + (dstY / 2) * convertedFrame->linesize[1] + (dstX / 2),
            convertedFrame->data[2] + (dstY / 2) * convertedFrame->linesize[2] + (dstX / 2),
            nullptr,
        };
        std::array<int, 4> dstLinesize{
            convertedFrame->linesize[0],
            convertedFrame->linesize[1],
            convertedFrame->linesize[2],
            0,
        };
        sws_scale(scaler.get(),
                  frame->data,
                  frame->linesize,
                  0,
                  frame->height,
                  dstData.data(),
                  dstLinesize.data());
        convertedFrame->pts = pts;
        return convertedFrame;
    }

    RepeaterConfig config_;
    EncodedOutputSink& output_;
    std::optional<ActiveInput> active_;
    std::atomic_bool stopping_{false};
    std::atomic_bool finished_{false};
    std::thread thread_;
};

#if defined(WH_REPEATER_HAVE_GSTREAMER)
class GStreamerOutputMuxer {
public:
    GStreamerOutputMuxer(const RepeaterConfig& config, PlutoSink& output)
        : config_{config}
        , output_{output}
        , width_{outputWidth(config_)}
        , height_{outputHeight(config_)}
        , frameRate_{outputFrameRate(config_)}
    {
        configureGStreamerRuntime();
        gst_init(nullptr, nullptr);
        const auto pipelineText = gstRawOutputPipeline(config_);
        GError* rawError{};
        GstElement* rawPipeline = gst_parse_launch(pipelineText.c_str(), &rawError);
        if (rawError != nullptr) {
            std::string message{rawError->message != nullptr ? rawError->message : "unknown GStreamer parse error"};
            g_error_free(rawError);
            if (rawPipeline != nullptr) {
                gst_element_set_state(rawPipeline, GST_STATE_NULL);
                gst_object_unref(rawPipeline);
            }
            throw std::runtime_error{"create GStreamer output pipeline failed: " + message};
        }
        if (rawPipeline == nullptr) {
            throw std::runtime_error{"create GStreamer output pipeline returned null"};
        }
        pipeline_.reset(rawPipeline);

        rawVideoSrc_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "video_src");
        rawAudioSrc_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "audio_src");
        rawTsSink_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "ts_sink");
        if (rawVideoSrc_ == nullptr || rawAudioSrc_ == nullptr || rawTsSink_ == nullptr) {
            throw std::runtime_error{"GStreamer output pipeline is missing app elements"};
        }
        videoSrcObject_.reset(GST_OBJECT(rawVideoSrc_));
        audioSrcObject_.reset(GST_OBJECT(rawAudioSrc_));
        tsSinkObject_.reset(GST_OBJECT(rawTsSink_));

        auto* videoSrc = GST_APP_SRC(rawVideoSrc_);
        auto* audioSrc = GST_APP_SRC(rawAudioSrc_);
        gst_app_src_set_stream_type(videoSrc, GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_stream_type(audioSrc, GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_latency(videoSrc, 0, GST_CLOCK_TIME_NONE);
        gst_app_src_set_latency(audioSrc, 0, GST_CLOCK_TIME_NONE);
        gst_app_sink_set_emit_signals(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_drop(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_max_buffers(GST_APP_SINK(rawTsSink_), 16);

        GstBus* rawBus = gst_element_get_bus(pipeline_.get());
        if (rawBus == nullptr) {
            throw std::runtime_error{"GStreamer output pipeline has no bus"};
        }
        busObject_.reset(GST_OBJECT(rawBus));

        const auto status = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
        if (status == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error{"start GStreamer output pipeline failed"};
        }

        std::cout << "media pipeline using GStreamer output at "
                  << width_ << "x" << height_ << " " << frameRate_ << " fps\n";
        if (config_.streaming.rtmp.enabled && !config_.streaming.rtmp.url.empty()) {
            std::cout << "media pipeline streaming RTMP through GStreamer to "
                      << config_.streaming.rtmp.url << '\n';
        }
    }

    ~GStreamerOutputMuxer()
    {
        try {
            finish();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: GStreamer output finish failed: " << ex.what() << '\n';
        }
    }

    GStreamerOutputMuxer(const GStreamerOutputMuxer&) = delete;
    GStreamerOutputMuxer& operator=(const GStreamerOutputMuxer&) = delete;

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int frameRate() const { return frameRate_; }
    [[nodiscard]] AVPixelFormat pixelFormat() const { return AV_PIX_FMT_YUV420P; }

    void writeVideoFrame(const AVFrame* frame)
    {
        if (finished_) {
            return;
        }
        if (frame == nullptr
            || frame->format != AV_PIX_FMT_YUV420P
            || frame->width != width_
            || frame->height != height_
            || frame->data[0] == nullptr
            || frame->data[1] == nullptr
            || frame->data[2] == nullptr) {
            throw std::runtime_error{"invalid frame passed to GStreamer output"};
        }

        const auto ySize = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        const auto chromaWidth = width_ / 2;
        const auto chromaHeight = height_ / 2;
        const auto uvSize = static_cast<std::size_t>(chromaWidth) * static_cast<std::size_t>(chromaHeight);
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, ySize + uvSize * 2, nullptr);
        if (buffer == nullptr) {
            throw std::runtime_error{"allocate GStreamer video buffer failed"};
        }

        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            throw std::runtime_error{"map GStreamer video buffer failed"};
        }
        auto* dst = map.data;
        copyPlane(dst, width_, frame->data[0], frame->linesize[0], width_, height_);
        dst += ySize;
        copyPlane(dst, chromaWidth, frame->data[1], frame->linesize[1], chromaWidth, chromaHeight);
        dst += uvSize;
        copyPlane(dst, chromaWidth, frame->data[2], frame->linesize[2], chromaWidth, chromaHeight);
        gst_buffer_unmap(buffer, &map);

        const auto pts = videoFrameIndex_;
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(static_cast<guint64>(pts), GST_SECOND, static_cast<guint64>(frameRate_));
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, static_cast<guint64>(frameRate_));
        ++videoFrameIndex_;

        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(rawVideoSrc_), buffer);
        if (flow != GST_FLOW_OK) {
            throw std::runtime_error{"push GStreamer video buffer failed: " + std::to_string(static_cast<int>(flow))};
        }
        drain(false);
    }

    void writeAudioSamples(const float* samples, int count, std::int64_t pts)
    {
        (void)pts;
        if (finished_ || samples == nullptr || count <= 0) {
            return;
        }
        const auto byteSize = static_cast<std::size_t>(count) * sizeof(float);
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, byteSize, nullptr);
        if (buffer == nullptr) {
            throw std::runtime_error{"allocate GStreamer audio buffer failed"};
        }
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            throw std::runtime_error{"map GStreamer audio buffer failed"};
        }
        std::memcpy(map.data, samples, byteSize);
        gst_buffer_unmap(buffer, &map);

        const auto outputPts = audioSampleIndex_;
        audioSampleIndex_ += count;
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(static_cast<guint64>(outputPts), GST_SECOND, defaultOutputAudioSampleRate);
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(static_cast<guint64>(count), GST_SECOND, defaultOutputAudioSampleRate);

        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(rawAudioSrc_), buffer);
        if (flow != GST_FLOW_OK) {
            throw std::runtime_error{"push GStreamer audio buffer failed: " + std::to_string(static_cast<int>(flow))};
        }
        drain(false);
    }

    void finish()
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        if (rawVideoSrc_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(rawVideoSrc_));
        }
        if (rawAudioSrc_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(rawAudioSrc_));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline && drain(true)) {
        }
        if (pipeline_) {
            gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        }
    }

private:
    static void copyPlane(std::uint8_t* dst,
                          int dstStride,
                          const std::uint8_t* src,
                          int srcStride,
                          int width,
                          int height)
    {
        for (int row = 0; row < height; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(row) * dstStride,
                        src + static_cast<std::size_t>(row) * srcStride,
                        static_cast<std::size_t>(width));
        }
    }

    bool drain(bool waitForEos)
    {
        bool sawWork = false;
        for (;;) {
            checkBus(waitForEos ? 50 * GST_MSECOND : 0);
            GstSamplePtr sample{waitForEos
                    ? gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 50 * GST_MSECOND)
                    : gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 0)};
            if (!sample) {
                break;
            }
            sawWork = true;
            GstBuffer* buffer = gst_sample_get_buffer(sample.get());
            if (buffer == nullptr) {
                continue;
            }
            GstMapInfo map{};
            if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                continue;
            }
            output_.writeMuxData(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(map.data),
                map.size,
            });
            gst_buffer_unmap(buffer, &map);
        }
        checkBus(0);
        return sawWork;
    }

    void checkBus(GstClockTime timeout)
    {
        auto* bus = GST_BUS(busObject_.get());
        while (bus != nullptr) {
            GstMessagePtr message{timeout > 0
                    ? gst_bus_timed_pop_filtered(bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
                    : gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))};
            if (!message) {
                break;
            }
            timeout = 0;
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_EOS) {
                eosSeen_ = true;
                continue;
            }
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_ERROR) {
                GError* error{};
                gchar* debug{};
                gst_message_parse_error(message.get(), &error, &debug);
                std::string errorText{error != nullptr && error->message != nullptr ? error->message : "unknown GStreamer error"};
                if (debug != nullptr && debug[0] != '\0') {
                    errorText += " (";
                    errorText += debug;
                    errorText += ")";
                }
                if (error != nullptr) {
                    g_error_free(error);
                }
                g_free(debug);
                throw std::runtime_error{errorText};
            }
        }
    }

    const RepeaterConfig& config_;
    PlutoSink& output_;
    int width_{};
    int height_{};
    int frameRate_{};
    GstElementPtr pipeline_;
    GstObjectPtr videoSrcObject_;
    GstObjectPtr audioSrcObject_;
    GstObjectPtr tsSinkObject_;
    GstObjectPtr busObject_;
    GstElement* rawVideoSrc_{};
    GstElement* rawAudioSrc_{};
    GstElement* rawTsSink_{};
    std::int64_t videoFrameIndex_{0};
    std::int64_t audioSampleIndex_{0};
    bool eosSeen_{false};
    bool finished_{false};
};

class GStreamerLiveTranscoder {
public:
    GStreamerLiveTranscoder(RepeaterConfig config, PlutoSink& output)
        : config_{std::move(config)}
        , output_{output}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~GStreamerLiveTranscoder()
    {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    GStreamerLiveTranscoder(const GStreamerLiveTranscoder&) = delete;
    GStreamerLiveTranscoder& operator=(const GStreamerLiveTranscoder&) = delete;

    void append(std::span<const std::byte> packet)
    {
        input_.append(packet);
    }

    void stop()
    {
        stopping_.store(true);
        input_.stop();
    }

    [[nodiscard]] bool finished() const
    {
        return finished_.load();
    }

    [[nodiscard]] bool ready() const
    {
        return ready_.load();
    }

    [[nodiscard]] std::string error() const
    {
        return error_;
    }

private:
    void run()
    {
        try {
            transcodeLoop();
        } catch (const std::exception& ex) {
            error_ = ex.what();
            std::cerr << "wh-repeater: GStreamer live transcode stopped: " << ex.what() << '\n';
        }
        finished_.store(true);
    }

    void transcodeLoop()
    {
        configureGStreamerRuntime();
        gst_init(nullptr, nullptr);
        const auto pipelineText = gstLiveTsPipeline(config_);
        GError* rawError{};
        GstElement* rawPipeline = gst_parse_launch(pipelineText.c_str(), &rawError);
        if (rawError != nullptr) {
            std::string message{rawError->message != nullptr ? rawError->message : "unknown GStreamer parse error"};
            g_error_free(rawError);
            if (rawPipeline != nullptr) {
                gst_element_set_state(rawPipeline, GST_STATE_NULL);
                gst_object_unref(rawPipeline);
            }
            throw std::runtime_error{"create GStreamer live pipeline failed: " + message};
        }
        if (rawPipeline == nullptr) {
            throw std::runtime_error{"create GStreamer live pipeline returned null"};
        }
        pipeline_.reset(rawPipeline);

        rawInput_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "ts_src");
        rawTsSink_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "ts_sink");
        if (rawInput_ == nullptr || rawTsSink_ == nullptr) {
            throw std::runtime_error{"GStreamer live pipeline is missing app elements"};
        }
        inputObject_.reset(GST_OBJECT(rawInput_));
        sinkObject_.reset(GST_OBJECT(rawTsSink_));
        gst_app_src_set_stream_type(GST_APP_SRC(rawInput_), GST_APP_STREAM_TYPE_STREAM);
        gst_app_sink_set_emit_signals(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_drop(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_max_buffers(GST_APP_SINK(rawTsSink_), 16);

        GstBus* rawBus = gst_element_get_bus(pipeline_.get());
        if (rawBus == nullptr) {
            throw std::runtime_error{"GStreamer live pipeline has no bus"};
        }
        busObject_.reset(GST_OBJECT(rawBus));

        const auto status = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
        if (status == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error{"start GStreamer live pipeline failed"};
        }
        std::cout << "media pipeline retranscoding received TS through GStreamer at "
                  << outputWidth(config_) << "x" << outputHeight(config_) << " "
                  << outputFrameRate(config_) << " fps\n";

        std::array<std::byte, 1316 * 8> buffer{};
        while (!stopping_.load()) {
            const auto bytes = input_.read(reinterpret_cast<std::uint8_t*>(buffer.data()), static_cast<int>(buffer.size()));
            if (bytes == AVERROR_EOF) {
                break;
            }
            if (bytes < 0) {
                continue;
            }
            pushInput(std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(bytes)});
            drain(false);
        }
    }

    void pushInput(std::span<const std::byte> data)
    {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
        if (buffer == nullptr) {
            throw std::runtime_error{"allocate GStreamer TS input buffer failed"};
        }
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            throw std::runtime_error{"map GStreamer TS input buffer failed"};
        }
        std::memcpy(map.data, data.data(), data.size());
        gst_buffer_unmap(buffer, &map);
        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(rawInput_), buffer);
        if (flow != GST_FLOW_OK) {
            throw std::runtime_error{"push GStreamer TS input failed: " + std::to_string(static_cast<int>(flow))};
        }
    }

    void drain(bool wait)
    {
        for (;;) {
            checkBus(wait ? 50 * GST_MSECOND : 0);
            GstSamplePtr sample{wait
                    ? gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 50 * GST_MSECOND)
                    : gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 0)};
            if (!sample) {
                break;
            }
            GstBuffer* buffer = gst_sample_get_buffer(sample.get());
            if (buffer == nullptr) {
                continue;
            }
            GstMapInfo map{};
            if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                continue;
            }
            ready_.store(true);
            output_.writeMuxData(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(map.data),
                map.size,
            });
            gst_buffer_unmap(buffer, &map);
        }
        checkBus(0);
    }

    void checkBus(GstClockTime timeout)
    {
        auto* bus = GST_BUS(busObject_.get());
        while (bus != nullptr) {
            GstMessagePtr message{timeout > 0
                    ? gst_bus_timed_pop_filtered(bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
                    : gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))};
            if (!message) {
                break;
            }
            timeout = 0;
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_EOS) {
                return;
            }
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_ERROR) {
                GError* error{};
                gchar* debug{};
                gst_message_parse_error(message.get(), &error, &debug);
                std::string errorText{error != nullptr && error->message != nullptr ? error->message : "unknown GStreamer error"};
                if (debug != nullptr && debug[0] != '\0') {
                    errorText += " (";
                    errorText += debug;
                    errorText += ")";
                }
                if (error != nullptr) {
                    g_error_free(error);
                }
                g_free(debug);
                throw std::runtime_error{errorText};
            }
        }
    }

    RepeaterConfig config_;
    PlutoSink& output_;
    BlockingTsInput input_;
    GstElementPtr pipeline_;
    GstObjectPtr inputObject_;
    GstObjectPtr sinkObject_;
    GstObjectPtr busObject_;
    GstElement* rawInput_{};
    GstElement* rawTsSink_{};
    std::atomic_bool stopping_{false};
    std::atomic_bool ready_{false};
    std::atomic_bool finished_{false};
    std::string error_;
    std::thread thread_;
};

class GStreamerAnalogueCaptureTranscoder {
public:
    GStreamerAnalogueCaptureTranscoder(RepeaterConfig config, PlutoSink& output)
        : config_{std::move(config)}
        , output_{output}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~GStreamerAnalogueCaptureTranscoder()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    GStreamerAnalogueCaptureTranscoder(const GStreamerAnalogueCaptureTranscoder&) = delete;
    GStreamerAnalogueCaptureTranscoder& operator=(const GStreamerAnalogueCaptureTranscoder&) = delete;

    [[nodiscard]] bool finished() const
    {
        return finished_.load();
    }

private:
    void run()
    {
        try {
            captureLoop();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: GStreamer analogue capture stopped: " << ex.what() << '\n';
        }
        finished_.store(true);
    }

    void captureLoop()
    {
        configureGStreamerRuntime();
        gst_init(nullptr, nullptr);
        const auto pipelineText = gstAnalogueCapturePipeline(config_);
        GError* rawError{};
        GstElement* rawPipeline = gst_parse_launch(pipelineText.c_str(), &rawError);
        if (rawError != nullptr) {
            std::string message{rawError->message != nullptr ? rawError->message : "unknown GStreamer parse error"};
            g_error_free(rawError);
            if (rawPipeline != nullptr) {
                gst_element_set_state(rawPipeline, GST_STATE_NULL);
                gst_object_unref(rawPipeline);
            }
            throw std::runtime_error{"create GStreamer analogue pipeline failed: " + message};
        }
        if (rawPipeline == nullptr) {
            throw std::runtime_error{"create GStreamer analogue pipeline returned null"};
        }
        pipeline_.reset(rawPipeline);

        rawTsSink_ = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "ts_sink");
        if (rawTsSink_ == nullptr) {
            throw std::runtime_error{"GStreamer analogue pipeline is missing TS appsink"};
        }
        sinkObject_.reset(GST_OBJECT(rawTsSink_));
        gst_app_sink_set_emit_signals(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_drop(GST_APP_SINK(rawTsSink_), false);
        gst_app_sink_set_max_buffers(GST_APP_SINK(rawTsSink_), 16);

        GstBus* rawBus = gst_element_get_bus(pipeline_.get());
        if (rawBus == nullptr) {
            throw std::runtime_error{"GStreamer analogue pipeline has no bus"};
        }
        busObject_.reset(GST_OBJECT(rawBus));

        const auto status = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
        if (status == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error{"start GStreamer analogue pipeline failed"};
        }
        std::cout << "media pipeline capturing analogue through GStreamer from "
                  << config_.analogue.capture.captureDevice << '\n';

        while (!stopping_.load()) {
            drain(true);
        }
    }

    void drain(bool wait)
    {
        checkBus(wait ? 100 * GST_MSECOND : 0);
        GstSamplePtr sample{wait
                ? gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 100 * GST_MSECOND)
                : gst_app_sink_try_pull_sample(GST_APP_SINK(rawTsSink_), 0)};
        if (!sample) {
            return;
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample.get());
        if (buffer == nullptr) {
            return;
        }
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            return;
        }
        output_.writeMuxData(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(map.data),
            map.size,
        });
        gst_buffer_unmap(buffer, &map);
    }

    void checkBus(GstClockTime timeout)
    {
        auto* bus = GST_BUS(busObject_.get());
        while (bus != nullptr) {
            GstMessagePtr message{timeout > 0
                    ? gst_bus_timed_pop_filtered(bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
                    : gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))};
            if (!message) {
                break;
            }
            timeout = 0;
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_EOS) {
                stopping_.store(true);
                return;
            }
            if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_ERROR) {
                GError* error{};
                gchar* debug{};
                gst_message_parse_error(message.get(), &error, &debug);
                std::string errorText{error != nullptr && error->message != nullptr ? error->message : "unknown GStreamer error"};
                if (debug != nullptr && debug[0] != '\0') {
                    errorText += " (";
                    errorText += debug;
                    errorText += ")";
                }
                if (error != nullptr) {
                    g_error_free(error);
                }
                g_free(debug);
                throw std::runtime_error{errorText};
            }
        }
    }

    RepeaterConfig config_;
    PlutoSink& output_;
    GstElementPtr pipeline_;
    GstObjectPtr sinkObject_;
    GstObjectPtr busObject_;
    GstElement* rawTsSink_{};
    std::atomic_bool stopping_{false};
    std::atomic_bool finished_{false};
    std::thread thread_;
};
#endif

class FallbackMuxer : public EncodedOutputSink {
public:
    FallbackMuxer(const RepeaterConfig& config, PlutoSink& output, PersistentRtmpMuxer* rtmp)
        : config_{config}
        , output_{output}
        , rtmp_{rtmp}
        , frameRate_{outputFrameRate(config_)}
        , slideDurationFrames_{std::max<std::int64_t>(1, config_.fallback.slideDuration.count() * frameRate_ / 1000)}
        , morseUnits_{morseToneUnits(identText(config_))}
        , noticeMorseUnits_{morseToneUnits("K")}
        , morseUnitSamples_{std::max<std::int64_t>(1, static_cast<std::int64_t>(defaultOutputAudioSampleRate * 1.2 / std::max(1U, config_.ident.morseWpm)))}
        , nextIdentFrame_{config_.ident.interval.count() > 0
                ? 0
                : std::numeric_limits<std::int64_t>::max()}
    {
        initialiseSlides();
        initialise();
    }

    ~FallbackMuxer()
    {
        try {
            flushAudio();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: fallback audio flush failed: " << ex.what() << '\n';
        }
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            gstMuxer_->finish();
            return;
        }
#endif
        if (headerWritten_) {
            av_write_trailer(format_);
        }
        if (codec_) {
            avcodec_free_context(&codec_);
        }
        if (audioCodec_) {
            avcodec_free_context(&audioCodec_);
        }
        if (format_) {
            if (format_->pb != nullptr) {
                avio_context_free(&format_->pb);
            }
            avformat_free_context(format_);
        }
        av_free(avioBuffer_);
    }

    FallbackMuxer(const FallbackMuxer&) = delete;
    FallbackMuxer& operator=(const FallbackMuxer&) = delete;

    [[nodiscard]] AVPixelFormat pixelFormat() const override
    {
        return outputPixelFormat();
    }

    [[nodiscard]] int width() const override
    {
        return outputWidthPixels();
    }

    [[nodiscard]] int height() const override
    {
        return outputHeightPixels();
    }

    [[nodiscard]] int frameRateValue() const override
    {
        return frameRate_;
    }

    [[nodiscard]] AVCodecContext* audioCodec() const override
    {
        return audioCodec_;
    }

    [[nodiscard]] std::int64_t nextVideoPts() const override
    {
        std::lock_guard lock{outputMutex_};
        return nextOutputVideoPts(lastVideoPts_);
    }

    [[nodiscard]] std::int64_t nextAudioPts() const override
    {
        std::lock_guard lock{outputMutex_};
        return audioSampleIndex_;
    }

    void setNotice(std::optional<std::string> notice, bool endTone = false)
    {
        std::lock_guard lock{outputMutex_};
        if (notice_ == notice) {
            if (notice_.has_value()
                && endTone
                && !noticeMorsePlayedForNotice_
                && !noticeMorsePending_
                && !noticeMorseActive_
                && !noticeMorseUnits_.empty()) {
                noticeMorsePending_ = true;
            }
            return;
        }
        notice_ = std::move(notice);
        noticeMorsePlayedForNotice_ = false;
        noticeMorsePending_ = notice_.has_value() && endTone && !noticeMorseUnits_.empty();
        pendingSlateRender_.reset();
        if (!notice_.has_value()) {
            noticeMorseActive_ = false;
            noticeMorseStartSample_ = std::numeric_limits<std::int64_t>::max();
            noticeMorseEndSample_ = 0;
        }
        cachedSlate_.reset();
    }

    void setStreamInfo(std::optional<std::string> streamInfo) override
    {
        std::lock_guard lock{outputMutex_};
        setStreamInfoLocked(std::move(streamInfo), false);
    }

    void beginSubmittedSource(std::string_view source, bool hasAudio) override
    {
        std::lock_guard lock{outputMutex_};
        const auto sourceText = std::string{source};
        clearSubmittedVideoLocked();
        clearSubmittedAudioLocked();
        submittedVideoSource_ = sourceText;
        submittedAudioSource_ = sourceText;
        submittedSourceHasAudio_ = hasAudio;
        submittedVideoBaseFrame_ = nextOutputVideoPts(lastVideoPts_);
        submittedAudioStartSample_ = audioSampleIndex_;
        submittedVideoAt_ = {};
    }

    void endSubmittedSource(std::string_view source) override
    {
        std::lock_guard lock{outputMutex_};
        const auto sourceText = std::string{source};
        if (submittedVideoSource_ == sourceText) {
            clearSubmittedVideoLocked();
            submittedVideoSource_ = "submitted";
        }
        if (submittedAudioSource_ == sourceText) {
            clearSubmittedAudioLocked();
            submittedAudioSource_ = "submitted";
        }
        if (sourceText == "fallback-video") {
            submittedSourceHasAudio_ = false;
        }
    }

    void setSubmittedSourceHasAudio(bool hasAudio) override
    {
        std::lock_guard lock{outputMutex_};
        submittedSourceHasAudio_ = hasAudio;
    }

    void setLiveStreamInfo(std::optional<std::string> streamInfo)
    {
        std::lock_guard lock{outputMutex_};
        setStreamInfoLocked(std::move(streamInfo), true);
    }

    void setStreamInfoLocked(std::optional<std::string> streamInfo, bool submittedVideoOnly)
    {
        if (streamInfo_ == streamInfo && streamInfoSubmittedVideoOnly_ == submittedVideoOnly) {
            return;
        }
        streamInfo_ = std::move(streamInfo);
        streamInfoSubmittedVideoOnly_ = submittedVideoOnly;
        streamInfoOverlay_.reset();
        liveInfoFramesRemaining_ = streamInfo_.has_value() && !streamInfo_->empty() ? std::max(1, frameRate_) * 4 : 0;
        cachedComposited_.reset();
    }

    void writeFrame(std::int64_t frameIndex)
    {
        std::lock_guard lock{outputMutex_};
        const auto started = std::chrono::steady_clock::now();
        writeFrameLocked(frameIndex, false);
        recordWriteFrameDurationLocked(std::chrono::steady_clock::now() - started);
        maybeLogTimingStatsLocked("fallback");
    }

    void writeFrame(std::int64_t frameIndex, bool preferSubmittedVideo)
    {
        std::lock_guard lock{outputMutex_};
        const auto started = std::chrono::steady_clock::now();
        writeFrameLocked(frameIndex, preferSubmittedVideo);
        recordWriteFrameDurationLocked(std::chrono::steady_clock::now() - started);
        maybeLogTimingStatsLocked(preferSubmittedVideo ? "submitted" : "fallback");
    }

    void submitVideoFrame(AVFrame* frame, std::string_view source) override
    {
        std::unique_lock lock{outputMutex_};
        if (frame == nullptr
            || frame->format != outputPixelFormat()
            || frame->width != outputWidthPixels()
            || frame->height != outputHeightPixels()
            || frame->data[0] == nullptr
            || frame->linesize[0] <= 0) {
            std::cerr << "wh-repeater: " << source << " frame rejected before muxer queue\n";
            return;
        }
        FramePtr clone{av_frame_clone(frame)};
        if (!clone) {
            throw std::runtime_error{"clone submitted video frame failed"};
        }
        normaliseVideoFrameProperties(*clone);
        const auto sourceText = std::string{source};
        if (submittedVideoSource_ != sourceText) {
            clearSubmittedVideoLocked();
            clearSubmittedAudioLocked();
            submittedVideoSource_ = sourceText;
            submittedVideoBaseFrame_ = nextOutputVideoPts(lastVideoPts_);
        } else if (sourceText == "fallback-video" && submittedVideoBaseFrame_ == AV_NOPTS_VALUE) {
            submittedVideoBaseFrame_ = nextOutputVideoPts(lastVideoPts_);
        }
        if (sourceText == "fallback-video") {
            const auto maxQueuedVideoFrames = std::size_t{2};
            while (submittedVideoFrames_.size() >= maxQueuedVideoFrames) {
                submittedVideoConsumed_.wait_for(lock, std::chrono::milliseconds{100});
                if (submittedVideoFrames_.size() >= maxQueuedVideoFrames) {
                    return;
                }
            }
        } else {
            submittedVideoFrames_.clear();
        }
        submittedVideoFrames_.push_back(std::move(clone));
        submittedVideoAt_ = std::chrono::steady_clock::now();
    }

    void submitAudioFrame(AVFrame* frame, std::string_view source) override
    {
        std::unique_lock lock{outputMutex_};
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            return;
        }
#endif
        if (audioCodec_ == nullptr
            || frame == nullptr
            || frame->format != audioCodec_->sample_fmt
            || frame->sample_rate != audioCodec_->sample_rate
            || frame->nb_samples <= 0
            || av_channel_layout_compare(&frame->ch_layout, &audioCodec_->ch_layout) != 0
            || frame->extended_data == nullptr
            || frame->extended_data[0] == nullptr) {
            std::cerr << "wh-repeater: " << source << " audio frame rejected before muxer queue\n";
            return;
        }
        if (!submittedAudioFifo_) {
            submittedAudioFifo_.reset(av_audio_fifo_alloc(audioCodec_->sample_fmt,
                                                          audioCodec_->ch_layout.nb_channels,
                                                          audioCodec_->sample_rate));
            if (!submittedAudioFifo_) {
                throw std::runtime_error{"allocate submitted audio FIFO failed"};
            }
        }
        const auto sourceText = std::string{source};
        if (submittedAudioSource_ != sourceText) {
            clearSubmittedAudioLocked();
            submittedAudioSource_ = sourceText;
            submittedAudioStartSample_ = audioSampleIndex_;
        }
        int writeSamples = frame->nb_samples;
        const auto maxSubmittedAudioSamples = sourceText == "fallback-video"
            ? std::max(audioCodec_->frame_size * 6, audioCodec_->sample_rate)
            : audioCodec_->sample_rate * 2;
        if (sourceText == "fallback-video"
            && av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples > maxSubmittedAudioSamples) {
            const auto dropSamples = av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples - maxSubmittedAudioSamples;
            av_audio_fifo_drain(submittedAudioFifo_.get(), std::max(0, dropSamples));
        }
        const auto backpressureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (sourceText != "fallback-video"
               && av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples > maxSubmittedAudioSamples) {
            submittedAudioConsumed_.wait_for(lock, std::chrono::milliseconds{100});
            if (av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples > maxSubmittedAudioSamples) {
                std::cerr << "wh-repeater: submitted audio queue still full; applying decode backpressure\n";
            }
            if (std::chrono::steady_clock::now() >= backpressureDeadline
                && av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples > maxSubmittedAudioSamples) {
                const auto dropSamples = std::min(writeSamples, av_audio_fifo_size(submittedAudioFifo_.get()));
                std::cerr << "wh-repeater: submitted audio queue full for too long; dropping "
                          << dropSamples << " samples to avoid worker stall\n";
                av_audio_fifo_drain(submittedAudioFifo_.get(), dropSamples);
                break;
            }
        }
        checkAv(av_audio_fifo_realloc(submittedAudioFifo_.get(),
                                      av_audio_fifo_size(submittedAudioFifo_.get()) + writeSamples),
                "grow submitted audio FIFO");
        if (av_audio_fifo_write(submittedAudioFifo_.get(),
                                reinterpret_cast<void**>(frame->extended_data),
                                writeSamples) < writeSamples) {
            throw std::runtime_error{"write submitted audio FIFO failed"};
        }
        ++statsSubmittedAudioInputFrames_;
        statsSubmittedAudioInputSamples_ += static_cast<std::uint64_t>(writeSamples);
    }

private:
    void startSlateRenderLocked(const std::string& text)
    {
        auto state = std::make_shared<AsyncRenderedFrame>(text);
        pendingSlateRender_ = state;
        const auto config = config_;
        const auto frameRate = frameRate_;
        std::thread{[state, config, text, frameRate] {
            auto frame = renderHtmlSlateFrame(config, text, frameRate);
            std::lock_guard lock{state->mutex};
            state->frame = std::move(frame);
            state->ready = true;
        }}.detach();
    }

    void pollSlateRenderLocked(const std::string& text)
    {
        if (!pendingSlateRender_ || pendingSlateRender_->key != text) {
            return;
        }
        auto state = pendingSlateRender_;
        {
            std::lock_guard lock{state->mutex};
            if (!state->ready || !state->frame) {
                return;
            }
            cachedSlate_ = std::move(state->frame);
        }
        pendingSlateRender_.reset();
        cachedComposited_.reset();
    }

    void startTestcardRenderLocked()
    {
        auto state = std::make_shared<AsyncRenderedFrame>("testcard");
        pendingTestcardRender_ = state;
        const auto config = config_;
        const auto frameRate = frameRate_;
        std::thread{[state, config, frameRate] {
            auto frame = renderHtmlTestcardFrame(config, frameRate);
            std::lock_guard lock{state->mutex};
            state->frame = std::move(frame);
            state->ready = true;
        }}.detach();
    }

    void pollTestcardRenderLocked()
    {
        if (!pendingTestcardRender_) {
            return;
        }
        auto state = pendingTestcardRender_;
        {
            std::lock_guard lock{state->mutex};
            if (!state->ready || !state->frame) {
                return;
            }
            cachedTestcard_ = std::move(state->frame);
        }
        pendingTestcardRender_.reset();
        cachedComposited_.reset();
    }

    void writeFrameLocked(std::int64_t frameIndex, bool preferSubmittedVideo)
    {
        FramePtr frame;
        const AVFrame* contentIdentity{};
        const auto submittedVideoFresh = submittedVideoSource_ == "fallback-video"
            ? (!submittedVideoFrames_.empty() && fallbackVideoAudioReadyLocked())
            : (!submittedVideoFrames_.empty()
               && std::chrono::steady_clock::now() - submittedVideoAt_ < std::chrono::seconds{5});
        if (preferSubmittedVideo && submittedVideoFresh) {
            if (submittedVideoSource_ == "fallback-video") {
                while (submittedVideoFrames_.size() > 1
                       && submittedFrameDuePts(*submittedVideoFrames_[1]) <= frameIndex) {
                    submittedVideoFrames_.pop_front();
                    submittedVideoConsumed_.notify_one();
                }
                if (submittedFrameDuePts(*submittedVideoFrames_.front()) > frameIndex) {
                    writeHeldOrGeneratedSubmittedFrame(frameIndex);
                    return;
                }
                frame = std::move(submittedVideoFrames_.front());
                submittedVideoFrames_.pop_front();
                submittedVideoConsumed_.notify_one();
            } else {
                frame.reset(av_frame_clone(submittedVideoFrames_.back().get()));
            }
            if (!frame) {
                throw std::runtime_error{"submitted video frame missing"};
            }
            if (submittedVideoSource_ == "fallback-video") {
                heldSubmittedVideoFrame_.reset(av_frame_clone(frame.get()));
                if (!heldSubmittedVideoFrame_) {
                    throw std::runtime_error{"clone held fallback video frame failed"};
                }
                normaliseVideoFrameProperties(*heldSubmittedVideoFrame_);
                heldRenderedSubmittedVideoFrame_.reset();
            }
            ++statsSubmittedVideoFrames_;
            writeSubmittedVideoFrame(std::move(frame), frameIndex);
            return;
        }
        if (preferSubmittedVideo
            && submittedVideoSource_ == "fallback-video"
            && (heldSubmittedVideoFrame_ || heldRenderedSubmittedVideoFrame_)) {
            ++statsHeldVideoFrames_;
            writeHeldOrGeneratedSubmittedFrame(frameIndex);
            return;
        }

        if (notice_.has_value()) {
            pollSlateRenderLocked(*notice_);
            if (!pendingSlateRender_) {
                startSlateRenderLocked(*notice_);
            }
            if (!cachedSlate_) {
                cachedSlate_ = renderSlateFrame(config_, *notice_, frameRate_);
                cachedComposited_.reset();
            }
            contentIdentity = cachedSlate_.get();
            frame.reset(av_frame_clone(cachedSlate_.get()));
            if (!frame) {
                throw std::runtime_error{"clone rendered slate frame failed"};
            }
        } else {
            const auto identActive = identActiveForFrame(frameIndex);
            if (identActive) {
                pollTestcardRenderLocked();
                if (!pendingTestcardRender_) {
                    startTestcardRenderLocked();
                }
                if (!cachedTestcard_) {
                    cachedTestcard_ = renderTestcardFrame(config_, frameRate_);
                    cachedComposited_.reset();
                }
                contentIdentity = cachedTestcard_.get();
                frame.reset(av_frame_clone(cachedTestcard_.get()));
            } else {
                frame = slideFrame(frameIndex);
                contentIdentity = cachedSlide_ ? cachedSlide_.get() : cachedIdent_.get();
            }
            if (!frame) {
                throw std::runtime_error{"clone rendered fallback frame failed"};
            }
        }
        frame->pts = frameIndex;
        auto compositedFrame = compositedFallbackFrame(frame.get(), contentIdentity, frameIndex);
        ++statsGeneratedVideoFrames_;
        encode(compositedFrame.get());
        writeAudioUntil(frameIndex + 1, false);
    }

    void clearSubmittedAudioLocked()
    {
        if (submittedAudioFifo_) {
            av_audio_fifo_reset(submittedAudioFifo_.get());
            submittedAudioConsumed_.notify_all();
        }
    }

    bool fallbackVideoAudioReadyLocked() const
    {
        if (!submittedSourceHasAudio_ || audioCodec_ == nullptr) {
            return true;
        }
        const auto requiredSamples = std::max(audioFrameSamples_, 1) * 2;
        return submittedAudioFifo_ && av_audio_fifo_size(submittedAudioFifo_.get()) >= requiredSamples;
    }

    void clearSubmittedVideoLocked()
    {
        submittedVideoFrames_.clear();
        heldSubmittedVideoFrame_.reset();
        heldRenderedSubmittedVideoFrame_.reset();
        submittedVideoBaseFrame_ = AV_NOPTS_VALUE;
        submittedVideoConsumed_.notify_all();
    }

    std::int64_t submittedFrameDuePts(const AVFrame& frame) const
    {
        const auto base = submittedVideoBaseFrame_ == AV_NOPTS_VALUE
            ? nextOutputVideoPts(lastVideoPts_)
            : submittedVideoBaseFrame_;
        const auto relativePts = frame.pts == AV_NOPTS_VALUE ? 0 : frame.pts;
        return base + relativePts;
    }

    static long durationMs(std::chrono::steady_clock::duration duration)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    void recordWriteFrameDurationLocked(std::chrono::steady_clock::duration duration)
    {
        ++statsOutputFrames_;
        maxWriteFrameDuration_ = std::max(maxWriteFrameDuration_, duration);
    }

    void recordVideoEncodeDurationLocked(std::chrono::steady_clock::duration duration)
    {
        maxVideoEncodeDuration_ = std::max(maxVideoEncodeDuration_, duration);
    }

    void recordAudioEncodeDurationLocked(std::chrono::steady_clock::duration duration)
    {
        ++statsAudioFrames_;
        maxAudioEncodeDuration_ = std::max(maxAudioEncodeDuration_, duration);
    }

    void maybeLogTimingStatsLocked(std::string_view mode)
    {
        const auto now = std::chrono::steady_clock::now();
        if (nextStatsLogAt_ == std::chrono::steady_clock::time_point{}) {
            nextStatsLogAt_ = now + std::chrono::seconds{5};
            statsStartedAt_ = now;
            return;
        }
        if (now < nextStatsLogAt_) {
            return;
        }

        const auto elapsedMs = std::max<long>(1, durationMs(now - statsStartedAt_));
        const auto submittedAudioSamples = submittedAudioFifo_ ? av_audio_fifo_size(submittedAudioFifo_.get()) : 0;
        std::cout << "media pipeline output stats mode=" << mode
                  << " elapsed_ms=" << elapsedMs
                  << " frames=" << statsOutputFrames_
                  << " generated=" << statsGeneratedVideoFrames_
                  << " submitted=" << statsSubmittedVideoFrames_
                  << " held=" << statsHeldVideoFrames_
                  << " audio_frames=" << statsAudioFrames_
                  << " submitted_audio_in_frames=" << statsSubmittedAudioInputFrames_
                  << " submitted_audio_in_samples=" << statsSubmittedAudioInputSamples_
                  << " submitted_audio_out_frames=" << statsSubmittedAudioOutputFrames_
                  << " submitted_video_q=" << submittedVideoFrames_.size()
                  << " submitted_audio_samples=" << submittedAudioSamples
                  << " max_write_ms=" << durationMs(maxWriteFrameDuration_)
                  << " max_video_ms=" << durationMs(maxVideoEncodeDuration_)
                  << " max_audio_ms=" << durationMs(maxAudioEncodeDuration_)
                  << '\n';

        statsStartedAt_ = now;
        nextStatsLogAt_ = now + std::chrono::seconds{5};
        statsOutputFrames_ = 0;
        statsGeneratedVideoFrames_ = 0;
        statsSubmittedVideoFrames_ = 0;
        statsHeldVideoFrames_ = 0;
        statsAudioFrames_ = 0;
        statsSubmittedAudioInputFrames_ = 0;
        statsSubmittedAudioInputSamples_ = 0;
        statsSubmittedAudioOutputFrames_ = 0;
        maxWriteFrameDuration_ = {};
        maxVideoEncodeDuration_ = {};
        maxAudioEncodeDuration_ = {};
    }

    void writeHeldOrGeneratedSubmittedFrame(std::int64_t frameIndex)
    {
        if (heldRenderedSubmittedVideoFrame_) {
            FramePtr frame{av_frame_clone(heldRenderedSubmittedVideoFrame_.get())};
            if (!frame) {
                throw std::runtime_error{"clone rendered held submitted video frame failed"};
            }
            frame->pts = frameIndex;
            encode(frame.get(), submittedVideoSource_);
            writeAudioUntil(frameIndex + 1, true);
            if (liveInfoFramesRemaining_ > 0) {
                --liveInfoFramesRemaining_;
            }
            return;
        }

        FramePtr frame;
        if (heldSubmittedVideoFrame_) {
            frame.reset(av_frame_clone(heldSubmittedVideoFrame_.get()));
        } else {
            frame = generatedVideoFrame(frameIndex);
        }
        if (!frame) {
            throw std::runtime_error{"create held submitted video frame failed"};
        }
        writeSubmittedVideoFrame(std::move(frame), frameIndex);
    }

    void writeSubmittedVideoFrame(FramePtr frame, std::int64_t frameIndex)
    {
        frame->pts = frameIndex;
        auto overlayFrame = identOverlay_->render(frame.get());
        overlayFrame->pts = frameIndex;
        if (streamInfoOverlay_ == nullptr && liveInfoFramesRemaining_ > 0
            && streamInfo_.has_value() && !streamInfo_->empty()) {
            streamInfoOverlay_ = std::make_unique<OverlayRenderer>(streamInfoFilter(*streamInfo_, frameRate_),
                                                                    outputWidthPixels(),
                                                                    outputHeightPixels(),
                                                                    outputPixelFormat(),
                                                                    frameRate_);
        }
        if (streamInfoOverlay_ != nullptr && liveInfoFramesRemaining_ > 0) {
            overlayFrame = streamInfoOverlay_->render(overlayFrame.get());
            overlayFrame->pts = frameIndex;
            --liveInfoFramesRemaining_;
        }
        if (submittedVideoSource_ == "fallback-video") {
            heldRenderedSubmittedVideoFrame_.reset(av_frame_clone(overlayFrame.get()));
            if (!heldRenderedSubmittedVideoFrame_) {
                throw std::runtime_error{"cache rendered fallback video frame failed"};
            }
        }
        encode(overlayFrame.get(), submittedVideoSource_);
        writeAudioUntil(frameIndex + 1, true);
    }

    FramePtr compositedFallbackFrame(AVFrame* frame, const AVFrame* contentIdentity, std::int64_t frameIndex)
    {
        const auto secondIndex = frameIndex / std::max(1, frameRate_);
        if (cachedComposited_
            && cachedCompositedIdentity_ == contentIdentity
            && cachedCompositedSecondIndex_ == secondIndex) {
            auto clone = FramePtr{av_frame_clone(cachedComposited_.get())};
            if (!clone) {
                throw std::runtime_error{"clone composited fallback frame failed"};
            }
            clone->pts = frameIndex;
            return clone;
        }

        auto overlayFrame = identOverlay_->render(frame);
        overlayFrame->pts = frameIndex;
        auto clockFrame = clockOverlay(frameIndex).render(overlayFrame.get());
        clockFrame->pts = frameIndex;
        auto* finalFrame = clockFrame.get();
        FramePtr streamInfoFrame;
        if (!streamInfoSubmittedVideoOnly_
            && !notice_.has_value()
            && streamInfo_.has_value()
            && !streamInfo_->empty()) {
            if (!streamInfoOverlay_) {
                streamInfoOverlay_ = std::make_unique<OverlayRenderer>(streamInfoFilter(*streamInfo_, frameRate_),
                                                                        outputWidthPixels(),
                                                                        outputHeightPixels(),
                                                                        outputPixelFormat(),
                                                                        frameRate_);
            }
            streamInfoFrame = streamInfoOverlay_->render(finalFrame);
            streamInfoFrame->pts = frameIndex;
            finalFrame = streamInfoFrame.get();
        }
        cachedComposited_ = FramePtr{av_frame_clone(finalFrame)};
        if (!cachedComposited_) {
            throw std::runtime_error{"cache composited fallback frame failed"};
        }
        cachedCompositedIdentity_ = contentIdentity;
        cachedCompositedSecondIndex_ = secondIndex;
        return streamInfoFrame ? std::move(streamInfoFrame) : std::move(clockFrame);
    }

    OverlayRenderer& clockOverlay(std::int64_t frameIndex)
    {
        const auto secondIndex = frameIndex / std::max(1, frameRate_);
        if (!clockOverlay_ || clockSecondIndex_ != secondIndex) {
            clockSecondIndex_ = secondIndex;
            clockOverlay_ = std::make_unique<OverlayRenderer>(fallbackClockFilter(fallbackClockText()),
                                                              outputWidthPixels(),
                                                              outputHeightPixels(),
                                                              outputPixelFormat(),
                                                              frameRate_);
        }
        return *clockOverlay_;
    }

    void initialiseSlides()
    {
        activeSlidePaths_ = isDecember() ? slideFilesIn(config_.fallback.christmasSlideDirectory) : std::vector<std::filesystem::path>{};
        if (activeSlidePaths_.empty()) {
            activeSlidePaths_ = slideFilesIn(config_.fallback.slideDirectory);
        }
        if (!activeSlidePaths_.empty()) {
            std::mt19937 generator{std::random_device{}()};
            std::shuffle(activeSlidePaths_.begin(), activeSlidePaths_.end(), generator);
            std::cout << "media pipeline loaded " << activeSlidePaths_.size() << " slideshow images";
            if (isDecember() && std::filesystem::path{config_.fallback.christmasSlideDirectory}.is_absolute()) {
                std::cout << " from Christmas folder";
            }
            std::cout << '\n';
        }
    }

    FramePtr slideFrame(std::int64_t frameIndex)
    {
        if (activeSlidePaths_.empty()) {
            if (!cachedIdent_) {
                cachedIdent_ = renderIdentFrame(config_, frameRate_);
            }
            FramePtr frame{av_frame_clone(cachedIdent_.get())};
            return frame;
        }

        if (!cachedSlide_ || frameIndex >= nextSlideFrame_) {
            const auto& path = activeSlidePaths_[slideIndex_ % activeSlidePaths_.size()];
            try {
                cachedSlide_ = decodeSlideFrame(config_, path);
                cachedComposited_.reset();
            } catch (const std::exception& ex) {
                std::cerr << "wh-repeater: load slideshow image failed: " << ex.what() << '\n';
                cachedSlide_ = renderIdentFrame(config_, frameRate_);
                cachedComposited_.reset();
            }
            ++slideIndex_;
            nextSlideFrame_ = frameIndex + slideDurationFrames_;
        }

        return FramePtr{av_frame_clone(cachedSlide_.get())};
    }

    void initialise()
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (config_.media.backend == "gstreamer") {
            gstMuxer_ = std::make_unique<GStreamerOutputMuxer>(config_, output_);
            identOverlay_ = std::make_unique<OverlayRenderer>(identFilter(identText(config_)),
                                                              outputWidthPixels(),
                                                              outputHeightPixels(),
                                                              outputPixelFormat(),
                                                              frameRate_);
            audioFrameSamples_ = 1024;
            return;
        }
#endif
        checkAv(avformat_alloc_output_context2(&format_, nullptr, "mpegts", nullptr), "allocate MPEG-TS muxer");
        avioBuffer_ = static_cast<std::uint8_t*>(av_malloc(32768));
        if (avioBuffer_ == nullptr) {
            throw std::runtime_error{"allocate MPEG-TS IO buffer failed"};
        }
        format_->pb = avio_alloc_context(avioBuffer_, 32768, 1, &output_, nullptr, plutoWritePacket, nullptr);
        if (format_->pb == nullptr) {
            throw std::runtime_error{"allocate MPEG-TS AVIO failed"};
        }
        avioBuffer_ = nullptr;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;

        stream_ = avformat_new_stream(format_, nullptr);
        if (stream_ == nullptr) {
            throw std::runtime_error{"create fallback video stream failed"};
        }

        const auto videoBitrateKbps = fallbackVideoBitrateKbps(config_);
        auto encoder = openFallbackEncoder(config_, *format_, frameRate_, videoBitrateKbps);
        codec_ = encoder.context;
        identOverlay_ = std::make_unique<OverlayRenderer>(identFilter(identText(config_)),
                                                          codec_->width,
                                                          codec_->height,
                                                          outputPixelFormat(),
                                                          frameRate_);
        std::cout << "media pipeline using H.264 encoder " << encoder.name
                  << " at " << codec_->width << "x" << codec_->height
                  << " " << frameRate_ << " fps\n";
        checkAv(avcodec_parameters_from_context(stream_->codecpar, codec_), "copy fallback encoder parameters");
        stream_->time_base = codec_->time_base;

        initialiseAudio();

        AVDictionary* options = nullptr;
        av_dict_set(&options, "muxdelay", "0", 0);
        av_dict_set(&options, "muxpreload", "0", 0);
        av_dict_set(&options, "pcr_period", "40", 0);
        av_dict_set(&options, "mpegts_flags", "initial_discontinuity+system_b", 0);
        format_->max_delay = 0;
        const auto headerStatus = avformat_write_header(format_, &options);
        av_dict_free(&options);
        checkAv(headerStatus, "write fallback MPEG-TS header");
        headerWritten_ = true;

        if (rtmp_ != nullptr) {
            if (audioCodec_ != nullptr) {
                rtmp_->configureAudio(*audioCodec_);
            }
            rtmp_->configureVideo(*codec_);
        }
    }

    void initialiseAudio()
    {
        const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (encoder == nullptr) {
            std::cerr << "wh-repeater: AAC encoder unavailable; fallback ident audio disabled\n";
            return;
        }

        audioStream_ = avformat_new_stream(format_, nullptr);
        if (audioStream_ == nullptr) {
            throw std::runtime_error{"create fallback audio stream failed"};
        }

        audioCodec_ = avcodec_alloc_context3(encoder);
        if (audioCodec_ == nullptr) {
            throw std::runtime_error{"allocate fallback audio encoder failed"};
        }
        audioCodec_->codec_type = AVMEDIA_TYPE_AUDIO;
        audioCodec_->sample_rate = defaultOutputAudioSampleRate;
        audioCodec_->bit_rate = static_cast<std::int64_t>(std::max(32U, config_.pluto.audioBitrateKbps)) * 1000;
        audioCodec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&audioCodec_->ch_layout, outputAudioChannels(config_));
        audioCodec_->time_base = AVRational{1, defaultOutputAudioSampleRate};
        if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audioCodec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        checkAv(avcodec_open2(audioCodec_, encoder, nullptr), "open fallback AAC encoder");
        checkAv(avcodec_parameters_from_context(audioStream_->codecpar, audioCodec_), "copy fallback audio parameters");
        audioStream_->time_base = audioCodec_->time_base;
        audioFrameSamples_ = audioCodec_->frame_size > 0 ? audioCodec_->frame_size : 1024;
    }

    void encode(AVFrame* frame, std::string_view source = "fallback")
    {
        FramePtr generatedFrame;
        auto* safeFrame = fixedVideoFrame(frame, source, generatedFrame);
        sendVideoFrame(safeFrame);
    }

    void sendVideoFrame(AVFrame* safeFrame)
    {
        const auto started = std::chrono::steady_clock::now();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            gstMuxer_->writeVideoFrame(safeFrame);
            recordVideoEncodeDurationLocked(std::chrono::steady_clock::now() - started);
            return;
        }
#endif
        FramePtr hardwareFrame;
        AVFrame* encoderFrame = safeFrame;
        if (codec_->pix_fmt == AV_PIX_FMT_VAAPI) {
            hardwareFrame = uploadVaapiFrame(*codec_, safeFrame, encoderUploadScaler_);
            encoderFrame = hardwareFrame.get();
        }
        checkAv(avcodec_send_frame(codec_, encoderFrame), "send output frame");
        PacketPtr packet{av_packet_alloc()};
        if (!packet) {
            throw std::runtime_error{"allocate fallback packet failed"};
        }
        for (;;) {
            const auto status = avcodec_receive_packet(codec_, packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            checkAv(status, "receive fallback packet");
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->dts = packet->pts;
            }
            if (rtmp_ != nullptr) {
                rtmp_->writeVideoPacket(packet.get());
            }
            writeTransportPacket(packet.get());
            av_packet_unref(packet.get());
        }
        recordVideoEncodeDurationLocked(std::chrono::steady_clock::now() - started);
    }

    FramePtr generatedVideoFrame(std::int64_t pts)
    {
        FramePtr frame{av_frame_alloc()};
        if (!frame) {
            throw std::runtime_error{"allocate generated fallback frame failed"};
        }
        frame->format = outputPixelFormat();
        frame->width = outputWidthPixels();
        frame->height = outputHeightPixels();
        frame->pts = pts;
        checkAv(av_frame_get_buffer(frame.get(), 32), "allocate generated fallback frame buffer");
        checkAv(av_frame_make_writable(frame.get()), "make generated fallback frame writable");
        fillBlack(*frame);
        return frame;
    }

    AVFrame* fixedVideoFrame(AVFrame* frame, std::string_view source, FramePtr& generatedFrame)
    {
        const auto pts = nextOutputVideoPts(lastVideoPts_);

        const bool valid = frame != nullptr
            && frame->format == outputPixelFormat()
            && frame->width == outputWidthPixels()
            && frame->height == outputHeightPixels()
            && frame->data[0] != nullptr
            && frame->linesize[0] > 0;

        lastVideoPts_ = pts;
        if (!valid) {
            std::cerr << "wh-repeater: " << source << " frame rejected before encoder; sending generated frame\n";
            generatedFrame = generatedVideoFrame(pts);
            return generatedFrame.get();
        }
        lastVideoPts_ = pts;
        frame->pts = pts;
        normaliseVideoFrameProperties(*frame);
        return frame;
    }

    FramePtr generatedAudioFrame(std::int64_t pts, int samples)
    {
        FramePtr frame{av_frame_alloc()};
        if (!frame) {
            throw std::runtime_error{"allocate generated output audio frame failed"};
        }
        frame->format = audioCodec_->sample_fmt;
        frame->sample_rate = audioCodec_->sample_rate;
        frame->nb_samples = std::max(1, samples);
        frame->pts = pts;
        checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec_->ch_layout), "copy generated output audio layout");
        checkAv(av_frame_get_buffer(frame.get(), 0), "allocate generated output audio buffer");
        checkAv(av_frame_make_writable(frame.get()), "make generated output audio writable");
        for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
            if (frame->extended_data[channel] != nullptr) {
                std::memset(frame->extended_data[channel], 0, static_cast<std::size_t>(frame->linesize[0]));
            }
        }
        return frame;
    }

    AVFrame* fixedAudioFrame(AVFrame* frame, std::string_view source, FramePtr& generatedFrame)
    {
        const auto expectedSamples = audioCodec_->frame_size > 0 ? audioCodec_->frame_size : 1024;
        const auto pts = audioSampleIndex_;

        const bool valid = frame != nullptr
            && frame->format == audioCodec_->sample_fmt
            && frame->sample_rate == audioCodec_->sample_rate
            && frame->nb_samples > 0
            && av_channel_layout_compare(&frame->ch_layout, &audioCodec_->ch_layout) == 0
            && frame->extended_data != nullptr
            && frame->extended_data[0] != nullptr;

        if (!valid) {
            std::cerr << "wh-repeater: " << source << " audio frame rejected before encoder; sending silence\n";
            generatedFrame = generatedAudioFrame(pts, expectedSamples);
            audioSampleIndex_ = pts + generatedFrame->nb_samples;
            return generatedFrame.get();
        }
        frame->pts = pts;
        audioSampleIndex_ = pts + frame->nb_samples;
        return frame;
    }

    bool identActiveForFrame(std::int64_t frameIndex)
    {
        if (!config_.ident.enabled || config_.ident.interval.count() <= 0 || morseUnits_.empty()) {
            return false;
        }

        if (frameIndex >= nextIdentFrame_) {
            identStartFrame_ = frameIndex;
            identStartSample_ = audioSampleIndex_;
            const auto morseSamples = static_cast<std::int64_t>(morseUnits_.size()) * morseUnitSamples_;
            const auto morseFrames = firstIdent_
                ? static_cast<std::int64_t>(frameRate_) * 10
                : std::max<std::int64_t>(frameRate_ * 5,
                (morseSamples * frameRate_ + defaultOutputAudioSampleRate - 1) / defaultOutputAudioSampleRate);
            identEndFrame_ = identStartFrame_ + morseFrames;
            nextIdentFrame_ = frameIndex + static_cast<std::int64_t>(config_.ident.interval.count()) * frameRate_;
            firstIdent_ = false;
        }

        return frameIndex >= identStartFrame_ && frameIndex < identEndFrame_;
    }

    bool morseToneAtSample(std::int64_t sample) const
    {
        if (sample < identStartSample_ || morseUnits_.empty()) {
            return false;
        }
        const auto unit = static_cast<std::size_t>((sample - identStartSample_) / morseUnitSamples_);
        return unit < morseUnits_.size() && morseUnits_[unit];
    }

    void armNoticeMorseIfPending()
    {
        if (!noticeMorsePending_ || noticeMorseUnits_.empty()) {
            return;
        }
        noticeMorsePending_ = false;
        noticeMorseActive_ = true;
        noticeMorsePlayedForNotice_ = true;
        noticeMorseStartSample_ = audioSampleIndex_;
        noticeMorseEndSample_ = noticeMorseStartSample_
            + static_cast<std::int64_t>(noticeMorseUnits_.size()) * morseUnitSamples_;
    }

    bool noticeMorseToneAtSample(std::int64_t sample) const
    {
        if (!noticeMorseActive_
            || sample < noticeMorseStartSample_
            || sample >= noticeMorseEndSample_
            || noticeMorseUnits_.empty()) {
            return false;
        }
        const auto unit = static_cast<std::size_t>((sample - noticeMorseStartSample_) / morseUnitSamples_);
        return unit < noticeMorseUnits_.size() && noticeMorseUnits_[unit];
    }

    bool generatedToneAtSample(std::int64_t sample) const
    {
        return morseToneAtSample(sample) || noticeMorseToneAtSample(sample);
    }

    double generatedTonePhase(std::int64_t sample) const
    {
        const auto startSample = noticeMorseToneAtSample(sample) ? noticeMorseStartSample_ : identStartSample_;
        return 2.0 * pi * static_cast<double>(config_.ident.morseToneHz) * static_cast<double>(sample - startSample)
            / static_cast<double>(defaultOutputAudioSampleRate);
    }

    void writeAudioUntil(std::int64_t nextVideoFrame, bool preferSubmittedAudio)
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            while (audioFrameDue(audioSampleIndex_, audioFrameSamples_, nextVideoFrame, defaultOutputAudioSampleRate, frameRate_)) {
                writeAudioFrame(audioFrameSamples_, preferSubmittedAudio);
            }
            return;
        }
#endif
        if (audioCodec_ == nullptr || audioStream_ == nullptr || !headerWritten_) {
            return;
        }

        while (audioFrameDue(audioSampleIndex_, audioFrameSamples_, nextVideoFrame, defaultOutputAudioSampleRate, frameRate_)) {
            writeAudioFrame(audioFrameSamples_, preferSubmittedAudio);
        }
    }

    void writeAudioFrame(int samples, bool preferSubmittedAudio)
    {
        const auto started = std::chrono::steady_clock::now();
        armNoticeMorseIfPending();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            std::vector<float> audio(static_cast<std::size_t>(samples));
            for (int index = 0; index < samples; ++index) {
                const auto sample = audioSampleIndex_ + index;
                if (generatedToneAtSample(sample)) {
                    audio[static_cast<std::size_t>(index)] = static_cast<float>(0.22 * std::sin(generatedTonePhase(sample)));
                } else {
                    audio[static_cast<std::size_t>(index)] = 0.0F;
                }
            }
            const auto pts = audioSampleIndex_;
            audioSampleIndex_ += samples;
            gstMuxer_->writeAudioSamples(audio.data(), samples, pts);
            recordAudioEncodeDurationLocked(std::chrono::steady_clock::now() - started);
            return;
        }
#endif
        FramePtr frame{av_frame_alloc()};
        if (!frame) {
            throw std::runtime_error{"allocate fallback audio frame failed"};
        }
        frame->format = audioCodec_->sample_fmt;
        frame->sample_rate = audioCodec_->sample_rate;
        frame->nb_samples = samples;
        checkAv(av_channel_layout_copy(&frame->ch_layout, &audioCodec_->ch_layout), "copy fallback audio channel layout");
        frame->pts = audioSampleIndex_;
        checkAv(av_frame_get_buffer(frame.get(), 0), "allocate fallback audio frame buffer");
        checkAv(av_frame_make_writable(frame.get()), "make fallback audio frame writable");

        bool usedSubmittedAudio = false;
        if (preferSubmittedAudio
            && submittedAudioFifo_
            && av_audio_fifo_size(submittedAudioFifo_.get()) >= samples) {
            if (av_audio_fifo_read(submittedAudioFifo_.get(),
                                   reinterpret_cast<void**>(frame->extended_data),
                                   samples) < samples) {
                throw std::runtime_error{"read submitted audio FIFO failed"};
            }
            submittedAudioConsumed_.notify_one();
            usedSubmittedAudio = true;
            ++statsSubmittedAudioOutputFrames_;
        } else {
            for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
                auto* plane = reinterpret_cast<float*>(frame->extended_data[channel]);
                if (plane == nullptr) {
                    continue;
                }
                for (int index = 0; index < samples; ++index) {
                    const auto sample = audioSampleIndex_ + index;
                    if (generatedToneAtSample(sample)) {
                        plane[index] = static_cast<float>(0.22 * std::sin(generatedTonePhase(sample)));
                    } else {
                        plane[index] = 0.0F;
                    }
                }
            }
        }
        audioSampleIndex_ += samples;

        checkAv(avcodec_send_frame(audioCodec_, frame.get()), usedSubmittedAudio ? "send submitted audio frame" : "send fallback audio frame");
        drainAudioPackets();
        recordAudioEncodeDurationLocked(std::chrono::steady_clock::now() - started);
    }

    void flushAudio()
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            gstMuxer_->finish();
            return;
        }
#endif
        if (audioCodec_ == nullptr) {
            return;
        }
        const auto status = avcodec_send_frame(audioCodec_, nullptr);
        if (status >= 0) {
            drainAudioPackets();
        }
    }

    void drainAudioPackets()
    {
        PacketPtr packet{av_packet_alloc()};
        if (!packet) {
            throw std::runtime_error{"allocate fallback audio packet failed"};
        }
        for (;;) {
            const auto status = avcodec_receive_packet(audioCodec_, packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            checkAv(status, "receive fallback audio packet");
            if (rtmp_ != nullptr) {
                rtmp_->writeAudioPacket(packet.get());
            }
            writeTransportAudioPacket(packet.get());
            av_packet_unref(packet.get());
        }
    }

    void writeTransportPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, codec_->time_base, stream_->time_base);
        packet->duration = av_rescale_q(1, codec_->time_base, stream_->time_base);
        packet->stream_index = stream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write fallback MPEG-TS packet");
    }

    void writeTransportAudioPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, audioCodec_->time_base, audioStream_->time_base);
        packet->stream_index = audioStream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write fallback MPEG-TS audio packet");
    }

    int outputWidthPixels() const
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            return gstMuxer_->width();
        }
#endif
        return codec_ != nullptr ? codec_->width : outputWidth(config_);
    }

    int outputHeightPixels() const
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            return gstMuxer_->height();
        }
#endif
        return codec_ != nullptr ? codec_->height : outputHeight(config_);
    }

    AVPixelFormat outputPixelFormat() const
    {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstMuxer_) {
            return gstMuxer_->pixelFormat();
        }
#endif
        return codec_ != nullptr ? softwarePixelFormatForEncoder(*codec_) : AV_PIX_FMT_YUV420P;
    }

    const RepeaterConfig& config_;
    PlutoSink& output_;
    PersistentRtmpMuxer* rtmp_{};
    int frameRate_{};
    AVFormatContext* format_{nullptr};
    AVCodecContext* codec_{nullptr};
    AVCodecContext* audioCodec_{nullptr};
    AVStream* stream_{nullptr};
    AVStream* audioStream_{nullptr};
#if defined(WH_REPEATER_HAVE_GSTREAMER)
    std::unique_ptr<GStreamerOutputMuxer> gstMuxer_;
#endif
    std::unique_ptr<OverlayRenderer> identOverlay_;
    std::unique_ptr<OverlayRenderer> clockOverlay_;
    std::unique_ptr<OverlayRenderer> streamInfoOverlay_;
    SwsContextPtr encoderUploadScaler_;
    std::int64_t clockSecondIndex_{-1};
    std::optional<std::string> streamInfo_;
    bool streamInfoSubmittedVideoOnly_{false};
    int liveInfoFramesRemaining_{0};
    FramePtr cachedComposited_;
    const AVFrame* cachedCompositedIdentity_{};
    std::int64_t cachedCompositedSecondIndex_{-1};
    std::uint8_t* avioBuffer_{nullptr};
    bool headerWritten_{false};
    std::optional<std::string> notice_;
    FramePtr cachedSlate_;
    std::shared_ptr<AsyncRenderedFrame> pendingSlateRender_;
    FramePtr cachedIdent_;
    FramePtr cachedTestcard_;
    std::shared_ptr<AsyncRenderedFrame> pendingTestcardRender_;
    FramePtr cachedSlide_;
    std::vector<std::filesystem::path> activeSlidePaths_;
    std::size_t slideIndex_{0};
    std::int64_t slideDurationFrames_{1};
    std::int64_t nextSlideFrame_{0};
    std::vector<bool> morseUnits_;
    std::vector<bool> noticeMorseUnits_;
    std::int64_t morseUnitSamples_{1};
    std::int64_t nextIdentFrame_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t identStartFrame_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t identEndFrame_{0};
    std::int64_t identStartSample_{std::numeric_limits<std::int64_t>::max()};
    bool noticeMorsePending_{false};
    bool noticeMorseActive_{false};
    bool noticeMorsePlayedForNotice_{false};
    std::int64_t noticeMorseStartSample_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t noticeMorseEndSample_{0};
    std::int64_t audioSampleIndex_{0};
    int audioFrameSamples_{1024};
    bool firstIdent_{true};
    std::int64_t lastVideoPts_{AV_NOPTS_VALUE};
    std::deque<FramePtr> submittedVideoFrames_;
    FramePtr heldSubmittedVideoFrame_;
    FramePtr heldRenderedSubmittedVideoFrame_;
    std::string submittedVideoSource_{"submitted"};
    std::int64_t submittedVideoBaseFrame_{AV_NOPTS_VALUE};
    std::chrono::steady_clock::time_point submittedVideoAt_{};
    std::condition_variable_any submittedVideoConsumed_;
    AudioFifoPtr submittedAudioFifo_;
    std::string submittedAudioSource_{"submitted"};
    std::int64_t submittedAudioStartSample_{0};
    bool submittedSourceHasAudio_{false};
    std::condition_variable_any submittedAudioConsumed_;
    std::chrono::steady_clock::time_point statsStartedAt_{};
    std::chrono::steady_clock::time_point nextStatsLogAt_{};
    std::uint64_t statsOutputFrames_{0};
    std::uint64_t statsGeneratedVideoFrames_{0};
    std::uint64_t statsSubmittedVideoFrames_{0};
    std::uint64_t statsHeldVideoFrames_{0};
    std::uint64_t statsAudioFrames_{0};
    std::uint64_t statsSubmittedAudioInputFrames_{0};
    std::uint64_t statsSubmittedAudioInputSamples_{0};
    std::uint64_t statsSubmittedAudioOutputFrames_{0};
    std::chrono::steady_clock::duration maxWriteFrameDuration_{};
    std::chrono::steady_clock::duration maxVideoEncodeDuration_{};
    std::chrono::steady_clock::duration maxAudioEncodeDuration_{};
    mutable std::mutex outputMutex_;
};

} // namespace

MediaPipeline::MediaPipeline(RepeaterConfig config)
    : config_{std::move(config)}
    , output_{config_.pluto}
{
    ensureLibavReady();
    worker_ = std::thread{[this] {
        workerLoop();
    }};
}

MediaPipeline::~MediaPipeline()
{
    stopWorker();
}

void MediaPipeline::select(std::optional<ActiveInput> input)
{
    std::lock_guard lock{mutex_};
    active_ = std::move(input);
    if (!active_.has_value()) {
        streamIndicator_.reset();
        sessionStreamInfo_.reset();
        pendingSessionStreamInfo_.reset();
        liveRetryAfter_ = {};
    }
}

void MediaPipeline::setBeaconAllowed(bool allowed)
{
    std::lock_guard lock{mutex_};
    beaconAllowed_ = allowed;
}

void MediaPipeline::setAccessNotice(std::optional<std::string> notice, bool endTone)
{
    std::lock_guard lock{mutex_};
    accessNotice_ = std::move(notice);
    accessNoticeEndTone_ = accessNotice_.has_value() && endTone;
    inputReady_.notify_all();
}

void MediaPipeline::setAccessNoticeEndTone(bool endTone)
{
    std::lock_guard lock{mutex_};
    accessNoticeEndTone_ = accessNotice_.has_value() && endTone;
    inputReady_.notify_all();
}

void MediaPipeline::playFallbackVideo(std::string path)
{
    std::lock_guard lock{mutex_};
    enterFallbackVideo(std::move(path));
}

void MediaPipeline::stopFallbackVideo()
{
    std::lock_guard lock{mutex_};
    if (mode_ != MediaPipelineMode::fallbackVideo) {
        return;
    }
    fallbackVideoPath_.reset();
    pendingFallbackVideoSeek_.reset();
    fallbackVideoStatus_.reset();
    pendingFallbackVideoStatus_ = "null";
    enterFallback(std::chrono::steady_clock::now(), beaconAllowed_ || accessNotice_.has_value());
}

void MediaPipeline::seekFallbackVideo(std::chrono::milliseconds position)
{
    std::lock_guard lock{mutex_};
    pendingFallbackVideoSeek_ = std::max(std::chrono::milliseconds{0}, position);
    inputReady_.notify_all();
}

void MediaPipeline::setPreviewEnabled(bool enabled)
{
    output_.setPreviewEnabled(enabled);
}

void MediaPipeline::tick(std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock{mutex_};
    if (mode_ == MediaPipelineMode::fallbackVideo && !active_.has_value()) {
        return;
    }
    if (active_.has_value()) {
        if (mode_ == MediaPipelineMode::fallbackVideo) {
            fallbackVideoPath_.reset();
            pendingFallbackVideoSeek_.reset();
            fallbackVideoStatus_.reset();
            pendingFallbackVideoStatus_ = "null";
        }
        if (isAnalogueInput(config_, *active_)) {
            if (now < analogueRetryAfter_) {
                enterFallback(now, beaconAllowed_ || accessNotice_.has_value());
                return;
            }
            enterAnalogue(now);
        } else {
            if (now < liveRetryAfter_) {
                enterFallback(now, true);
                return;
            }
            enterRetransmit(now);
        }
        return;
    }

    if (config_.fallback.enabled) {
        enterFallback(now, beaconAllowed_ || accessNotice_.has_value());
    } else {
        enterIdle();
    }
}

void MediaPipeline::write(std::span<const std::byte> packet)
{
    if (packet.empty()) {
        return;
    }

    {
        std::lock_guard lock{mutex_};
        if (!active_.has_value()) {
            return;
        }
        if (mode_ == MediaPipelineMode::fallbackVideo) {
            fallbackVideoPath_.reset();
            pendingFallbackVideoSeek_.reset();
            fallbackVideoStatus_.reset();
            pendingFallbackVideoStatus_ = "null";
        }
        if (std::chrono::steady_clock::now() < liveRetryAfter_) {
            return;
        }
        lastInput_ = std::chrono::steady_clock::now();
        enterRetransmit(lastInput_);
    }
    queueInput(packet);
}

MediaPipelineMode MediaPipeline::mode() const
{
    std::lock_guard lock{mutex_};
    return mode_;
}

std::optional<std::string> MediaPipeline::takeStreamInfoUpdate()
{
    std::lock_guard lock{mutex_};
    auto update = std::move(pendingSessionStreamInfo_);
    pendingSessionStreamInfo_.reset();
    return update;
}

std::optional<std::string> MediaPipeline::takeFallbackVideoStatusUpdate()
{
    std::lock_guard lock{mutex_};
    auto update = std::move(pendingFallbackVideoStatus_);
    pendingFallbackVideoStatus_.reset();
    return update;
}

std::optional<std::string> MediaPipeline::fallbackVideoStatus() const
{
    std::lock_guard lock{mutex_};
    return fallbackVideoStatus_;
}

void MediaPipeline::setSessionStreamInfo(std::optional<std::string> streamInfo)
{
    std::lock_guard lock{mutex_};
    if (sessionStreamInfo_ == streamInfo) {
        return;
    }
    sessionStreamInfo_ = streamInfo;
    pendingSessionStreamInfo_ = std::move(streamInfo);
}

void MediaPipeline::setFallbackVideoStatus(std::optional<std::string> statusJson)
{
    std::lock_guard lock{mutex_};
    const auto pending = statusJson.has_value() ? statusJson : std::optional<std::string>{"null"};
    if (fallbackVideoStatus_ == statusJson && pendingFallbackVideoStatus_ == pending) {
        return;
    }
    fallbackVideoStatus_ = std::move(statusJson);
    pendingFallbackVideoStatus_ = pending;
}

void MediaPipeline::ensureLibavReady()
{
    avdevice_register_all();

    const auto networkStatus = avformat_network_init();
    if (networkStatus < 0) {
        throw std::runtime_error{"libav network init failed: " + avError(networkStatus)};
    }

    if (findVideoDecoder(AV_CODEC_ID_MPEG2VIDEO) == nullptr
        && findVideoDecoder(AV_CODEC_ID_H264) == nullptr
        && findVideoDecoder(AV_CODEC_ID_HEVC) == nullptr) {
        throw std::runtime_error{"libav has no MPEG-2, H.264, or H.265 decoder available"};
    }

    bool h264EncoderAvailable = false;
    std::string h264EncoderNames;
    for (const auto& encoderName : h264EncoderCandidates(config_)) {
        if (!h264EncoderNames.empty()) {
            h264EncoderNames += ", ";
        }
        h264EncoderNames += encoderName;
        if (avcodec_find_encoder_by_name(encoderName.c_str()) != nullptr) {
            h264EncoderAvailable = true;
        }
    }
    if (!h264EncoderAvailable) {
        throw std::runtime_error{"libav has no usable H.264 encoder candidate: " + h264EncoderNames};
    }
    if (avcodec_find_encoder(AV_CODEC_ID_AAC) == nullptr) {
        throw std::runtime_error{"libav has no AAC encoder available"};
    }

    if (config_.media.backend == "gstreamer") {
        const auto gst = probeGStreamerBackend();
        std::cout << "media pipeline requested GStreamer backend: " << gst.detail << '\n';
    }
}

void MediaPipeline::enterRetransmit(std::chrono::steady_clock::time_point now)
{
    if (mode_ == MediaPipelineMode::retransmit && transmitEnabled_) {
        return;
    }

    lastInput_ = now;
    mode_ = MediaPipelineMode::retransmit;
    transmitEnabled_ = true;
    streamIndicator_.reset();
    output_.setTransmitEnabled(true);
    inputReady_.notify_all();
    std::cout << "media pipeline entering retransmit mode; queued TS will require watermark transcode\n";
}

void MediaPipeline::enterAnalogue(std::chrono::steady_clock::time_point now)
{
    if (mode_ == MediaPipelineMode::analogue && transmitEnabled_) {
        return;
    }

    lastInput_ = now;
    inputQueue_.clear();
    mode_ = MediaPipelineMode::analogue;
    transmitEnabled_ = true;
    output_.setTransmitEnabled(true);
    inputReady_.notify_all();
    std::cout << "media pipeline entering analogue capture mode\n";
}

void MediaPipeline::enterFallbackVideo(std::string path)
{
    if (path.empty()) {
        std::cerr << "wh-repeater: fallback video play requested with no path\n";
        return;
    }

    inputQueue_.clear();
    fallbackVideoPath_ = std::move(path);
    pendingFallbackVideoSeek_.reset();
    fallbackVideoStatus_.reset();
    pendingFallbackVideoStatus_ = "null";
    mode_ = MediaPipelineMode::fallbackVideo;
    transmitEnabled_ = true;
    output_.setTransmitEnabled(true);
    inputReady_.notify_all();
    std::cout << "media pipeline entering fallback video mode path=" << *fallbackVideoPath_ << '\n';
}

void MediaPipeline::enterFallback(std::chrono::steady_clock::time_point now, bool transmitEnabled)
{
    (void)now;
    if (mode_ == MediaPipelineMode::fallback && transmitEnabled_ == transmitEnabled) {
        return;
    }

    inputQueue_.clear();
    pendingFallbackVideoSeek_.reset();
    fallbackVideoStatus_.reset();
    pendingFallbackVideoStatus_ = "null";
    mode_ = MediaPipelineMode::fallback;
    transmitEnabled_ = transmitEnabled;
    output_.setTransmitEnabled(transmitEnabled_);
    inputReady_.notify_all();
    std::cout << "media pipeline entering fallback mode";
    if (!transmitEnabled_) {
        std::cout << " with TX muted";
    }
    if (!config_.fallback.videoPaths.empty()) {
        std::cout << " videos=" << config_.fallback.videoPaths.size();
    }
    std::cout << '\n';
}

void MediaPipeline::enterIdle()
{
    if (mode_ == MediaPipelineMode::idle) {
        return;
    }

    mode_ = MediaPipelineMode::idle;
    transmitEnabled_ = false;
    output_.setTransmitEnabled(false);
    inputReady_.notify_all();
    std::cout << "media pipeline idle\n";
}

void MediaPipeline::workerLoop()
{
    std::int64_t fallbackFrameIndex = 0;
    std::unique_ptr<PersistentRtmpMuxer> rtmpMuxer;
    std::unique_ptr<FallbackMuxer> fallbackMuxer;
    std::unique_ptr<LiveTranscoder> liveTranscoder;
#if defined(WH_REPEATER_HAVE_GSTREAMER)
    std::unique_ptr<GStreamerLiveTranscoder> gstLiveTranscoder;
#endif
    std::unique_ptr<FallbackVideoTranscoder> fallbackVideoTranscoder;
    std::optional<std::string> activeFallbackVideoPath;
    std::unique_ptr<AnalogueCaptureTranscoder> analogueTranscoder;
#if defined(WH_REPEATER_HAVE_GSTREAMER)
    std::unique_ptr<GStreamerAnalogueCaptureTranscoder> gstAnalogueTranscoder;
#endif
    auto nextFrameAt = std::chrono::steady_clock::now();
    auto nextSlowOutputLogAt = std::chrono::steady_clock::time_point{};
    std::chrono::steady_clock::time_point liveAttemptStartedAt{};
    bool liveAcquisitionWarningLogged = false;
    bool liveStreamInfoShown = false;
    auto advanceFrameClock = [](std::chrono::steady_clock::time_point& nextFrame,
                                std::chrono::microseconds frameInterval) {
        advanceOutputFrameClock(nextFrame, frameInterval, std::chrono::steady_clock::now());
    };
    auto nextFrameDeadline = [&] {
        const auto now = std::chrono::steady_clock::now();
        return nextFrameAt > now ? nextFrameAt : now;
    };

    if (config_.media.backend != "gstreamer" && config_.streaming.rtmp.enabled && !config_.streaming.rtmp.url.empty()) {
        rtmpMuxer = std::make_unique<PersistentRtmpMuxer>(config_);
    }

    auto retireLiveTranscoders = [&] {
        if (liveTranscoder) {
            liveTranscoder->stop();
            liveTranscoder.reset();
        }
#if defined(WH_REPEATER_HAVE_GSTREAMER)
        if (gstLiveTranscoder) {
            gstLiveTranscoder->stop();
            gstLiveTranscoder.reset();
        }
#endif
        liveStreamInfoShown = false;
    };

    while (true) {
        MediaPipelineMode mode;
        std::optional<ActiveInput> active;
        std::optional<std::string> notice;
        bool noticeEndTone{};
        std::optional<std::string> streamIndicator;
        std::optional<std::string> fallbackVideoPath;
        std::optional<std::chrono::milliseconds> fallbackVideoSeek;
        bool beaconAllowed{};
        {
            std::unique_lock lock{mutex_};
            inputReady_.wait_for(lock, std::chrono::milliseconds{20}, [this] {
                return stopping_ || mode_ != MediaPipelineMode::idle;
            });
            if (stopping_) {
                break;
            }
            mode = mode_;
            active = active_;
            notice = accessNotice_;
            noticeEndTone = accessNoticeEndTone_;
            streamIndicator = streamIndicator_;
            fallbackVideoPath = fallbackVideoPath_;
            fallbackVideoSeek = pendingFallbackVideoSeek_;
            pendingFallbackVideoSeek_.reset();
            beaconAllowed = beaconAllowed_;
        }

        try {
            if (mode == MediaPipelineMode::fallback) {
                retireLiveTranscoders();
                fallbackVideoTranscoder.reset();
                activeFallbackVideoPath.reset();
                analogueTranscoder.reset();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                gstAnalogueTranscoder.reset();
#endif
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_, rtmpMuxer.get());
                    fallbackFrameIndex = 0;
                    nextFrameAt = std::chrono::steady_clock::now();
                }
                if (notice.has_value()) {
                    fallbackMuxer->setNotice(notice, noticeEndTone);
                } else if (!beaconAllowed) {
                    fallbackMuxer->setNotice(sleepingMessage(config_));
                } else {
                    fallbackMuxer->setNotice(std::nullopt);
                }
                fallbackMuxer->setStreamInfo(notice.has_value() ? std::nullopt : streamIndicator);
                std::this_thread::sleep_until(nextFrameAt);
                fallbackFrameIndex = std::max(fallbackFrameIndex, fallbackMuxer->nextVideoPts());
                fallbackMuxer->writeFrame(fallbackFrameIndex++);
                advanceFrameClock(nextFrameAt, outputFrameInterval(outputFrameRate(config_)));
                continue;
            }

            if (mode == MediaPipelineMode::fallbackVideo) {
                retireLiveTranscoders();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                gstAnalogueTranscoder.reset();
#endif
                analogueTranscoder.reset();
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_, rtmpMuxer.get());
                    fallbackFrameIndex = fallbackMuxer->nextVideoPts();
                    nextFrameAt = std::chrono::steady_clock::now();
                }
                if (fallbackVideoTranscoder
                    && fallbackVideoPath.has_value()
                    && activeFallbackVideoPath != fallbackVideoPath) {
                    fallbackVideoTranscoder.reset();
                    activeFallbackVideoPath.reset();
                    fallbackMuxer->setStreamInfo(std::nullopt);
                }
                if (!fallbackVideoTranscoder && fallbackVideoPath.has_value()) {
                    fallbackVideoTranscoder = std::make_unique<FallbackVideoTranscoder>(
                        config_,
                        *fallbackMuxer,
                        *fallbackVideoPath,
                        [this](std::optional<std::string> statusJson) {
                            setFallbackVideoStatus(std::move(statusJson));
                        });
                    activeFallbackVideoPath = fallbackVideoPath;
                }
                if (fallbackVideoTranscoder && fallbackVideoSeek.has_value()) {
                    fallbackVideoTranscoder->seekTo(*fallbackVideoSeek);
                }
                bool playbackFinished{};
                {
                    std::unique_lock lock{mutex_};
                    inputReady_.wait_until(lock, nextFrameDeadline(), [this, &fallbackVideoTranscoder] {
                        return stopping_ || mode_ != MediaPipelineMode::fallbackVideo
                            || (fallbackVideoTranscoder && fallbackVideoTranscoder->finished());
                    });
                    if (stopping_) {
                        break;
                    }
                    playbackFinished = fallbackVideoTranscoder && fallbackVideoTranscoder->finished();
                    if (playbackFinished && mode_ == MediaPipelineMode::fallbackVideo) {
                        fallbackVideoPath_.reset();
                        mode_ = MediaPipelineMode::fallback;
                        transmitEnabled_ = beaconAllowed_ || accessNotice_.has_value();
                        output_.setTransmitEnabled(transmitEnabled_);
                        inputReady_.notify_all();
                    }
                }
                if (playbackFinished) {
                    fallbackVideoTranscoder.reset();
                    activeFallbackVideoPath.reset();
                    fallbackMuxer->setStreamInfo(std::nullopt);
                    std::cout << "media pipeline fallback video finished; returning to generated fallback stream\n";
                } else {
                    const auto frameInterval = outputFrameInterval(outputFrameRate(config_));
                    fallbackMuxer->setNotice(std::nullopt);
                    std::this_thread::sleep_until(nextFrameAt);
                    fallbackFrameIndex = std::max(fallbackFrameIndex, fallbackMuxer->nextVideoPts());
                    const auto writeStartedAt = std::chrono::steady_clock::now();
                    fallbackMuxer->writeFrame(fallbackFrameIndex++, true);
                    const auto writeDuration = std::chrono::steady_clock::now() - writeStartedAt;
                    if (writeDuration > frameInterval) {
                        const auto logNow = std::chrono::steady_clock::now();
                        if (logNow >= nextSlowOutputLogAt) {
                            std::cerr << "wh-repeater: fallback video output frame took "
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(writeDuration).count()
                                      << " ms; target interval "
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(frameInterval).count()
                                      << " ms\n";
                            nextSlowOutputLogAt = logNow + std::chrono::seconds{5};
                        }
                    }
                    advanceFrameClock(nextFrameAt, frameInterval);
                }
            } else if (mode == MediaPipelineMode::analogue) {
                retireLiveTranscoders();
                fallbackVideoTranscoder.reset();
                activeFallbackVideoPath.reset();
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_, rtmpMuxer.get());
                    fallbackFrameIndex = fallbackMuxer->nextVideoPts();
                    nextFrameAt = std::chrono::steady_clock::now();
                }
                if (!analogueTranscoder) {
                    analogueTranscoder = std::make_unique<AnalogueCaptureTranscoder>(config_, *fallbackMuxer, active);
                }
                bool captureFinished{};
                {
                    std::unique_lock lock{mutex_};
                    inputReady_.wait_until(lock, nextFrameDeadline(), [this, &analogueTranscoder
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                                                                                 , &gstAnalogueTranscoder
#endif
                    ] {
                        return stopping_ || mode_ != MediaPipelineMode::analogue
                            || (analogueTranscoder && analogueTranscoder->finished())
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                            || (gstAnalogueTranscoder && gstAnalogueTranscoder->finished())
#endif
                            ;
                    });
                    if (stopping_) {
                        break;
                    }
                    captureFinished = (analogueTranscoder && analogueTranscoder->finished())
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                        || (gstAnalogueTranscoder && gstAnalogueTranscoder->finished())
#endif
                        ;
                    if (captureFinished && mode_ == MediaPipelineMode::analogue) {
                        analogueRetryAfter_ = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                        mode_ = MediaPipelineMode::fallback;
                        transmitEnabled_ = beaconAllowed_ || accessNotice_.has_value();
                        output_.setTransmitEnabled(transmitEnabled_);
                        inputReady_.notify_all();
                    }
                }
                if (captureFinished) {
                    analogueTranscoder.reset();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                    gstAnalogueTranscoder.reset();
#endif
                    fallbackMuxer->setStreamInfo(std::nullopt);
                    std::cerr << "wh-repeater: analogue capture ended; returning to generated fallback stream\n";
                } else {
                    const auto frameInterval = outputFrameInterval(outputFrameRate(config_));
                    fallbackMuxer->setNotice(std::nullopt);
                    std::this_thread::sleep_until(nextFrameAt);
                    fallbackFrameIndex = std::max(fallbackFrameIndex, fallbackMuxer->nextVideoPts());
                    fallbackMuxer->writeFrame(fallbackFrameIndex++, true);
                    advanceFrameClock(nextFrameAt, frameInterval);
                }
            } else if (mode == MediaPipelineMode::retransmit) {
                analogueTranscoder.reset();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                gstAnalogueTranscoder.reset();
#endif
                fallbackVideoTranscoder.reset();
                activeFallbackVideoPath.reset();
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_, rtmpMuxer.get());
                    fallbackFrameIndex = fallbackMuxer->nextVideoPts();
                    nextFrameAt = std::chrono::steady_clock::now();
                }
                if (!liveTranscoder) {
                    liveTranscoder = std::make_unique<LiveTranscoder>(config_, *fallbackMuxer, active);
                    liveAttemptStartedAt = std::chrono::steady_clock::now();
                    liveAcquisitionWarningLogged = false;
                }
                std::vector<std::byte> queued;
                bool liveFinished{};
                std::string liveError;
                bool liveReady = liveTranscoder && liveTranscoder->ready();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                liveReady = liveReady || (gstLiveTranscoder && gstLiveTranscoder->ready());
#endif
                std::unique_lock lock{mutex_};
                inputReady_.wait_until(lock, nextFrameDeadline(), [this, &liveTranscoder
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                                                                             , &gstLiveTranscoder
#endif
                ] {
                    return stopping_ || mode_ != MediaPipelineMode::retransmit || !inputQueue_.empty()
                        || (liveTranscoder && liveTranscoder->ready())
                        || (liveTranscoder && liveTranscoder->finished())
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                        || (gstLiveTranscoder && gstLiveTranscoder->ready())
                        || (gstLiveTranscoder && gstLiveTranscoder->finished())
#endif
                        ;
                });
                if (stopping_) {
                    break;
                }
                liveReady = (liveTranscoder && liveTranscoder->ready())
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                    || (gstLiveTranscoder && gstLiveTranscoder->ready())
#endif
                    ;
                liveFinished = (liveTranscoder && liveTranscoder->finished())
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                    || (gstLiveTranscoder && gstLiveTranscoder->finished())
#endif
                    ;
                if (!liveReady && !liveFinished && liveAttemptStartedAt != std::chrono::steady_clock::time_point{}
                    && std::chrono::steady_clock::now() - liveAttemptStartedAt > liveVideoAcquisitionTimeout) {
                    liveError = "no valid decoded video after "
                        + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(liveVideoAcquisitionTimeout).count())
                        + " seconds";
                    if (!liveAcquisitionWarningLogged) {
                        std::cerr << "wh-repeater: received stream not decodable yet; continuing to wait"
                                  << ": " << liveError << '\n';
                        liveAcquisitionWarningLogged = true;
                    }
                    if (active.has_value()) {
                        streamIndicator_ = receivedStreamErrorText(*active, liveError);
                    }
                    liveAttemptStartedAt = std::chrono::steady_clock::now();
                }
                if (liveFinished) {
                    if (liveError.empty() && liveTranscoder) {
                        liveError = liveTranscoder->error();
                    }
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                    if (liveError.empty() && gstLiveTranscoder) {
                        liveError = gstLiveTranscoder->error();
                    }
#endif
                    liveRetryAfter_ = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    if (active.has_value()) {
                        streamIndicator_ = receivedStreamErrorText(*active, liveError);
                    } else {
                        streamIndicator_.reset();
                    }
                    mode_ = MediaPipelineMode::fallback;
                    transmitEnabled_ = true;
                    output_.setTransmitEnabled(true);
                    inputQueue_.clear();
                    inputReady_.notify_all();
                } else if (liveReady) {
                    liveAcquisitionWarningLogged = false;
                }
                if (!inputQueue_.empty()) {
                    queued.insert(queued.end(), inputQueue_.begin(), inputQueue_.end());
                    inputQueue_.clear();
                }
                lock.unlock();
                if (!queued.empty()) {
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                    if (gstLiveTranscoder) {
                        gstLiveTranscoder->append(queued);
                    } else
#endif
                    if (liveTranscoder) {
                        liveTranscoder->append(queued);
                    }
                }
                if (liveReady) {
                    const auto frameInterval = outputFrameInterval(outputFrameRate(config_));
                    const auto liveFillTimeout = std::max<std::chrono::microseconds>(
                        frameInterval * 12,
                        std::chrono::milliseconds{500});
                    bool liveVideoStale = false;
                    if (liveTranscoder) {
                        const auto lastLiveFrameAt = liveTranscoder->lastVideoFrameAt();
                        liveVideoStale = lastLiveFrameAt == std::chrono::steady_clock::time_point{}
                            || std::chrono::steady_clock::now() - lastLiveFrameAt > liveFillTimeout;
                    }
                    if (liveVideoStale) {
                        if (notice.has_value()) {
                            fallbackMuxer->setNotice(notice, noticeEndTone);
                            fallbackMuxer->setStreamInfo(std::nullopt);
                        } else if (active.has_value()) {
                            fallbackMuxer->setNotice(std::nullopt);
                            fallbackMuxer->setStreamInfo(receivedStreamErrorText(*active, "waiting for live video frame"));
                        } else {
                            fallbackMuxer->setNotice(std::nullopt);
                            fallbackMuxer->setStreamInfo(std::nullopt);
                        }
                    } else {
                        fallbackMuxer->setNotice(std::nullopt);
                        if (liveTranscoder && !liveStreamInfoShown) {
                            const auto liveStreamInfo = liveTranscoder->streamInfo();
                            if (liveStreamInfo.has_value() && !liveStreamInfo->empty()) {
                                setSessionStreamInfo(liveStreamInfo);
                                fallbackMuxer->setLiveStreamInfo(liveStreamInfo);
                                liveStreamInfoShown = true;
                            }
                        }
                    }
                    std::this_thread::sleep_until(nextFrameAt);
                    fallbackFrameIndex = std::max(fallbackFrameIndex, fallbackMuxer->nextVideoPts());
                    fallbackMuxer->writeFrame(fallbackFrameIndex++, true);
                    advanceFrameClock(nextFrameAt, frameInterval);
                } else {
                    if (notice.has_value()) {
                        fallbackMuxer->setNotice(notice, noticeEndTone);
                    } else if (!beaconAllowed) {
                        fallbackMuxer->setNotice(sleepingMessage(config_));
                    } else {
                        fallbackMuxer->setNotice(std::nullopt);
                    }
                    if (streamIndicator.has_value()) {
                        fallbackMuxer->setStreamInfo(notice.has_value() ? std::nullopt : streamIndicator);
                    } else if (active.has_value()) {
                        fallbackMuxer->setStreamInfo(notice.has_value()
                                ? std::nullopt
                                : std::optional<std::string>{receivedStreamErrorText(*active, "waiting for valid video")});
                    } else {
                        fallbackMuxer->setStreamInfo(std::nullopt);
                    }
                    std::this_thread::sleep_until(nextFrameAt);
                    fallbackFrameIndex = std::max(fallbackFrameIndex, fallbackMuxer->nextVideoPts());
                    fallbackMuxer->writeFrame(fallbackFrameIndex++);
                    advanceFrameClock(nextFrameAt, outputFrameInterval(outputFrameRate(config_)));
                }
                if (liveFinished) {
                    retireLiveTranscoders();
                    liveAttemptStartedAt = {};
                    liveAcquisitionWarningLogged = false;
                    fallbackMuxer->setStreamInfo(streamIndicator_);
                    fallbackFrameIndex = fallbackMuxer->nextVideoPts();
                    nextFrameAt = std::chrono::steady_clock::now();
                    std::cerr << "wh-repeater: received stream decode failed; showing fallback"
                              << (liveError.empty() ? "" : ": ") << liveError << '\n';
                    continue;
                }
            } else {
                fallbackMuxer.reset();
                retireLiveTranscoders();
                fallbackVideoTranscoder.reset();
                activeFallbackVideoPath.reset();
                analogueTranscoder.reset();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
                gstAnalogueTranscoder.reset();
#endif
            }
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: media pipeline error: " << ex.what() << '\n';
            std::this_thread::sleep_for(std::chrono::seconds{1});
            fallbackMuxer.reset();
            retireLiveTranscoders();
            fallbackVideoTranscoder.reset();
            activeFallbackVideoPath.reset();
            analogueTranscoder.reset();
#if defined(WH_REPEATER_HAVE_GSTREAMER)
            gstAnalogueTranscoder.reset();
#endif
        }
    }
    fallbackMuxer.reset();
    liveTranscoder.reset();
    fallbackVideoTranscoder.reset();
    activeFallbackVideoPath.reset();
    analogueTranscoder.reset();
    rtmpMuxer.reset();
}

void MediaPipeline::stopWorker()
{
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
        inputReady_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MediaPipeline::queueInput(std::span<const std::byte> packet)
{
    std::lock_guard lock{mutex_};
    constexpr std::size_t maxQueuedBytes = 32 * 1024 * 1024;
    if (inputQueue_.size() + packet.size() > maxQueuedBytes) {
        inputQueue_.clear();
    }
    inputQueue_.insert(inputQueue_.end(), packet.begin(), packet.end());
    inputReady_.notify_all();
}

} // namespace whrepeater
