/*
 * ============================================================================
 *  wh-repeater - Configuration Model
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Defines the JSON-backed repeater configuration structures, default
 *    values, load/save helpers, and derived Pluto mux/video bitrate
 *    calculations.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#pragma once

#include "whrepeater/types.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace whrepeater {

struct PlutoConfig {
    std::string address{"230.10.0.1"};
    std::uint16_t port{1234};
    bool mqttEnabled{true};
    std::string mqttHost{"192.168.2.1"};
    std::uint16_t mqttPort{1883};
    std::string mqttProtocol{"pluto-ori"};
    std::string mqttDeviceId;
    std::string callsign{"GB3GV"};
    std::string system{"dvbs2"};
    std::uint64_t txFrequencyHz{2400000000ULL};
    std::uint32_t symbolRateS{333000};
    int txGainDb{-40};
    int ncoHz{0};
    bool pilots{false};
    std::string frame{"long"};
    std::string fecMode{"fixed"};
    std::string constellation{"qpsk"};
    std::uint32_t muxRateKbps{1200};
    std::uint32_t videoBitrateKbps{900};
    std::uint32_t audioBitrateKbps{96};
    std::uint32_t outputAudioChannels{2};
    std::uint32_t outputWidth{1280};
    std::uint32_t outputHeight{720};
    std::uint32_t outputFrameRate{25};
    std::string h264Profile{"main"};
    std::string h264Level{"auto"};
    std::string fec{"1/2"};
    std::string watermarkText{"WH Repeater"};
};

struct FallbackConfig {
    bool enabled{true};
    std::vector<std::string> videoPaths;
    std::string videoDirectory{"/home/pi/Videos"};
    std::string slideDirectory{"/var/lib/wh-repeater/slides"};
    std::string christmasSlideDirectory{"/var/lib/wh-repeater/slides/christmas"};
    std::chrono::milliseconds slideDuration{10000};
    std::chrono::milliseconds inputTimeout{1500};
    std::uint32_t staticFrameRate{2};
    bool hardwareDecode{false};
};

struct RtmpStreamingConfig {
    bool enabled{false};
    std::string url;
};

struct StreamingConfig {
    RtmpStreamingConfig rtmp;
};

struct BeaconScheduleConfig {
    bool enabled{false};
    std::string startTime{"09:00"};
    std::string endTime{"23:00"};
};

struct IdentConfig {
    bool enabled{true};
    std::string serviceName{"WH Repeater"};
    std::chrono::seconds interval{std::chrono::minutes{10}};
    std::uint32_t morseToneHz{650};
    std::uint32_t morseWpm{10};
};

struct HardwarePttConfig {
    bool enabled{false};
    std::string chip{"/dev/gpiochip0"};
    std::uint32_t line{0};
    bool activeHigh{true};
};

struct AnalogueCaptureConfig {
    bool enabled{false};
    ReceiverId receiver{5};
    std::string deviceId{"usb-capture"};
    std::string label{"USB analogue"};
    std::string captureDevice{"/dev/video0"};
    std::string captureStandard{"pal"};
    std::uint32_t captureWidth{720};
    std::uint32_t captureHeight{576};
    std::uint32_t captureFrameRate{25};
    std::uint32_t captureFrameRateNumerator{25};
    std::uint32_t captureFrameRateDenominator{1};
    std::string lockMode{"v4l2-sync"};
    std::string gpioChip{"/dev/gpiochip0"};
    std::uint32_t gpioLine{26};
    bool gpioActiveHigh{true};
};

struct AnalogueConfig {
    AnalogueCaptureConfig capture;
};

struct MediaConfig {
    std::string backend{"ffmpeg"};
};

struct TsGatewayConfig {
    std::string address{"127.0.0.1"};
    std::uint16_t port{5000};
};

struct GatewayInputConfig {
    std::string listenAddress{"0.0.0.0"};
    std::uint16_t listenPort{5000};
    std::size_t packetSize{1316};
};

struct PiStatusConfig {
    bool enabled{true};
    std::string address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::chrono::milliseconds pollInterval{500};
};

struct ReceiverConfig {
    ReceiverId receiver;
    bool enabled{true};
    std::vector<ScanTarget> targets;
    std::chrono::milliseconds dwellTime{1500};
    std::chrono::milliseconds hangTime{5000};
};

struct RepeaterConfig {
    std::string mode{"local-transcode"};
    std::vector<ReceiverConfig> receivers;
    PlutoConfig pluto;
    FallbackConfig fallback;
    StreamingConfig streaming;
    BeaconScheduleConfig beaconSchedule;
    IdentConfig ident;
    HardwarePttConfig hardwarePtt;
    AnalogueConfig analogue;
    MediaConfig media;
    TsGatewayConfig tsGateway;
    GatewayInputConfig gatewayInput;
    PiStatusConfig piStatus;
    std::chrono::milliseconds statusInterval{500};
    double minimumMerDb{2.0};
    double minimumDNumberDb{0.0};
};

RepeaterConfig loadConfig(const std::filesystem::path& path);
void saveConfig(const std::filesystem::path& path, const RepeaterConfig& config);
RepeaterConfig configFromJson(std::string_view text);
std::string configToJson(const RepeaterConfig& config);
std::uint32_t calculatePlutoMuxRateKbps(const PlutoConfig& config);
std::uint32_t calculatePlutoVideoBitrateKbps(const PlutoConfig& config);
RepeaterConfig defaultConfig();

} // namespace whrepeater
