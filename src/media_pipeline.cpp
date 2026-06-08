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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace whrepeater {
namespace {

constexpr int fallbackWidth{1280};
constexpr int fallbackHeight{720};
constexpr std::string_view slateFont{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
constexpr int fallbackAudioSampleRate{48000};
constexpr double pi{3.14159265358979323846};

int fallbackFrameRate(const RepeaterConfig& config)
{
    return static_cast<int>(std::clamp(config.fallback.staticFrameRate, 1U, 25U));
}

int fallbackVideoBitrateKbps(const RepeaterConfig& config)
{
    const auto muxBudget = static_cast<int>(config.pluto.muxRateKbps * 6 / 10);
    return std::max(250, std::min(static_cast<int>(config.pluto.videoBitrateKbps), muxBudget));
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

struct YuvColor {
    std::uint8_t y;
    std::uint8_t u;
    std::uint8_t v;
};

void fillTestcard(AVFrame& frame)
{
    constexpr std::array<YuvColor, 8> bars{{
        {235, 128, 128},
        {226, 146, 16},
        {194, 49, 206},
        {180, 67, 44},
        {145, 206, 212},
        {132, 224, 30},
        {84, 80, 240},
        {16, 128, 128},
    }};

    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const auto bar = std::min<std::size_t>(bars.size() - 1, static_cast<std::size_t>(x * bars.size() / frame.width));
            frame.data[0][y * frame.linesize[0] + x] = bars[bar].y;
        }
    }
    for (int y = 0; y < frame.height / 2; ++y) {
        for (int x = 0; x < frame.width / 2; ++x) {
            const auto bar = std::min<std::size_t>(bars.size() - 1, static_cast<std::size_t>((x * 2) * bars.size() / frame.width));
            frame.data[1][y * frame.linesize[1] + x] = bars[bar].u;
            frame.data[2][y * frame.linesize[2] + x] = bars[bar].v;
        }
    }

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

FramePtr allocateVideoFrame()
{
    FramePtr frame{av_frame_alloc()};
    if (!frame) {
        throw std::runtime_error{"allocate fallback frame failed"};
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = fallbackWidth;
    frame->height = fallbackHeight;
    checkAv(av_frame_get_buffer(frame.get(), 32), "allocate fallback frame buffer");
    checkAv(av_frame_make_writable(frame.get()), "make fallback frame writable");
    return frame;
}

std::string slateFilter(std::string_view text)
{
    const auto lines = splitLines(text);
    std::ostringstream filter;
    filter << "drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(lines[0])
           << "':x=(w-text_w)/2:y=120:fontsize=70:fontcolor=white";
    constexpr std::array<int, 5> bodyY{250, 320, 395, 455, 540};
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto y = index - 1 < bodyY.size() ? bodyY[index - 1] : 500 + static_cast<int>(index - bodyY.size()) * 60;
        filter << ",drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(lines[index])
               << "':x=(w-text_w)/2:y=" << y << ":fontsize=42:fontcolor=white";
    }
    return filter.str();
}

FramePtr renderFilteredFrame(std::string_view filter, int frameRate, void (*fill)(AVFrame&))
{
    auto inputFrame = allocateVideoFrame();
    fill(*inputFrame);

    AVFilterGraph* rawGraph = avfilter_graph_alloc();
    if (rawGraph == nullptr) {
        throw std::runtime_error{"allocate video filter graph failed"};
    }
    std::unique_ptr<AVFilterGraph, AvFilterGraphDeleter> graph{rawGraph};

    AVFilterContext* source{};
    AVFilterContext* sink{};
    const auto sourceArgs = "video_size=" + std::to_string(fallbackWidth) + "x" + std::to_string(fallbackHeight)
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

FramePtr renderSlateFrame(std::string_view text, int frameRate)
{
    return renderFilteredFrame(slateFilter(text), frameRate, fillBlue);
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

std::string identFilter(std::string_view text)
{
    std::ostringstream filter;
    filter << "drawtext=fontfile='" << slateFont << "':text='" << escapeDrawText(text)
           << "':x=46:y=44:fontsize=34:fontcolor=white"
           << ":box=1:boxcolor=0x0057b8@0.68:boxborderw=14";
    return filter.str();
}

std::string streamInfoText(const ActiveInput& input, std::string_view codec, int width, int height, int frameRate)
{
    std::ostringstream text;
    text << "RX" << input.receiver.value;
    if (!input.target.label.empty()) {
        text << " " << input.target.label;
    }
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

std::string liveOverlayFilter(const RepeaterConfig& config, const std::optional<std::string>& streamInfo, int frameRate)
{
    auto filter = identFilter(identText(config));
    if (streamInfo.has_value() && !streamInfo->empty()) {
        filter += "," + streamInfoFilter(*streamInfo, frameRate);
    }
    return filter;
}

FramePtr renderIdentFrame(const RepeaterConfig& config, int frameRate)
{
    return renderFilteredFrame(identFilter(identText(config)), frameRate, fillBlack);
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
    return renderFilteredFrame(testcardFilter(identText(config)), frameRate, fillTestcard);
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
    const auto callsign = config.pluto.callsign.empty() ? std::string{"WH Repeater"} : config.pluto.callsign;
    return callsign + " is in power saving mode\n"
        + "sleeping between " + config.beaconSchedule.endTime + " and " + config.beaconSchedule.startTime + "\n"
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
                             int width,
                             int height,
                             int frameRate,
                             int videoBitrateKbps)
{
    codec.codec_type = AVMEDIA_TYPE_VIDEO;
    codec.width = width;
    codec.height = height;
    codec.pix_fmt = AV_PIX_FMT_YUV420P;
    codec.time_base = AVRational{1, frameRate};
    codec.framerate = AVRational{frameRate, 1};
    codec.gop_size = frameRate * 2;
    codec.max_b_frames = 0;
    codec.bit_rate = static_cast<std::int64_t>(videoBitrateKbps) * 1000;
    codec.rc_max_rate = codec.bit_rate;
    codec.rc_buffer_size = static_cast<int>(codec.bit_rate / frameRate);
    if ((format.oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

EncoderOpenResult openH264Encoder(const AVFormatContext& format,
                                  int width,
                                  int height,
                                  int frameRate,
                                  int videoBitrateKbps)
{
    constexpr std::array<std::string_view, 2> preferredEncoders{"h264_v4l2m2m", "libx264"};
    std::string errors;

    for (const auto encoderName : preferredEncoders) {
        const auto* encoder = avcodec_find_encoder_by_name(std::string{encoderName}.c_str());
        if (encoder == nullptr) {
            continue;
        }

        AVCodecContext* codec = avcodec_alloc_context3(encoder);
        if (codec == nullptr) {
            throw std::runtime_error{"allocate H.264 encoder failed"};
        }
        configureEncoderContext(*codec, format, width, height, frameRate, videoBitrateKbps);

        AVDictionary* options = nullptr;
        if (encoderName == "libx264") {
            av_dict_set(&options, "preset", "veryfast", 0);
            av_dict_set(&options, "tune", "zerolatency", 0);
            const auto x264Params = "bframes=0:rc-lookahead=0:vbv-maxrate="
                + std::to_string(videoBitrateKbps)
                + ":vbv-bufsize="
                + std::to_string(std::max(1, videoBitrateKbps / frameRate));
            av_dict_set(&options, "x264-params", x264Params.c_str(), 0);
        }

        const auto status = avcodec_open2(codec, encoder, &options);
        av_dict_free(&options);
        if (status >= 0) {
            return EncoderOpenResult{codec, encoder, std::string{encoderName}};
        }

        errors += std::string{encoderName} + ": " + avError(status) + "; ";
        avcodec_free_context(&codec);
    }

    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (encoder != nullptr) {
        AVCodecContext* codec = avcodec_alloc_context3(encoder);
        if (codec == nullptr) {
            throw std::runtime_error{"allocate H.264 encoder failed"};
        }
        configureEncoderContext(*codec, format, width, height, frameRate, videoBitrateKbps);
        const auto status = avcodec_open2(codec, encoder, nullptr);
        if (status >= 0) {
            return EncoderOpenResult{codec, encoder, encoder->name == nullptr ? "h264" : encoder->name};
        }
        errors += std::string{"h264: "} + avError(status) + "; ";
        avcodec_free_context(&codec);
    }

    throw std::runtime_error{"open H.264 encoder failed: " + errors};
}

EncoderOpenResult openFallbackEncoder(const AVFormatContext& format, int frameRate, int videoBitrateKbps)
{
    return openH264Encoder(format, fallbackWidth, fallbackHeight, frameRate, videoBitrateKbps);
}

class BlockingTsInput {
public:
    void append(std::span<const std::byte> data)
    {
        {
            std::lock_guard lock{mutex_};
            if (buffer_.size() + data.size() > maxBufferedBytes) {
                buffer_.clear();
            }
            buffer_.insert(buffer_.end(), data.begin(), data.end());
        }
        ready_.notify_one();
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
    static constexpr std::size_t maxBufferedBytes{4 * 1024 * 1024};
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

FramePtr decodeSlideFrame(const std::filesystem::path& path)
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

    auto output = allocateVideoFrame();
    fillBlack(*output);

    const auto scale = std::min(static_cast<double>(fallbackWidth) / static_cast<double>(decoded->width),
                                static_cast<double>(fallbackHeight) / static_cast<double>(decoded->height));
    auto dstWidth = std::max(2, static_cast<int>(std::floor(decoded->width * scale))) & ~1;
    auto dstHeight = std::max(2, static_cast<int>(std::floor(decoded->height * scale))) & ~1;
    dstWidth = std::min(dstWidth, fallbackWidth);
    dstHeight = std::min(dstHeight, fallbackHeight);
    const auto dstX = ((fallbackWidth - dstWidth) / 2) & ~1;
    const auto dstY = ((fallbackHeight - dstHeight) / 2) & ~1;

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
    if (codecId == AV_CODEC_ID_H264) {
        if (const auto* decoder = avcodec_find_decoder_by_name("h264_v4l2m2m"); decoder != nullptr) {
            return decoder;
        }
    }
    if (codecId == AV_CODEC_ID_HEVC) {
        if (const auto* decoder = avcodec_find_decoder_by_name("hevc_v4l2m2m"); decoder != nullptr) {
            return decoder;
        }
    }
    if (codecId == AV_CODEC_ID_MPEG2VIDEO) {
        if (const auto* decoder = avcodec_find_decoder_by_name("mpeg2_v4l2m2m"); decoder != nullptr) {
            return decoder;
        }
    }
    return avcodec_find_decoder(codecId);
}

int streamFrameRate(const AVStream& stream)
{
    AVRational rate = stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0
        ? stream.avg_frame_rate
        : stream.r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        return 25;
    }
    return std::clamp(static_cast<int>(std::llround(av_q2d(rate))), 1, 50);
}

int evenDimension(int value)
{
    return std::max(16, value & ~1);
}

class PersistentRtmpMuxer {
public:
    explicit PersistentRtmpMuxer(const RepeaterConfig& config)
        : config_{config}
    {
    }

    ~PersistentRtmpMuxer()
    {
        close();
    }

    PersistentRtmpMuxer(const PersistentRtmpMuxer&) = delete;
    PersistentRtmpMuxer& operator=(const PersistentRtmpMuxer&) = delete;

    void configureVideo(const AVCodecContext& codec)
    {
        std::lock_guard lock{mutex_};
        if (!enabled() || videoConfigured_) {
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
        openLocked();
    }

    void configureAudio(const AVCodecContext& codec)
    {
        std::lock_guard lock{mutex_};
        if (!enabled() || audioConfigured_) {
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

    void openLocked()
    {
        if (!enabled() || !videoConfigured_ || headerWritten_) {
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
            const auto openStatus = avio_open2(&rtmpFormat_->pb, config_.streaming.rtmp.url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (openStatus < 0) {
                markConnectFailed("RTMP connect failed", openStatus);
                closeLocked(false);
                return;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
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
        const auto status = av_interleaved_write_frame(rtmpFormat_, copy.get());
        if (status < 0) {
            std::cerr << "wh-repeater: RTMP " << (video ? "video" : "audio")
                      << " write failed: " << avError(status) << '\n';
            closeLocked(false);
            scheduleReconnect();
        }
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

    void close()
    {
        std::lock_guard lock{mutex_};
        closeLocked(true);
    }

    void closeLocked(bool writeTrailer)
    {
        if (headerWritten_ && writeTrailer && rtmpFormat_ != nullptr) {
            av_write_trailer(rtmpFormat_);
        }
        headerWritten_ = false;
        videoStream_ = nullptr;
        audioStream_ = nullptr;
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
    bool videoConfigured_{false};
    bool audioConfigured_{false};
    bool headerWritten_{false};
};

class H264OutputMuxer {
public:
    H264OutputMuxer(const RepeaterConfig& config,
                    PlutoSink& output,
                    PersistentRtmpMuxer* rtmp,
                    int width,
                    int height,
                    int frameRate,
                    std::optional<std::string> streamInfo)
        : config_{config}
        , output_{output}
        , rtmp_{rtmp}
        , width_{width}
        , height_{height}
        , frameRate_{frameRate}
        , streamInfo_{std::move(streamInfo)}
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
        return codec_->pix_fmt;
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

    void encode(AVFrame* frame)
    {
        auto overlayFrame = overlay_->render(frame);
        checkAv(avcodec_send_frame(codec_, overlayFrame.get()), "send transcode frame");
        drainPackets();
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
        flushed_ = true;
    }

private:
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
        const auto videoBitrateKbps = std::max(250, static_cast<int>(config_.pluto.videoBitrateKbps));
        auto encoder = openH264Encoder(*format_, width_, height_, frameRate, videoBitrateKbps);
        codec_ = encoder.context;
        overlay_ = std::make_unique<OverlayRenderer>(liveOverlayFilter(config_, streamInfo_, frameRate),
                                                     codec_->width,
                                                     codec_->height,
                                                     codec_->pix_fmt,
                                                     frameRate);
        std::cout << "media pipeline transcoding received video to H.264 with encoder "
                  << encoder.name << " at " << width_ << "x" << height_ << '\n';
        checkAv(avcodec_parameters_from_context(stream_->codecpar, codec_), "copy live encoder parameters");
        stream_->time_base = codec_->time_base;

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
            rtmp_->configureVideo(*codec_);
        }
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

    void writeTransportPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, codec_->time_base, stream_->time_base);
        packet->duration = av_rescale_q(1, codec_->time_base, stream_->time_base);
        packet->stream_index = stream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write live MPEG-TS packet");
    }

    const RepeaterConfig& config_;
    PlutoSink& output_;
    PersistentRtmpMuxer* rtmp_{};
    int width_{};
    int height_{};
    int frameRate_{};
    std::optional<std::string> streamInfo_;
    AVFormatContext* format_{nullptr};
    AVCodecContext* codec_{nullptr};
    AVStream* stream_{nullptr};
    std::unique_ptr<OverlayRenderer> overlay_;
    std::uint8_t* avioBuffer_{nullptr};
    bool headerWritten_{false};
    bool flushed_{false};
};

class LiveTranscoder {
public:
    LiveTranscoder(RepeaterConfig config, PlutoSink& output, PersistentRtmpMuxer* rtmp, std::optional<ActiveInput> active)
        : config_{std::move(config)}
        , output_{output}
        , rtmp_{rtmp}
        , active_{std::move(active)}
        , thread_{[this] {
            run();
        }}
    {
    }

    ~LiveTranscoder()
    {
        input_.stop();
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

private:
    void run()
    {
        try {
            transcodeLoop();
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: live transcode stopped: " << ex.what() << '\n';
        }
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
        rawInput->flags |= AVFMT_FLAG_CUSTOM_IO;

        const auto* demuxer = av_find_input_format("mpegts");
        AVFormatContext* openTarget = inputFormat.get();
        checkAv(avformat_open_input(&openTarget, nullptr, demuxer, nullptr), "open received MPEG-TS");
        inputFormat.release();
        inputFormat.reset(openTarget);
        checkAv(avformat_find_stream_info(inputFormat.get(), nullptr), "probe received MPEG-TS streams");

        const auto videoStreamIndex = av_find_best_stream(inputFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        checkAv(videoStreamIndex, "find received video stream");
        auto* videoStream = inputFormat->streams[videoStreamIndex];
        const auto codecId = videoStream->codecpar->codec_id;
        const auto* decoder = findVideoDecoder(codecId);
        if (decoder == nullptr) {
            throw std::runtime_error{"no decoder available for received " + codecDescription(codecId)};
        }

        CodecContextPtr decoderContext{avcodec_alloc_context3(decoder)};
        if (!decoderContext) {
            throw std::runtime_error{"allocate live decoder failed"};
        }
        checkAv(avcodec_parameters_to_context(decoderContext.get(), videoStream->codecpar), "copy live decoder parameters");
        checkAv(avcodec_open2(decoderContext.get(), decoder, nullptr), "open live video decoder");

        const auto decodedWidth = evenDimension(decoderContext->width > 0 ? decoderContext->width : videoStream->codecpar->width);
        const auto decodedHeight = evenDimension(decoderContext->height > 0 ? decoderContext->height : videoStream->codecpar->height);
        const auto frameRate = streamFrameRate(*videoStream);
        std::cout << "media pipeline detected received " << codecDescription(codecId)
                  << " video, decoding with " << (decoder->name == nullptr ? "default" : decoder->name)
                  << " at " << decodedWidth << "x" << decodedHeight << " " << frameRate << " fps\n";

        auto streamInfo = active_.has_value()
            ? std::optional<std::string>{streamInfoText(*active_, codecDescription(codecId), decodedWidth, decodedHeight, frameRate)}
            : std::nullopt;
        H264OutputMuxer outputMuxer{config_, output_, rtmp_, fallbackWidth, fallbackHeight, fallbackFrameRate(config_), std::move(streamInfo)};
        auto frame = FramePtr{av_frame_alloc()};
        auto convertedFrame = FramePtr{av_frame_alloc()};
        auto packet = PacketPtr{av_packet_alloc()};
        if (!frame || !convertedFrame || !packet) {
            throw std::runtime_error{"allocate live transcode buffers failed"};
        }

        convertedFrame->format = outputMuxer.pixelFormat();
        convertedFrame->width = outputMuxer.width();
        convertedFrame->height = outputMuxer.height();
        checkAv(av_frame_get_buffer(convertedFrame.get(), 32), "allocate live converted frame buffer");

        SwsContextPtr scaler;
        std::int64_t frameIndex = 0;
        while (true) {
            const auto readStatus = av_read_frame(inputFormat.get(), packet.get());
            if (readStatus == AVERROR_EOF) {
                break;
            }
            checkAv(readStatus, "read received MPEG-TS packet");
            if (packet->stream_index == videoStreamIndex) {
                decodePacket(decoderContext.get(), packet.get(), frame.get(), convertedFrame.get(), scaler, outputMuxer, frameIndex);
            }
            av_packet_unref(packet.get());
        }

        checkAv(avcodec_send_packet(decoderContext.get(), nullptr), "flush live decoder");
        drainDecoder(decoderContext.get(), frame.get(), convertedFrame.get(), scaler, outputMuxer, frameIndex);
        outputMuxer.flush();
    }

    void decodePacket(AVCodecContext* decoder,
                      AVPacket* packet,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      H264OutputMuxer& outputMuxer,
                      std::int64_t& frameIndex)
    {
        const auto sendStatus = avcodec_send_packet(decoder, packet);
        if (sendStatus == AVERROR(EAGAIN)) {
            drainDecoder(decoder, frame, convertedFrame, scaler, outputMuxer, frameIndex);
            checkAv(avcodec_send_packet(decoder, packet), "send received packet after drain");
        } else {
            checkAv(sendStatus, "send received packet");
        }
        drainDecoder(decoder, frame, convertedFrame, scaler, outputMuxer, frameIndex);
    }

    void drainDecoder(AVCodecContext* decoder,
                      AVFrame* frame,
                      AVFrame* convertedFrame,
                      SwsContextPtr& scaler,
                      H264OutputMuxer& outputMuxer,
                      std::int64_t& frameIndex)
    {
        for (;;) {
            const auto receiveStatus = avcodec_receive_frame(decoder, frame);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                break;
            }
            checkAv(receiveStatus, "receive decoded live frame");
            outputMuxer.encode(convertFrame(frame, convertedFrame, scaler, outputMuxer, frameIndex++));
            av_frame_unref(frame);
        }
    }

    AVFrame* convertFrame(AVFrame* frame,
                          AVFrame* convertedFrame,
                          SwsContextPtr& scaler,
                          const H264OutputMuxer& outputMuxer,
                          std::int64_t pts)
    {
        if (frame->format == outputMuxer.pixelFormat()
            && frame->width == outputMuxer.width()
            && frame->height == outputMuxer.height()) {
            frame->pts = pts;
            return frame;
        }

        checkAv(av_frame_make_writable(convertedFrame), "make live converted frame writable");
        if (!scaler) {
            scaler.reset(sws_getContext(frame->width,
                                        frame->height,
                                        static_cast<AVPixelFormat>(frame->format),
                                        outputMuxer.width(),
                                        outputMuxer.height(),
                                        outputMuxer.pixelFormat(),
                                        SWS_BILINEAR,
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
        return convertedFrame;
    }

    RepeaterConfig config_;
    PlutoSink& output_;
    PersistentRtmpMuxer* rtmp_{};
    std::optional<ActiveInput> active_;
    BlockingTsInput input_;
    std::thread thread_;
};

class FallbackMuxer {
public:
    FallbackMuxer(const RepeaterConfig& config, PlutoSink& output, PersistentRtmpMuxer* rtmp)
        : config_{config}
        , output_{output}
        , rtmp_{rtmp}
        , frameRate_{fallbackFrameRate(config_)}
        , slideDurationFrames_{std::max<std::int64_t>(1, config_.fallback.slideDuration.count() * frameRate_ / 1000)}
        , morseUnits_{morseToneUnits(identText(config_))}
        , morseUnitSamples_{std::max<std::int64_t>(1, static_cast<std::int64_t>(fallbackAudioSampleRate * 1.2 / std::max(1U, config_.ident.morseWpm)))}
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

    void setNotice(std::optional<std::string> notice)
    {
        if (notice_ == notice) {
            return;
        }
        notice_ = std::move(notice);
        cachedSlate_.reset();
    }

    void writeFrame(std::int64_t frameIndex)
    {
        FramePtr frame;
        if (notice_.has_value()) {
            if (!cachedSlate_) {
                cachedSlate_ = renderSlateFrame(*notice_, frameRate_);
            }
            frame.reset(av_frame_clone(cachedSlate_.get()));
            if (!frame) {
                throw std::runtime_error{"clone rendered slate frame failed"};
            }
        } else {
            const auto identActive = identActiveForFrame(frameIndex);
            if (identActive) {
                if (!cachedTestcard_) {
                    cachedTestcard_ = renderTestcardFrame(config_, frameRate_);
                }
                frame.reset(av_frame_clone(cachedTestcard_.get()));
            } else {
                frame = slideFrame(frameIndex);
            }
            if (!frame) {
                throw std::runtime_error{"clone rendered fallback frame failed"};
            }
        }
        frame->pts = frameIndex;
        auto overlayFrame = identOverlay_->render(frame.get());
        overlayFrame->pts = frameIndex;
        encode(overlayFrame.get());
        writeAudioUntil(frameIndex + 1);
    }

private:
    void initialiseSlides()
    {
        activeSlidePaths_ = isDecember() ? slideFilesIn(config_.fallback.christmasSlideDirectory) : std::vector<std::filesystem::path>{};
        if (activeSlidePaths_.empty()) {
            activeSlidePaths_ = slideFilesIn(config_.fallback.slideDirectory);
        }
        if (activeSlidePaths_.empty() && !config_.fallback.stillPath.empty()) {
            activeSlidePaths_.push_back(config_.fallback.stillPath);
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
                cachedSlide_ = decodeSlideFrame(path);
            } catch (const std::exception& ex) {
                std::cerr << "wh-repeater: load slideshow image failed: " << ex.what() << '\n';
                cachedSlide_ = renderIdentFrame(config_, frameRate_);
            }
            ++slideIndex_;
            nextSlideFrame_ = frameIndex + slideDurationFrames_;
        }

        return FramePtr{av_frame_clone(cachedSlide_.get())};
    }

    void initialise()
    {
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
        auto encoder = openFallbackEncoder(*format_, frameRate_, videoBitrateKbps);
        codec_ = encoder.context;
        identOverlay_ = std::make_unique<OverlayRenderer>(identFilter(identText(config_)), codec_->width, codec_->height, codec_->pix_fmt, frameRate_);
        std::cout << "media pipeline using H.264 encoder " << encoder.name
                  << " at " << frameRate_ << " fps\n";
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
        audioCodec_->sample_rate = fallbackAudioSampleRate;
        audioCodec_->bit_rate = static_cast<std::int64_t>(std::max(32U, config_.pluto.audioBitrateKbps)) * 1000;
        audioCodec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&audioCodec_->ch_layout, 1);
        audioCodec_->time_base = AVRational{1, fallbackAudioSampleRate};
        if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audioCodec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        checkAv(avcodec_open2(audioCodec_, encoder, nullptr), "open fallback AAC encoder");
        checkAv(avcodec_parameters_from_context(audioStream_->codecpar, audioCodec_), "copy fallback audio parameters");
        audioStream_->time_base = audioCodec_->time_base;
        audioFrameSamples_ = audioCodec_->frame_size > 0 ? audioCodec_->frame_size : 1024;
    }

    void encode(AVFrame* frame)
    {
        checkAv(avcodec_send_frame(codec_, frame), "send fallback frame");
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
                (morseSamples * frameRate_ + fallbackAudioSampleRate - 1) / fallbackAudioSampleRate);
            identEndFrame_ = identStartFrame_ + morseFrames;
            nextIdentFrame_ = frameIndex + static_cast<std::int64_t>(config_.ident.interval.count()) * frameRate_;
            firstIdent_ = false;
            std::cout << "media pipeline starting Morse ident audio for " << identText(config_) << '\n';
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

    void writeAudioUntil(std::int64_t nextVideoFrame)
    {
        if (audioCodec_ == nullptr || audioStream_ == nullptr || !headerWritten_) {
            return;
        }

        const auto targetSample = nextVideoFrame * fallbackAudioSampleRate / frameRate_;
        while (audioSampleIndex_ + audioFrameSamples_ <= targetSample) {
            writeAudioFrame(audioFrameSamples_);
        }
    }

    void writeAudioFrame(int samples)
    {
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

        auto* plane = reinterpret_cast<float*>(frame->data[0]);
        for (int index = 0; index < samples; ++index) {
            const auto sample = audioSampleIndex_ + index;
            if (morseToneAtSample(sample)) {
                const auto phase = 2.0 * pi * static_cast<double>(config_.ident.morseToneHz) * static_cast<double>(sample - identStartSample_)
                    / static_cast<double>(fallbackAudioSampleRate);
                plane[index] = static_cast<float>(0.22 * std::sin(phase));
            } else {
                plane[index] = 0.0F;
            }
        }
        audioSampleIndex_ += samples;

        checkAv(avcodec_send_frame(audioCodec_, frame.get()), "send fallback audio frame");
        drainAudioPackets();
    }

    void flushAudio()
    {
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

    const RepeaterConfig& config_;
    PlutoSink& output_;
    PersistentRtmpMuxer* rtmp_{};
    int frameRate_{};
    AVFormatContext* format_{nullptr};
    AVCodecContext* codec_{nullptr};
    AVCodecContext* audioCodec_{nullptr};
    AVStream* stream_{nullptr};
    AVStream* audioStream_{nullptr};
    std::unique_ptr<OverlayRenderer> identOverlay_;
    std::uint8_t* avioBuffer_{nullptr};
    bool headerWritten_{false};
    std::optional<std::string> notice_;
    FramePtr cachedSlate_;
    FramePtr cachedIdent_;
    FramePtr cachedTestcard_;
    FramePtr cachedSlide_;
    std::vector<std::filesystem::path> activeSlidePaths_;
    std::size_t slideIndex_{0};
    std::int64_t slideDurationFrames_{1};
    std::int64_t nextSlideFrame_{0};
    std::vector<bool> morseUnits_;
    std::int64_t morseUnitSamples_{1};
    std::int64_t nextIdentFrame_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t identStartFrame_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t identEndFrame_{0};
    std::int64_t identStartSample_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t audioSampleIndex_{0};
    int audioFrameSamples_{1024};
    bool firstIdent_{true};
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
}

void MediaPipeline::setBeaconAllowed(bool allowed)
{
    std::lock_guard lock{mutex_};
    beaconAllowed_ = allowed;
}

void MediaPipeline::setAccessNotice(std::optional<std::string> notice)
{
    std::lock_guard lock{mutex_};
    accessNotice_ = std::move(notice);
    inputReady_.notify_all();
}

void MediaPipeline::tick(std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock{mutex_};
    if (active_.has_value()) {
        enterRetransmit(now);
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

void MediaPipeline::ensureLibavReady()
{
    const auto networkStatus = avformat_network_init();
    if (networkStatus < 0) {
        throw std::runtime_error{"libav network init failed: " + avError(networkStatus)};
    }

    if (findVideoDecoder(AV_CODEC_ID_MPEG2VIDEO) == nullptr
        && findVideoDecoder(AV_CODEC_ID_H264) == nullptr
        && findVideoDecoder(AV_CODEC_ID_HEVC) == nullptr) {
        throw std::runtime_error{"libav has no MPEG-2, H.264, or H.265 decoder available"};
    }

    if (avcodec_find_encoder_by_name("h264_v4l2m2m") == nullptr
        && avcodec_find_encoder_by_name("libx264") == nullptr
        && avcodec_find_encoder(AV_CODEC_ID_H264) == nullptr) {
        throw std::runtime_error{"libav has no H.264 encoder available"};
    }
    if (avcodec_find_encoder(AV_CODEC_ID_AAC) == nullptr) {
        throw std::runtime_error{"libav has no AAC encoder available"};
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
    output_.setTransmitEnabled(true);
    inputReady_.notify_all();
    std::cout << "media pipeline entering retransmit mode; queued TS will require watermark transcode\n";
}

void MediaPipeline::enterFallback(std::chrono::steady_clock::time_point now, bool transmitEnabled)
{
    (void)now;
    if (mode_ == MediaPipelineMode::fallback && transmitEnabled_ == transmitEnabled) {
        return;
    }

    inputQueue_.clear();
    mode_ = MediaPipelineMode::fallback;
    transmitEnabled_ = transmitEnabled;
    output_.setTransmitEnabled(transmitEnabled_);
    inputReady_.notify_all();
    std::cout << "media pipeline entering fallback mode";
    if (!transmitEnabled_) {
        std::cout << " with TX muted";
    }
    if (!config_.fallback.stillPath.empty()) {
        std::cout << " still=" << config_.fallback.stillPath;
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
    auto nextFrameAt = std::chrono::steady_clock::now();

    if (config_.streaming.rtmp.enabled && !config_.streaming.rtmp.url.empty()) {
        rtmpMuxer = std::make_unique<PersistentRtmpMuxer>(config_);
    }

    while (true) {
        MediaPipelineMode mode;
        std::optional<ActiveInput> active;
        std::optional<std::string> notice;
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
            beaconAllowed = beaconAllowed_;
        }

        try {
            if (mode == MediaPipelineMode::fallback) {
                liveTranscoder.reset();
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_, rtmpMuxer.get());
                    fallbackFrameIndex = 0;
                    nextFrameAt = std::chrono::steady_clock::now();
                }
                if (notice.has_value()) {
                    fallbackMuxer->setNotice(notice);
                } else if (!beaconAllowed) {
                    fallbackMuxer->setNotice(sleepingMessage(config_));
                } else {
                    fallbackMuxer->setNotice(std::nullopt);
                }
                std::this_thread::sleep_until(nextFrameAt);
                fallbackMuxer->writeFrame(fallbackFrameIndex++);
                nextFrameAt += std::chrono::milliseconds{1000 / fallbackFrameRate(config_)};
                continue;
            }

            fallbackMuxer.reset();
            if (mode == MediaPipelineMode::retransmit) {
                if (!liveTranscoder) {
                    liveTranscoder = std::make_unique<LiveTranscoder>(config_, output_, rtmpMuxer.get(), active);
                }
                std::vector<std::byte> queued;
                std::unique_lock lock{mutex_};
                inputReady_.wait_for(lock, std::chrono::milliseconds{100}, [this] {
                    return stopping_ || mode_ != MediaPipelineMode::retransmit || !inputQueue_.empty();
                });
                if (stopping_) {
                    break;
                }
                if (!inputQueue_.empty()) {
                    queued.insert(queued.end(), inputQueue_.begin(), inputQueue_.end());
                    inputQueue_.clear();
                }
                lock.unlock();
                if (!queued.empty()) {
                    liveTranscoder->append(queued);
                }
            } else {
                liveTranscoder.reset();
            }
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: media pipeline error: " << ex.what() << '\n';
            std::this_thread::sleep_for(std::chrono::seconds{1});
            fallbackMuxer.reset();
            liveTranscoder.reset();
        }
    }
    fallbackMuxer.reset();
    liveTranscoder.reset();
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
    constexpr std::size_t maxQueuedBytes = 2 * 1024 * 1024;
    if (inputQueue_.size() + packet.size() > maxQueuedBytes) {
        inputQueue_.clear();
    }
    inputQueue_.insert(inputQueue_.end(), packet.begin(), packet.end());
    inputReady_.notify_all();
}

} // namespace whrepeater
