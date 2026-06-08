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
#include <cctype>
#include <cstdint>
#include <iostream>
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
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

namespace whrepeater {
namespace {

constexpr int fallbackWidth{1280};
constexpr int fallbackHeight{720};
constexpr std::string_view slateFont{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};

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

const std::array<std::uint8_t, 7> glyph(char ch)
{
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
    case 'A': return {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    case 'B': return {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
    case 'C': return {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
    case 'D': return {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
    case 'E': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    case 'F': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
    case 'G': return {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e};
    case 'H': return {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    case 'I': return {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
    case 'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    case 'M': return {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    case 'P': return {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
    case 'Q': return {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d};
    case 'R': return {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    case 'S': return {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    case 'T': return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};
    case 'X': return {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f};
    case '0': return {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
    case '1': return {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
    case '2': return {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
    case '3': return {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
    case '4': return {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
    case '5': return {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e};
    case '6': return {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e};
    case '7': return {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
    case '9': return {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e};
    case '-': return {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
    case '/': return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    case ':': return {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00};
    case '(': return {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    case ')': return {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
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

void drawBlock(AVFrame& frame, int x, int y, int scale, std::uint8_t level)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
        return;
    }
    const auto blockWidth = std::min(scale, frame.width - x);
    const auto blockHeight = std::min(scale, frame.height - y);
    for (int row = 0; row < blockHeight; ++row) {
        std::fill_n(frame.data[0] + (y + row) * frame.linesize[0] + x, blockWidth, level);
    }

    const auto chromaX = x / 2;
    const auto chromaY = y / 2;
    const auto chromaWidth = std::max(1, (blockWidth + 1) / 2);
    const auto chromaHeight = std::max(1, (blockHeight + 1) / 2);
    if (chromaX >= frame.width / 2 || chromaY >= frame.height / 2) {
        return;
    }
    const auto clippedChromaWidth = std::min(chromaWidth, frame.width / 2 - chromaX);
    const auto clippedChromaHeight = std::min(chromaHeight, frame.height / 2 - chromaY);
    for (int row = 0; row < clippedChromaHeight; ++row) {
        std::fill_n(frame.data[1] + (chromaY + row) * frame.linesize[1] + chromaX, clippedChromaWidth, 128);
        std::fill_n(frame.data[2] + (chromaY + row) * frame.linesize[2] + chromaX, clippedChromaWidth, 128);
    }
}

void drawText(AVFrame& frame, std::string_view text, int x, int y, int scale)
{
    const auto charWidth = 6 * scale;
    for (char ch : text) {
        const auto bits = glyph(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((bits[row] & (1 << (4 - col))) == 0) {
                    continue;
                }
                drawBlock(frame, x + col * scale + 1, y + row * scale + 1, scale + 1, 235);
            }
        }
        x += charWidth;
        if (x + charWidth >= frame.width) {
            break;
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

FramePtr renderSlateFrame(std::string_view text, int frameRate)
{
    auto inputFrame = allocateVideoFrame();
    fillBlue(*inputFrame);

    AVFilterGraph* rawGraph = avfilter_graph_alloc();
    if (rawGraph == nullptr) {
        throw std::runtime_error{"allocate slate filter graph failed"};
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
                                          "slate_source",
                                          sourceArgs.c_str(),
                                          nullptr,
                                          graph.get()),
            "create slate buffer source");
    checkAv(avfilter_graph_create_filter(&sink,
                                          avfilter_get_by_name("buffersink"),
                                          "slate_sink",
                                          nullptr,
                                          nullptr,
                                          graph.get()),
            "create slate buffer sink");

    AVFilterInOut* rawInputs = avfilter_inout_alloc();
    AVFilterInOut* rawOutputs = avfilter_inout_alloc();
    if (rawInputs == nullptr || rawOutputs == nullptr) {
        avfilter_inout_free(&rawInputs);
        avfilter_inout_free(&rawOutputs);
        throw std::runtime_error{"allocate slate filter graph endpoints failed"};
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

    auto filter = slateFilter(text);
    AVFilterInOut* inputPtr = inputs.release();
    AVFilterInOut* outputPtr = outputs.release();
    const auto parseStatus = avfilter_graph_parse_ptr(graph.get(), filter.c_str(), &inputPtr, &outputPtr, nullptr);
    avfilter_inout_free(&inputPtr);
    avfilter_inout_free(&outputPtr);
    checkAv(parseStatus, "parse slate drawtext filter");
    checkAv(avfilter_graph_config(graph.get(), nullptr), "configure slate drawtext filter");
    checkAv(av_buffersrc_add_frame_flags(source, inputFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF), "send slate frame to filter");

    FramePtr outputFrame{av_frame_alloc()};
    if (!outputFrame) {
        throw std::runtime_error{"allocate rendered slate frame failed"};
    }
    checkAv(av_buffersink_get_frame(sink, outputFrame.get()), "receive rendered slate frame");
    return outputFrame;
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

void configureEncoderContext(AVCodecContext& codec, const AVFormatContext& format, int frameRate, int videoBitrateKbps)
{
    codec.codec_type = AVMEDIA_TYPE_VIDEO;
    codec.width = fallbackWidth;
    codec.height = fallbackHeight;
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

EncoderOpenResult openFallbackEncoder(const AVFormatContext& format, int frameRate, int videoBitrateKbps)
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
            throw std::runtime_error{"allocate fallback encoder failed"};
        }
        configureEncoderContext(*codec, format, frameRate, videoBitrateKbps);

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
            throw std::runtime_error{"allocate fallback encoder failed"};
        }
        configureEncoderContext(*codec, format, frameRate, videoBitrateKbps);
        const auto status = avcodec_open2(codec, encoder, nullptr);
        if (status >= 0) {
            return EncoderOpenResult{codec, encoder, encoder->name == nullptr ? "h264" : encoder->name};
        }
        errors += std::string{"h264: "} + avError(status) + "; ";
        avcodec_free_context(&codec);
    }

    throw std::runtime_error{"open fallback H.264 encoder failed: " + errors};
}

class FallbackMuxer {
public:
    FallbackMuxer(const RepeaterConfig& config, PlutoSink& output)
        : config_{config}
        , output_{output}
        , frameRate_{fallbackFrameRate(config_)}
    {
        initialise();
    }

    ~FallbackMuxer()
    {
        if (rtmpHeaderWritten_) {
            av_write_trailer(rtmpFormat_);
        }
        if (headerWritten_) {
            av_write_trailer(format_);
        }
        if (rtmpFormat_) {
            if ((rtmpFormat_->oformat->flags & AVFMT_NOFILE) == 0 && rtmpFormat_->pb != nullptr) {
                avio_closep(&rtmpFormat_->pb);
            }
            avformat_free_context(rtmpFormat_);
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
            frame = allocateVideoFrame();
            fillBlack(*frame);
            drawText(*frame, config_.pluto.watermarkText.empty() ? "WH REPEATER" : config_.pluto.watermarkText, 36, 36, 5);
            drawText(*frame, "NO INPUT", 36, 104, 4);
        }
        frame->pts = frameIndex;
        encode(frame.get());
    }

private:
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
        std::cout << "media pipeline using H.264 encoder " << encoder.name
                  << " at " << frameRate_ << " fps\n";
        checkAv(avcodec_parameters_from_context(stream_->codecpar, codec_), "copy fallback encoder parameters");
        stream_->time_base = codec_->time_base;

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

        initialiseRtmp();
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
            writeRtmpPacket(packet.get());
            writeTransportPacket(packet.get());
            av_packet_unref(packet.get());
        }
    }

    void initialiseRtmp()
    {
        if (!config_.streaming.rtmp.enabled) {
            return;
        }
        if (config_.streaming.rtmp.url.empty()) {
            std::cerr << "wh-repeater: RTMP streaming enabled but URL is empty\n";
            return;
        }

        AVFormatContext* rawFormat{};
        const auto allocStatus = avformat_alloc_output_context2(&rawFormat, nullptr, "flv", config_.streaming.rtmp.url.c_str());
        if (allocStatus < 0 || rawFormat == nullptr) {
            std::cerr << "wh-repeater: allocate RTMP output failed: " << avError(allocStatus) << '\n';
            return;
        }
        rtmpFormat_ = rawFormat;

        rtmpStream_ = avformat_new_stream(rtmpFormat_, nullptr);
        if (rtmpStream_ == nullptr) {
            std::cerr << "wh-repeater: create RTMP video stream failed\n";
            avformat_free_context(rtmpFormat_);
            rtmpFormat_ = nullptr;
            return;
        }
        if (avcodec_parameters_from_context(rtmpStream_->codecpar, codec_) < 0) {
            std::cerr << "wh-repeater: copy RTMP encoder parameters failed\n";
            avformat_free_context(rtmpFormat_);
            rtmpFormat_ = nullptr;
            rtmpStream_ = nullptr;
            return;
        }
        rtmpStream_->time_base = codec_->time_base;

        if ((rtmpFormat_->oformat->flags & AVFMT_NOFILE) == 0) {
            const auto openStatus = avio_open2(&rtmpFormat_->pb, config_.streaming.rtmp.url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (openStatus < 0) {
                std::cerr << "wh-repeater: RTMP connect failed: " << avError(openStatus) << '\n';
                avformat_free_context(rtmpFormat_);
                rtmpFormat_ = nullptr;
                rtmpStream_ = nullptr;
                return;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
        const auto headerStatus = avformat_write_header(rtmpFormat_, &options);
        av_dict_free(&options);
        if (headerStatus < 0) {
            std::cerr << "wh-repeater: write RTMP header failed: " << avError(headerStatus) << '\n';
            if ((rtmpFormat_->oformat->flags & AVFMT_NOFILE) == 0 && rtmpFormat_->pb != nullptr) {
                avio_closep(&rtmpFormat_->pb);
            }
            avformat_free_context(rtmpFormat_);
            rtmpFormat_ = nullptr;
            rtmpStream_ = nullptr;
            return;
        }

        rtmpHeaderWritten_ = true;
        std::cout << "media pipeline streaming RTMP to " << config_.streaming.rtmp.url << '\n';
    }

    void writeRtmpPacket(const AVPacket* packet)
    {
        if (!rtmpHeaderWritten_ || rtmpFormat_ == nullptr || rtmpStream_ == nullptr) {
            return;
        }

        PacketPtr copy{av_packet_clone(packet)};
        if (!copy) {
            std::cerr << "wh-repeater: clone RTMP packet failed\n";
            return;
        }
        av_packet_rescale_ts(copy.get(), codec_->time_base, rtmpStream_->time_base);
        copy->duration = av_rescale_q(1, codec_->time_base, rtmpStream_->time_base);
        copy->stream_index = rtmpStream_->index;
        const auto status = av_interleaved_write_frame(rtmpFormat_, copy.get());
        if (status < 0) {
            std::cerr << "wh-repeater: RTMP write failed: " << avError(status) << '\n';
            rtmpHeaderWritten_ = false;
        }
    }

    void writeTransportPacket(AVPacket* packet)
    {
        av_packet_rescale_ts(packet, codec_->time_base, stream_->time_base);
        packet->duration = av_rescale_q(1, codec_->time_base, stream_->time_base);
        packet->stream_index = stream_->index;
        checkAv(av_interleaved_write_frame(format_, packet), "write fallback MPEG-TS packet");
    }

    const RepeaterConfig& config_;
    PlutoSink& output_;
    int frameRate_{};
    AVFormatContext* format_{nullptr};
    AVCodecContext* codec_{nullptr};
    AVStream* stream_{nullptr};
    AVFormatContext* rtmpFormat_{nullptr};
    AVStream* rtmpStream_{nullptr};
    std::uint8_t* avioBuffer_{nullptr};
    bool headerWritten_{false};
    bool rtmpHeaderWritten_{false};
    std::optional<std::string> notice_;
    FramePtr cachedSlate_;
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

    if (avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) == nullptr && avcodec_find_decoder(AV_CODEC_ID_H264) == nullptr) {
        throw std::runtime_error{"libav has no MPEG-2 or H.264 decoder available"};
    }

    if (avcodec_find_encoder_by_name("h264_v4l2m2m") == nullptr
        && avcodec_find_encoder_by_name("libx264") == nullptr
        && avcodec_find_encoder(AV_CODEC_ID_H264) == nullptr) {
        throw std::runtime_error{"libav has no H.264 encoder available"};
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
    std::unique_ptr<FallbackMuxer> fallbackMuxer;
    auto nextFrameAt = std::chrono::steady_clock::now();

    while (true) {
        MediaPipelineMode mode;
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
            notice = accessNotice_;
            beaconAllowed = beaconAllowed_;
        }

        try {
            if (mode == MediaPipelineMode::fallback) {
                if (!fallbackMuxer) {
                    fallbackMuxer = std::make_unique<FallbackMuxer>(config_, output_);
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
                std::unique_lock lock{mutex_};
                inputReady_.wait_for(lock, std::chrono::milliseconds{100}, [this] {
                    return stopping_ || mode_ != MediaPipelineMode::retransmit || !inputQueue_.empty();
                });
                if (stopping_) {
                    break;
                }
                if (!inputQueue_.empty()) {
                    // The queued input is deliberately retained for the next step:
                    // decode/filter/encode of received TS. Do not forward raw TS.
                    inputQueue_.clear();
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "wh-repeater: media pipeline error: " << ex.what() << '\n';
            std::this_thread::sleep_for(std::chrono::seconds{1});
            fallbackMuxer.reset();
        }
    }
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
