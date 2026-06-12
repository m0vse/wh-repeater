/*
 * ============================================================================
 *  wh-repeater - Configuration Parser and Writer
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements JSON parsing, validation, serialization, defaults, scan-
 *    target handling, and derived Pluto mux/video bitrate calculations.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

#include "whrepeater/config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace whrepeater {
namespace {

class Json {
public:
    enum class Type { null, boolean, number, string, array, object };

    Type type{Type::null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input)
        : input_{input}
    {
    }

    Json parse()
    {
        auto value = parseValue();
        skipSpace();
        if (pos_ != input_.size()) {
            throw std::runtime_error{"unexpected trailing JSON input"};
        }
        return value;
    }

private:
    Json parseValue()
    {
        skipSpace();
        if (pos_ >= input_.size()) {
            throw std::runtime_error{"unexpected end of JSON"};
        }
        switch (input_[pos_]) {
        case 'n':
            consumeLiteral("null");
            return {};
        case 't':
            consumeLiteral("true");
            {
                Json value;
                value.type = Json::Type::boolean;
                value.boolean = true;
                return value;
            }
        case 'f':
            consumeLiteral("false");
            {
                Json value;
                value.type = Json::Type::boolean;
                value.boolean = false;
                return value;
            }
        case '"':
            return parseString();
        case '[':
            return parseArray();
        case '{':
            return parseObject();
        default:
            return parseNumber();
        }
    }

    Json parseString()
    {
        expect('"');
        Json value;
        value.type = Json::Type::string;
        while (pos_ < input_.size()) {
            const auto ch = input_[pos_++];
            if (ch == '"') {
                return value;
            }
            if (ch != '\\') {
                value.string.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size()) {
                throw std::runtime_error{"unterminated JSON escape"};
            }
            const auto escaped = input_[pos_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.string.push_back(escaped);
                break;
            case 'b':
                value.string.push_back('\b');
                break;
            case 'f':
                value.string.push_back('\f');
                break;
            case 'n':
                value.string.push_back('\n');
                break;
            case 'r':
                value.string.push_back('\r');
                break;
            case 't':
                value.string.push_back('\t');
                break;
            default:
                throw std::runtime_error{"unsupported JSON escape"};
            }
        }
        throw std::runtime_error{"unterminated JSON string"};
    }

    Json parseArray()
    {
        expect('[');
        Json value;
        value.type = Json::Type::array;
        skipSpace();
        if (peek(']')) {
            ++pos_;
            return value;
        }
        for (;;) {
            value.array.push_back(parseValue());
            skipSpace();
            if (peek(']')) {
                ++pos_;
                return value;
            }
            expect(',');
        }
    }

    Json parseObject()
    {
        expect('{');
        Json value;
        value.type = Json::Type::object;
        skipSpace();
        if (peek('}')) {
            ++pos_;
            return value;
        }
        for (;;) {
            auto key = parseString();
            skipSpace();
            expect(':');
            value.object.emplace(std::move(key.string), parseValue());
            skipSpace();
            if (peek('}')) {
                ++pos_;
                return value;
            }
            expect(',');
        }
    }

    Json parseNumber()
    {
        const auto start = pos_;
        if (peek('-')) {
            ++pos_;
        }
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
        if (peek('.')) {
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                ++pos_;
            }
        }
        const auto text = std::string{input_.substr(start, pos_ - start)};
        if (text.empty() || text == "-") {
            throw std::runtime_error{"invalid JSON number"};
        }
        Json value;
        value.type = Json::Type::number;
        value.number = std::stod(text);
        return value;
    }

    void skipSpace()
    {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    bool peek(char ch) const
    {
        return pos_ < input_.size() && input_[pos_] == ch;
    }

    void expect(char ch)
    {
        skipSpace();
        if (!peek(ch)) {
            throw std::runtime_error{"unexpected JSON token"};
        }
        ++pos_;
    }

    void consumeLiteral(std::string_view literal)
    {
        if (input_.substr(pos_, literal.size()) != literal) {
            throw std::runtime_error{"invalid JSON literal"};
        }
        pos_ += literal.size();
    }

    std::string_view input_;
    std::size_t pos_{};
};

const Json& requiredMember(const Json& object, std::string_view name)
{
    if (object.type != Json::Type::object) {
        throw std::runtime_error{"expected JSON object"};
    }
    const auto it = object.object.find(std::string{name});
    if (it == object.object.end()) {
        throw std::runtime_error{"missing required field: " + std::string{name}};
    }
    return it->second;
}

const Json* optionalMember(const Json& object, std::string_view name)
{
    if (object.type != Json::Type::object) {
        return nullptr;
    }
    const auto it = object.object.find(std::string{name});
    return it == object.object.end() ? nullptr : &it->second;
}

std::uint32_t jsonUint32(const Json& value, std::string_view name)
{
    if (value.type != Json::Type::number || value.number < 0) {
        throw std::runtime_error{"expected unsigned number for " + std::string{name}};
    }
    return static_cast<std::uint32_t>(value.number);
}

int jsonInt(const Json& value, std::string_view name)
{
    if (value.type != Json::Type::number) {
        throw std::runtime_error{"expected number for " + std::string{name}};
    }
    return static_cast<int>(value.number);
}

double jsonDouble(const Json& value, std::string_view name)
{
    if (value.type != Json::Type::number) {
        throw std::runtime_error{"expected number for " + std::string{name}};
    }
    return value.number;
}

bool jsonBool(const Json& value, std::string_view name)
{
    if (value.type != Json::Type::boolean) {
        throw std::runtime_error{"expected boolean for " + std::string{name}};
    }
    return value.boolean;
}

std::string jsonText(const Json& value, std::string_view name)
{
    if (value.type != Json::Type::string) {
        throw std::runtime_error{"expected string for " + std::string{name}};
    }
    return value.string;
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

Antenna parseAntenna(std::string_view value)
{
    if (value == "top") {
        return Antenna::top;
    }
    if (value == "bottom") {
        return Antenna::bottom;
    }
    throw std::runtime_error{"antenna must be top or bottom"};
}

DvbSystem parseSystem(std::string_view value)
{
    if (value == "auto") {
        return DvbSystem::unknown;
    }
    if (value == "dvbs") {
        return DvbSystem::dvbs;
    }
    if (value == "dvbs2") {
        return DvbSystem::dvbs2;
    }
    throw std::runtime_error{"system must be auto, dvbs, or dvbs2"};
}

std::string parseFec(std::string_view value)
{
    static constexpr std::array<std::string_view, 12> fecValues{
        "auto",
        "1/4",
        "1/3",
        "2/5",
        "1/2",
        "3/5",
        "2/3",
        "3/4",
        "4/5",
        "5/6",
        "7/8",
        "8/9",
    };
    for (const auto fec : fecValues) {
        if (value == fec) {
            return std::string{value};
        }
    }
    if (value == "9/10") {
        return "9/10";
    }
    throw std::runtime_error{"FEC must be auto, 1/4, 1/3, 2/5, 1/2, 3/5, 2/3, 3/4, 4/5, 5/6, 7/8, 8/9, or 9/10"};
}

std::string parsePlutoSystem(std::string_view value)
{
    if (value == "dvbs" || value == "dvbs2") {
        return std::string{value};
    }
    throw std::runtime_error{"pluto.system must be dvbs or dvbs2"};
}

std::string parseH264Profile(std::string_view value)
{
    if (value == "baseline" || value == "main" || value == "high") {
        return std::string{value};
    }
    throw std::runtime_error{"pluto.h264Profile must be baseline, main, or high"};
}

std::string parseH264Level(std::string_view value)
{
    if (value == "auto" || value == "3" || value == "3.1" || value == "4") {
        return std::string{value};
    }
    throw std::runtime_error{"pluto.h264Level must be auto, 3, 3.1, or 4"};
}

std::string parseClockTime(std::string_view value, std::string_view name)
{
    if (value.size() != 5 || value[2] != ':'
        || !std::isdigit(static_cast<unsigned char>(value[0]))
        || !std::isdigit(static_cast<unsigned char>(value[1]))
        || !std::isdigit(static_cast<unsigned char>(value[3]))
        || !std::isdigit(static_cast<unsigned char>(value[4]))) {
        throw std::runtime_error{std::string{name} + " must be HH:MM"};
    }
    const auto hours = (value[0] - '0') * 10 + (value[1] - '0');
    const auto minutes = (value[3] - '0') * 10 + (value[4] - '0');
    if (hours > 23 || minutes > 59) {
        throw std::runtime_error{std::string{name} + " must be a valid 24-hour time"};
    }
    return std::string{value};
}

std::pair<std::uint32_t, std::uint32_t> fecFraction(std::string_view fec)
{
    const auto slash = fec.find('/');
    if (slash == std::string_view::npos) {
        throw std::runtime_error{"FEC must be a fraction"};
    }
    const auto numerator = static_cast<std::uint32_t>(std::stoul(std::string{fec.substr(0, slash)}));
    const auto denominator = static_cast<std::uint32_t>(std::stoul(std::string{fec.substr(slash + 1)}));
    if (numerator == 0 || denominator == 0 || numerator > denominator) {
        throw std::runtime_error{"FEC must be a valid fraction"};
    }
    return {numerator, denominator};
}

std::uint32_t constellationBits(std::string_view system, std::string_view constellation)
{
    if (system == "dvbs") {
        return 2;
    }
    if (constellation == "qpsk") {
        return 2;
    }
    if (constellation == "8psk") {
        return 3;
    }
    if (constellation == "16apsk") {
        return 4;
    }
    if (constellation == "32apsk") {
        return 5;
    }
    throw std::runtime_error{"unsupported Pluto constellation"};
}

std::uint64_t floorDivide(std::uint64_t numerator, std::uint64_t denominator)
{
    if (denominator == 0) {
        throw std::runtime_error{"cannot divide by zero"};
    }
    return numerator / denominator;
}

std::string antennaName(Antenna antenna)
{
    return antenna == Antenna::top ? "top" : "bottom";
}

Antenna fixedAntennaForReceiver(ReceiverId receiver)
{
    return (receiver.value == 1 || receiver.value == 3) ? Antenna::top : Antenna::bottom;
}

std::string systemName(DvbSystem system)
{
    switch (system) {
    case DvbSystem::dvbs:
        return "dvbs";
    case DvbSystem::dvbs2:
        return "dvbs2";
    case DvbSystem::unknown:
        return "auto";
    }
    return "auto";
}

ScanTarget parseTarget(const Json& value)
{
    ScanTarget target;
    target.frequencyKhz = jsonUint32(requiredMember(value, "frequencyKhz"), "frequencyKhz");
    target.symbolRateKs = jsonUint32(requiredMember(value, "symbolRateKs"), "symbolRateKs");
    target.localOscillatorKhz = jsonUint32(requiredMember(value, "localOscillatorKhz"), "localOscillatorKhz");
    if (const auto* antenna = optionalMember(value, "antenna")) {
        target.antenna = parseAntenna(jsonText(*antenna, "antenna"));
    }
    if (const auto* system = optionalMember(value, "system")) {
        target.system = parseSystem(jsonText(*system, "system"));
    }
    if (const auto* fec = optionalMember(value, "fec")) {
        target.fec = parseFec(jsonText(*fec, "fec"));
    }
    if (const auto* label = optionalMember(value, "label")) {
        target.label = jsonText(*label, "label");
    }
    return target;
}

std::string targetJson(const ScanTarget& target)
{
    std::ostringstream out;
    out << "{"
        << "\"frequencyKhz\":" << target.frequencyKhz << ","
        << "\"symbolRateKs\":" << target.symbolRateKs << ","
        << "\"localOscillatorKhz\":" << target.localOscillatorKhz << ","
        << "\"antenna\":" << jsonString(antennaName(target.antenna)) << ","
        << "\"system\":" << jsonString(systemName(target.system)) << ","
        << "\"fec\":" << jsonString(target.fec) << ","
        << "\"label\":" << jsonString(target.label)
        << "}";
    return out.str();
}

} // namespace

std::uint32_t calculatePlutoMuxRateKbps(const PlutoConfig& config)
{
    const auto [fecNumerator, fecDenominator] = fecFraction(config.fec);
    const auto bitsPerSymbol = constellationBits(config.system, config.constellation);

    std::uint64_t numerator = static_cast<std::uint64_t>(config.symbolRateS) * bitsPerSymbol * fecNumerator;
    std::uint64_t denominator = fecDenominator;
    if (config.system == "dvbs") {
        numerator *= 188;
        denominator *= 204;
    }

    const auto muxBps = floorDivide(numerator, denominator);
    return static_cast<std::uint32_t>(muxBps / 1000);
}

std::uint32_t calculatePlutoVideoBitrateKbps(const PlutoConfig& config)
{
    const auto muxRate = calculatePlutoMuxRateKbps(config);
    if (muxRate <= config.audioBitrateKbps) {
        return 1;
    }
    return muxRate - config.audioBitrateKbps;
}

RepeaterConfig defaultConfig()
{
    RepeaterConfig config;
    config.receivers = {
        ReceiverConfig{
            .receiver = ReceiverId{1},
            .targets = {
                ScanTarget{.frequencyKhz = 10491500, .symbolRateKs = 1500, .localOscillatorKhz = 9750000, .antenna = Antenna::top, .label = "QO-100 beacon"},
                ScanTarget{.frequencyKhz = 10499250, .symbolRateKs = 333, .localOscillatorKhz = 9750000, .antenna = Antenna::top, .label = "Wideband low SR"},
            },
        },
        ReceiverConfig{
            .receiver = ReceiverId{2},
            .targets = {
                ScanTarget{.frequencyKhz = 10498750, .symbolRateKs = 333, .localOscillatorKhz = 9750000, .antenna = Antenna::bottom, .label = "Wideband low SR"},
            },
        },
        ReceiverConfig{.receiver = ReceiverId{3}, .targets = {}},
        ReceiverConfig{.receiver = ReceiverId{4}, .targets = {}},
    };
    config.pluto.muxRateKbps = calculatePlutoMuxRateKbps(config.pluto);
    config.pluto.videoBitrateKbps = calculatePlutoVideoBitrateKbps(config.pluto);
    return config;
}

RepeaterConfig loadConfig(const std::filesystem::path& path)
{
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error{"cannot open config file: " + path.string()};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return configFromJson(buffer.str());
}

void saveConfig(const std::filesystem::path& path, const RepeaterConfig& config)
{
    std::ofstream output{path};
    if (!output) {
        throw std::runtime_error{"cannot write config file: " + path.string()};
    }
    output << configToJson(config);
}

RepeaterConfig configFromJson(std::string_view text)
{
    const auto root = JsonParser{text}.parse();
    RepeaterConfig config;

    if (const auto* statusIntervalMs = optionalMember(root, "statusIntervalMs")) {
        config.statusInterval = std::chrono::milliseconds{jsonInt(*statusIntervalMs, "statusIntervalMs")};
    }
    if (const auto* mode = optionalMember(root, "mode")) {
        config.mode = jsonText(*mode, "mode");
        if (config.mode != "local-transcode" && config.mode != "ts-gateway") {
            throw std::runtime_error{"mode must be local-transcode or ts-gateway"};
        }
    }

    if (const auto* selection = optionalMember(root, "selection")) {
        if (const auto* mer = optionalMember(*selection, "minimumMerDb")) {
            config.minimumMerDb = jsonDouble(*mer, "minimumMerDb");
        }
        if (const auto* dNumber = optionalMember(*selection, "minimumDNumberDb")) {
            config.minimumDNumberDb = jsonDouble(*dNumber, "minimumDNumberDb");
        }
    }

    if (const auto* pluto = optionalMember(root, "pluto")) {
        if (const auto* address = optionalMember(*pluto, "address")) {
            config.pluto.address = jsonText(*address, "pluto.address");
        }
        if (const auto* port = optionalMember(*pluto, "port")) {
            config.pluto.port = static_cast<std::uint16_t>(jsonUint32(*port, "pluto.port"));
        }
        if (const auto* mqttEnabled = optionalMember(*pluto, "mqttEnabled")) {
            config.pluto.mqttEnabled = jsonBool(*mqttEnabled, "pluto.mqttEnabled");
        }
        if (const auto* mqttHost = optionalMember(*pluto, "mqttHost")) {
            config.pluto.mqttHost = jsonText(*mqttHost, "pluto.mqttHost");
        }
        if (const auto* mqttPort = optionalMember(*pluto, "mqttPort")) {
            config.pluto.mqttPort = static_cast<std::uint16_t>(jsonUint32(*mqttPort, "pluto.mqttPort"));
        }
        if (const auto* mqttProtocol = optionalMember(*pluto, "mqttProtocol")) {
            config.pluto.mqttProtocol = jsonText(*mqttProtocol, "pluto.mqttProtocol");
            if (config.pluto.mqttProtocol != "pluto-ori" && config.pluto.mqttProtocol != "tezuka") {
                throw std::runtime_error{"pluto.mqttProtocol must be pluto-ori or tezuka"};
            }
        }
        if (const auto* mqttDeviceId = optionalMember(*pluto, "mqttDeviceId")) {
            config.pluto.mqttDeviceId = jsonText(*mqttDeviceId, "pluto.mqttDeviceId");
        }
        if (const auto* callsign = optionalMember(*pluto, "callsign")) {
            config.pluto.callsign = jsonText(*callsign, "pluto.callsign");
        }
        if (const auto* system = optionalMember(*pluto, "system")) {
            config.pluto.system = parsePlutoSystem(jsonText(*system, "pluto.system"));
        }
        if (const auto* txFrequencyHz = optionalMember(*pluto, "txFrequencyHz")) {
            config.pluto.txFrequencyHz = static_cast<std::uint64_t>(jsonUint32(*txFrequencyHz, "pluto.txFrequencyHz"));
        }
        if (const auto* symbolRateS = optionalMember(*pluto, "symbolRateS")) {
            config.pluto.symbolRateS = jsonUint32(*symbolRateS, "pluto.symbolRateS");
        }
        if (const auto* txGainDb = optionalMember(*pluto, "txGainDb")) {
            config.pluto.txGainDb = jsonInt(*txGainDb, "pluto.txGainDb");
        }
        if (const auto* ncoHz = optionalMember(*pluto, "ncoHz")) {
            config.pluto.ncoHz = jsonInt(*ncoHz, "pluto.ncoHz");
        }
        if (const auto* pilots = optionalMember(*pluto, "pilots")) {
            config.pluto.pilots = jsonBool(*pilots, "pluto.pilots");
        }
        if (const auto* frame = optionalMember(*pluto, "frame")) {
            config.pluto.frame = jsonText(*frame, "pluto.frame");
        }
        if (const auto* fecMode = optionalMember(*pluto, "fecMode")) {
            config.pluto.fecMode = jsonText(*fecMode, "pluto.fecMode");
        }
        if (const auto* constellation = optionalMember(*pluto, "constellation")) {
            config.pluto.constellation = jsonText(*constellation, "pluto.constellation");
        }
        if (const auto* muxRateKbps = optionalMember(*pluto, "muxRateKbps")) {
            config.pluto.muxRateKbps = jsonUint32(*muxRateKbps, "pluto.muxRateKbps");
        }
        if (const auto* videoBitrateKbps = optionalMember(*pluto, "videoBitrateKbps")) {
            config.pluto.videoBitrateKbps = jsonUint32(*videoBitrateKbps, "pluto.videoBitrateKbps");
        }
        if (const auto* audioBitrateKbps = optionalMember(*pluto, "audioBitrateKbps")) {
            config.pluto.audioBitrateKbps = jsonUint32(*audioBitrateKbps, "pluto.audioBitrateKbps");
        }
        if (const auto* outputWidth = optionalMember(*pluto, "outputWidth")) {
            config.pluto.outputWidth = std::clamp(jsonUint32(*outputWidth, "pluto.outputWidth"), 320U, 1920U) & ~1U;
        }
        if (const auto* outputHeight = optionalMember(*pluto, "outputHeight")) {
            config.pluto.outputHeight = std::clamp(jsonUint32(*outputHeight, "pluto.outputHeight"), 240U, 1080U) & ~1U;
        }
        if (const auto* outputFrameRate = optionalMember(*pluto, "outputFrameRate")) {
            config.pluto.outputFrameRate = std::clamp(jsonUint32(*outputFrameRate, "pluto.outputFrameRate"), 1U, 50U);
        }
        if (const auto* h264Profile = optionalMember(*pluto, "h264Profile")) {
            config.pluto.h264Profile = parseH264Profile(jsonText(*h264Profile, "pluto.h264Profile"));
        }
        if (const auto* h264Level = optionalMember(*pluto, "h264Level")) {
            config.pluto.h264Level = parseH264Level(jsonText(*h264Level, "pluto.h264Level"));
        }
        if (const auto* fec = optionalMember(*pluto, "fec")) {
            const auto parsedFec = parseFec(jsonText(*fec, "pluto.fec"));
            if (parsedFec == "auto") {
                throw std::runtime_error{"pluto.fec must be an explicit FEC"};
            }
            config.pluto.fec = parsedFec;
        }
        if (const auto* watermarkText = optionalMember(*pluto, "watermarkText")) {
            config.pluto.watermarkText = jsonText(*watermarkText, "pluto.watermarkText");
        }
        config.pluto.muxRateKbps = calculatePlutoMuxRateKbps(config.pluto);
        config.pluto.videoBitrateKbps = calculatePlutoVideoBitrateKbps(config.pluto);
    }

    if (const auto* fallback = optionalMember(root, "fallback")) {
        if (const auto* enabled = optionalMember(*fallback, "enabled")) {
            config.fallback.enabled = jsonBool(*enabled, "fallback.enabled");
        }
        if (const auto* videoDirectory = optionalMember(*fallback, "videoDirectory")) {
            config.fallback.videoDirectory = jsonText(*videoDirectory, "fallback.videoDirectory");
        }
        if (const auto* slideDirectory = optionalMember(*fallback, "slideDirectory")) {
            config.fallback.slideDirectory = jsonText(*slideDirectory, "fallback.slideDirectory");
        }
        if (const auto* christmasSlideDirectory = optionalMember(*fallback, "christmasSlideDirectory")) {
            config.fallback.christmasSlideDirectory = jsonText(*christmasSlideDirectory, "fallback.christmasSlideDirectory");
        }
        if (const auto* slideDurationSeconds = optionalMember(*fallback, "slideDurationSeconds")) {
            config.fallback.slideDuration = std::chrono::milliseconds{
                std::max(1, jsonInt(*slideDurationSeconds, "fallback.slideDurationSeconds")) * 1000};
        }
        if (const auto* inputTimeoutMs = optionalMember(*fallback, "inputTimeoutMs")) {
            config.fallback.inputTimeout = std::chrono::milliseconds{jsonInt(*inputTimeoutMs, "fallback.inputTimeoutMs")};
        }
        if (const auto* staticFrameRate = optionalMember(*fallback, "staticFrameRate")) {
            config.fallback.staticFrameRate = std::clamp(jsonUint32(*staticFrameRate, "fallback.staticFrameRate"), 1U, 25U);
        }
        if (const auto* hardwareDecode = optionalMember(*fallback, "hardwareDecode")) {
            config.fallback.hardwareDecode = jsonBool(*hardwareDecode, "fallback.hardwareDecode");
        }
        if (const auto* videoPaths = optionalMember(*fallback, "videoPaths")) {
            if (videoPaths->type != Json::Type::array) {
                throw std::runtime_error{"fallback videoPaths must be an array"};
            }
            config.fallback.videoPaths.clear();
            for (const auto& value : videoPaths->array) {
                config.fallback.videoPaths.push_back(jsonText(value, "fallback.videoPaths"));
            }
        }
    }

    if (const auto* streaming = optionalMember(root, "streaming")) {
        if (const auto* rtmp = optionalMember(*streaming, "rtmp")) {
            if (const auto* enabled = optionalMember(*rtmp, "enabled")) {
                config.streaming.rtmp.enabled = jsonBool(*enabled, "streaming.rtmp.enabled");
            }
            if (const auto* url = optionalMember(*rtmp, "url")) {
                config.streaming.rtmp.url = jsonText(*url, "streaming.rtmp.url");
            }
        }
    }

    if (const auto* media = optionalMember(root, "media")) {
        if (const auto* backend = optionalMember(*media, "backend")) {
            config.media.backend = jsonText(*backend, "media.backend");
            if (config.media.backend != "ffmpeg" && config.media.backend != "gstreamer") {
                throw std::runtime_error{"media.backend must be ffmpeg or gstreamer"};
            }
        }
    }

    if (const auto* tsGateway = optionalMember(root, "tsGateway")) {
        if (const auto* address = optionalMember(*tsGateway, "address")) {
            config.tsGateway.address = jsonText(*address, "tsGateway.address");
        }
        if (const auto* port = optionalMember(*tsGateway, "port")) {
            const auto value = jsonUint32(*port, "tsGateway.port");
            if (value == 0 || value > 65535) {
                throw std::runtime_error{"tsGateway.port must be between 1 and 65535"};
            }
            config.tsGateway.port = static_cast<std::uint16_t>(value);
        }
    }

    if (const auto* hardwarePtt = optionalMember(root, "hardwarePtt")) {
        if (const auto* enabled = optionalMember(*hardwarePtt, "enabled")) {
            config.hardwarePtt.enabled = jsonBool(*enabled, "hardwarePtt.enabled");
        }
        if (const auto* chip = optionalMember(*hardwarePtt, "chip")) {
            config.hardwarePtt.chip = jsonText(*chip, "hardwarePtt.chip");
        }
        if (const auto* line = optionalMember(*hardwarePtt, "line")) {
            config.hardwarePtt.line = jsonUint32(*line, "hardwarePtt.line");
        }
        if (const auto* activeHigh = optionalMember(*hardwarePtt, "activeHigh")) {
            config.hardwarePtt.activeHigh = jsonBool(*activeHigh, "hardwarePtt.activeHigh");
        }
    }

    if (const auto* schedule = optionalMember(root, "beaconSchedule")) {
        if (const auto* enabled = optionalMember(*schedule, "enabled")) {
            config.beaconSchedule.enabled = jsonBool(*enabled, "beaconSchedule.enabled");
        }
        if (const auto* startTime = optionalMember(*schedule, "startTime")) {
            config.beaconSchedule.startTime = parseClockTime(jsonText(*startTime, "beaconSchedule.startTime"), "beaconSchedule.startTime");
        }
        if (const auto* endTime = optionalMember(*schedule, "endTime")) {
            config.beaconSchedule.endTime = parseClockTime(jsonText(*endTime, "beaconSchedule.endTime"), "beaconSchedule.endTime");
        }
    }

    if (const auto* ident = optionalMember(root, "ident")) {
        if (const auto* enabled = optionalMember(*ident, "enabled")) {
            config.ident.enabled = jsonBool(*enabled, "ident.enabled");
        }
        if (const auto* serviceName = optionalMember(*ident, "serviceName")) {
            config.ident.serviceName = jsonText(*serviceName, "ident.serviceName");
        }
        if (const auto* intervalSeconds = optionalMember(*ident, "intervalSeconds")) {
            config.ident.interval = std::chrono::seconds{jsonInt(*intervalSeconds, "ident.intervalSeconds")};
        }
        if (const auto* morseToneHz = optionalMember(*ident, "morseToneHz")) {
            config.ident.morseToneHz = std::clamp(jsonUint32(*morseToneHz, "ident.morseToneHz"), 100U, 3000U);
        }
        if (const auto* morseWpm = optionalMember(*ident, "morseWpm")) {
            config.ident.morseWpm = std::clamp(jsonUint32(*morseWpm, "ident.morseWpm"), 5U, 40U);
        }
    }

    if (const auto* analogue = optionalMember(root, "analogue")) {
        if (const auto* capture = optionalMember(*analogue, "capture")) {
            if (const auto* enabled = optionalMember(*capture, "enabled")) {
                config.analogue.capture.enabled = jsonBool(*enabled, "analogue.capture.enabled");
            }
            if (const auto* receiverId = optionalMember(*capture, "receiverId")) {
                config.analogue.capture.receiver = ReceiverId{jsonInt(*receiverId, "analogue.capture.receiverId")};
                if (config.analogue.capture.receiver.value < 1) {
                    throw std::runtime_error{"analogue.capture.receiverId must be positive"};
                }
            }
            if (const auto* deviceId = optionalMember(*capture, "deviceId")) {
                config.analogue.capture.deviceId = jsonText(*deviceId, "analogue.capture.deviceId");
            }
            if (const auto* label = optionalMember(*capture, "label")) {
                config.analogue.capture.label = jsonText(*label, "analogue.capture.label");
            }
            if (const auto* captureDevice = optionalMember(*capture, "captureDevice")) {
                config.analogue.capture.captureDevice = jsonText(*captureDevice, "analogue.capture.captureDevice");
            }
            if (const auto* captureStandard = optionalMember(*capture, "captureStandard")) {
                config.analogue.capture.captureStandard = jsonText(*captureStandard, "analogue.capture.captureStandard");
                if (config.analogue.capture.captureStandard != "pal"
                    && config.analogue.capture.captureStandard != "ntsc"
                    && config.analogue.capture.captureStandard != "secam") {
                    throw std::runtime_error{"analogue.capture.captureStandard must be pal, ntsc, or secam"};
                }
            }
            if (const auto* captureWidth = optionalMember(*capture, "captureWidth")) {
                config.analogue.capture.captureWidth = std::clamp(jsonUint32(*captureWidth, "analogue.capture.captureWidth"), 160U, 1920U);
            }
            if (const auto* captureHeight = optionalMember(*capture, "captureHeight")) {
                config.analogue.capture.captureHeight = std::clamp(jsonUint32(*captureHeight, "analogue.capture.captureHeight"), 120U, 1080U);
            }
            if (const auto* captureFrameRate = optionalMember(*capture, "captureFrameRate")) {
                config.analogue.capture.captureFrameRate = std::clamp(jsonUint32(*captureFrameRate, "analogue.capture.captureFrameRate"), 1U, 50U);
                config.analogue.capture.captureFrameRateNumerator = config.analogue.capture.captureFrameRate;
                config.analogue.capture.captureFrameRateDenominator = 1;
            }
            if (const auto* frameRateNumerator = optionalMember(*capture, "captureFrameRateNumerator")) {
                config.analogue.capture.captureFrameRateNumerator = std::clamp(jsonUint32(*frameRateNumerator, "analogue.capture.captureFrameRateNumerator"), 1U, 60000U);
            }
            if (const auto* frameRateDenominator = optionalMember(*capture, "captureFrameRateDenominator")) {
                config.analogue.capture.captureFrameRateDenominator = std::clamp(jsonUint32(*frameRateDenominator, "analogue.capture.captureFrameRateDenominator"), 1U, 1001U);
            }
            if (const auto* lockMode = optionalMember(*capture, "lockMode")) {
                config.analogue.capture.lockMode = jsonText(*lockMode, "analogue.capture.lockMode");
                if (config.analogue.capture.lockMode != "manual"
                    && config.analogue.capture.lockMode != "device-present"
                    && config.analogue.capture.lockMode != "v4l2-sync"
                    && config.analogue.capture.lockMode != "gpio") {
                    throw std::runtime_error{"analogue.capture.lockMode must be manual, device-present, v4l2-sync, or gpio"};
                }
            }
            if (const auto* gpioChip = optionalMember(*capture, "gpioChip")) {
                config.analogue.capture.gpioChip = jsonText(*gpioChip, "analogue.capture.gpioChip");
            }
            if (const auto* gpioLine = optionalMember(*capture, "gpioLine")) {
                config.analogue.capture.gpioLine = jsonUint32(*gpioLine, "analogue.capture.gpioLine");
            }
            if (const auto* gpioActiveHigh = optionalMember(*capture, "gpioActiveHigh")) {
                config.analogue.capture.gpioActiveHigh = jsonBool(*gpioActiveHigh, "analogue.capture.gpioActiveHigh");
            }
        }
        if (const auto* sd1 = optionalMember(*analogue, "sd1")) {
            if (const auto* enabled = optionalMember(*sd1, "enabled")) {
                config.analogue.sd1.enabled = jsonBool(*enabled, "analogue.sd1.enabled");
            }
            if (const auto* receiverId = optionalMember(*sd1, "receiverId")) {
                config.analogue.sd1.receiver = ReceiverId{jsonInt(*receiverId, "analogue.sd1.receiverId")};
                if (config.analogue.sd1.receiver.value < 1) {
                    throw std::runtime_error{"analogue.sd1.receiverId must be positive"};
                }
            }
            if (const auto* deviceId = optionalMember(*sd1, "deviceId")) {
                config.analogue.sd1.deviceId = jsonText(*deviceId, "analogue.sd1.deviceId");
            }
            if (const auto* i2cDevice = optionalMember(*sd1, "i2cDevice")) {
                config.analogue.sd1.i2cDevice = jsonText(*i2cDevice, "analogue.sd1.i2cDevice");
            }
            if (const auto* i2cAddress = optionalMember(*sd1, "i2cAddress")) {
                const auto address = jsonUint32(*i2cAddress, "analogue.sd1.i2cAddress");
                if (address > 0x7f) {
                    throw std::runtime_error{"analogue.sd1.i2cAddress must be a 7-bit I2C address"};
                }
                config.analogue.sd1.i2cAddress = static_cast<std::uint8_t>(address);
            }
            if (const auto* source = optionalMember(*sd1, "source")) {
                config.analogue.sd1.source = jsonText(*source, "analogue.sd1.source");
            }
            if (const auto* captureDevice = optionalMember(*sd1, "captureDevice")) {
                config.analogue.sd1.captureDevice = jsonText(*captureDevice, "analogue.sd1.captureDevice");
            }
            if (const auto* captureWidth = optionalMember(*sd1, "captureWidth")) {
                config.analogue.sd1.captureWidth = std::clamp(jsonUint32(*captureWidth, "analogue.sd1.captureWidth"), 160U, 1920U);
            }
            if (const auto* captureHeight = optionalMember(*sd1, "captureHeight")) {
                config.analogue.sd1.captureHeight = std::clamp(jsonUint32(*captureHeight, "analogue.sd1.captureHeight"), 120U, 1080U);
            }
            if (const auto* captureFrameRate = optionalMember(*sd1, "captureFrameRate")) {
                config.analogue.sd1.captureFrameRate = std::clamp(jsonUint32(*captureFrameRate, "analogue.sd1.captureFrameRate"), 1U, 50U);
            }
        }
    }

    const auto& receivers = requiredMember(root, "receivers");
    if (receivers.type != Json::Type::array) {
        throw std::runtime_error{"receivers must be an array"};
    }

    for (const auto& receiverValue : receivers.array) {
        ReceiverConfig receiver;
        receiver.receiver = ReceiverId{jsonInt(requiredMember(receiverValue, "id"), "receiver.id")};
        if (receiver.receiver.value < 1 || receiver.receiver.value > 4) {
            throw std::runtime_error{"receiver id must be 1..4"};
        }
        if (const auto* enabled = optionalMember(receiverValue, "enabled")) {
            receiver.enabled = jsonBool(*enabled, "receiver.enabled");
        }
        if (const auto* dwellMs = optionalMember(receiverValue, "dwellMs")) {
            receiver.dwellTime = std::chrono::milliseconds{jsonInt(*dwellMs, "receiver.dwellMs")};
        }
        if (const auto* hangMs = optionalMember(receiverValue, "hangMs")) {
            receiver.hangTime = std::chrono::milliseconds{jsonInt(*hangMs, "receiver.hangMs")};
        }
        const auto& targets = requiredMember(receiverValue, "targets");
        if (targets.type != Json::Type::array) {
            throw std::runtime_error{"receiver targets must be an array"};
        }
        for (const auto& targetValue : targets.array) {
            auto target = parseTarget(targetValue);
            target.antenna = fixedAntennaForReceiver(receiver.receiver);
            receiver.targets.push_back(std::move(target));
        }
        config.receivers.push_back(std::move(receiver));
    }

    return config;
}

std::string configToJson(const RepeaterConfig& config)
{
    std::ostringstream out;
    out << "{\n"
        << "  \"mode\": " << jsonString(config.mode) << ",\n"
        << "  \"statusIntervalMs\": " << config.statusInterval.count() << ",\n"
        << "  \"selection\": {\n"
        << "    \"minimumMerDb\": " << config.minimumMerDb << ",\n"
        << "    \"minimumDNumberDb\": " << config.minimumDNumberDb << "\n"
        << "  },\n"
        << "  \"pluto\": {\n"
        << "    \"address\": " << jsonString(config.pluto.address) << ",\n"
        << "    \"port\": " << config.pluto.port << ",\n"
        << "    \"mqttEnabled\": " << (config.pluto.mqttEnabled ? "true" : "false") << ",\n"
        << "    \"mqttHost\": " << jsonString(config.pluto.mqttHost) << ",\n"
        << "    \"mqttPort\": " << config.pluto.mqttPort << ",\n"
        << "    \"mqttProtocol\": " << jsonString(config.pluto.mqttProtocol) << ",\n"
        << "    \"mqttDeviceId\": " << jsonString(config.pluto.mqttDeviceId) << ",\n"
        << "    \"callsign\": " << jsonString(config.pluto.callsign) << ",\n"
        << "    \"system\": " << jsonString(config.pluto.system) << ",\n"
        << "    \"txFrequencyHz\": " << config.pluto.txFrequencyHz << ",\n"
        << "    \"symbolRateS\": " << config.pluto.symbolRateS << ",\n"
        << "    \"txGainDb\": " << config.pluto.txGainDb << ",\n"
        << "    \"ncoHz\": " << config.pluto.ncoHz << ",\n"
        << "    \"pilots\": " << (config.pluto.pilots ? "true" : "false") << ",\n"
        << "    \"frame\": " << jsonString(config.pluto.frame) << ",\n"
        << "    \"fecMode\": " << jsonString(config.pluto.fecMode) << ",\n"
        << "    \"constellation\": " << jsonString(config.pluto.constellation) << ",\n"
        << "    \"muxRateKbps\": " << calculatePlutoMuxRateKbps(config.pluto) << ",\n"
        << "    \"videoBitrateKbps\": " << calculatePlutoVideoBitrateKbps(config.pluto) << ",\n"
        << "    \"audioBitrateKbps\": " << config.pluto.audioBitrateKbps << ",\n"
        << "    \"outputWidth\": " << config.pluto.outputWidth << ",\n"
        << "    \"outputHeight\": " << config.pluto.outputHeight << ",\n"
        << "    \"outputFrameRate\": " << config.pluto.outputFrameRate << ",\n"
        << "    \"h264Profile\": " << jsonString(config.pluto.h264Profile) << ",\n"
        << "    \"h264Level\": " << jsonString(config.pluto.h264Level) << ",\n"
        << "    \"fec\": " << jsonString(config.pluto.fec) << ",\n"
        << "    \"watermarkText\": " << jsonString(config.pluto.watermarkText) << "\n"
        << "  },\n"
        << "  \"fallback\": {\n"
        << "    \"enabled\": " << (config.fallback.enabled ? "true" : "false") << ",\n"
        << "    \"videoDirectory\": " << jsonString(config.fallback.videoDirectory) << ",\n"
        << "    \"slideDirectory\": " << jsonString(config.fallback.slideDirectory) << ",\n"
        << "    \"christmasSlideDirectory\": " << jsonString(config.fallback.christmasSlideDirectory) << ",\n"
        << "    \"slideDurationSeconds\": " << (config.fallback.slideDuration.count() / 1000) << ",\n"
        << "    \"inputTimeoutMs\": " << config.fallback.inputTimeout.count() << ",\n"
        << "    \"staticFrameRate\": " << config.fallback.staticFrameRate << ",\n"
        << "    \"hardwareDecode\": " << (config.fallback.hardwareDecode ? "true" : "false") << ",\n"
        << "    \"videoPaths\": [";
    for (std::size_t index = 0; index < config.fallback.videoPaths.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << jsonString(config.fallback.videoPaths[index]);
    }
    out << "]\n"
        << "  },\n"
        << "  \"streaming\": {\n"
        << "    \"rtmp\": {\n"
        << "      \"enabled\": " << (config.streaming.rtmp.enabled ? "true" : "false") << ",\n"
        << "      \"url\": " << jsonString(config.streaming.rtmp.url) << "\n"
        << "    }\n"
        << "  },\n"
        << "  \"media\": {\n"
        << "    \"backend\": " << jsonString(config.media.backend) << "\n"
        << "  },\n"
        << "  \"tsGateway\": {\n"
        << "    \"address\": " << jsonString(config.tsGateway.address) << ",\n"
        << "    \"port\": " << config.tsGateway.port << "\n"
        << "  },\n"
        << "  \"hardwarePtt\": {\n"
        << "    \"enabled\": " << (config.hardwarePtt.enabled ? "true" : "false") << ",\n"
        << "    \"chip\": " << jsonString(config.hardwarePtt.chip) << ",\n"
        << "    \"line\": " << config.hardwarePtt.line << ",\n"
        << "    \"activeHigh\": " << (config.hardwarePtt.activeHigh ? "true" : "false") << "\n"
        << "  },\n"
        << "  \"beaconSchedule\": {\n"
        << "    \"enabled\": " << (config.beaconSchedule.enabled ? "true" : "false") << ",\n"
        << "    \"startTime\": " << jsonString(config.beaconSchedule.startTime) << ",\n"
        << "    \"endTime\": " << jsonString(config.beaconSchedule.endTime) << "\n"
        << "  },\n"
        << "  \"ident\": {\n"
        << "    \"enabled\": " << (config.ident.enabled ? "true" : "false") << ",\n"
        << "    \"serviceName\": " << jsonString(config.ident.serviceName) << ",\n"
        << "    \"intervalSeconds\": " << config.ident.interval.count() << ",\n"
        << "    \"morseToneHz\": " << config.ident.morseToneHz << ",\n"
        << "    \"morseWpm\": " << config.ident.morseWpm << "\n"
        << "  },\n"
        << "  \"analogue\": {\n"
        << "    \"capture\": {\n"
        << "      \"enabled\": " << (config.analogue.capture.enabled ? "true" : "false") << ",\n"
        << "      \"receiverId\": " << config.analogue.capture.receiver.value << ",\n"
        << "      \"deviceId\": " << jsonString(config.analogue.capture.deviceId) << ",\n"
        << "      \"label\": " << jsonString(config.analogue.capture.label) << ",\n"
        << "      \"captureDevice\": " << jsonString(config.analogue.capture.captureDevice) << ",\n"
        << "      \"captureStandard\": " << jsonString(config.analogue.capture.captureStandard) << ",\n"
        << "      \"captureWidth\": " << config.analogue.capture.captureWidth << ",\n"
        << "      \"captureHeight\": " << config.analogue.capture.captureHeight << ",\n"
        << "      \"captureFrameRate\": " << config.analogue.capture.captureFrameRate << ",\n"
        << "      \"captureFrameRateNumerator\": " << config.analogue.capture.captureFrameRateNumerator << ",\n"
        << "      \"captureFrameRateDenominator\": " << config.analogue.capture.captureFrameRateDenominator << ",\n"
        << "      \"lockMode\": " << jsonString(config.analogue.capture.lockMode) << ",\n"
        << "      \"gpioChip\": " << jsonString(config.analogue.capture.gpioChip) << ",\n"
        << "      \"gpioLine\": " << config.analogue.capture.gpioLine << ",\n"
        << "      \"gpioActiveHigh\": " << (config.analogue.capture.gpioActiveHigh ? "true" : "false") << "\n"
        << "    },\n"
        << "    \"sd1\": {\n"
        << "      \"enabled\": " << (config.analogue.sd1.enabled ? "true" : "false") << ",\n"
        << "      \"receiverId\": " << config.analogue.sd1.receiver.value << ",\n"
        << "      \"deviceId\": " << jsonString(config.analogue.sd1.deviceId) << ",\n"
        << "      \"i2cDevice\": " << jsonString(config.analogue.sd1.i2cDevice) << ",\n"
        << "      \"i2cAddress\": " << static_cast<int>(config.analogue.sd1.i2cAddress) << ",\n"
        << "      \"source\": " << jsonString(config.analogue.sd1.source) << ",\n"
        << "      \"captureDevice\": " << jsonString(config.analogue.sd1.captureDevice) << ",\n"
        << "      \"captureWidth\": " << config.analogue.sd1.captureWidth << ",\n"
        << "      \"captureHeight\": " << config.analogue.sd1.captureHeight << ",\n"
        << "      \"captureFrameRate\": " << config.analogue.sd1.captureFrameRate << "\n"
        << "    }\n"
        << "  },\n"
        << "  \"receivers\": [\n";

    for (std::size_t receiverIndex = 0; receiverIndex < config.receivers.size(); ++receiverIndex) {
        const auto& receiver = config.receivers[receiverIndex];
        out << "    {\n"
            << "      \"id\": " << receiver.receiver.value << ",\n"
            << "      \"enabled\": " << (receiver.enabled ? "true" : "false") << ",\n"
            << "      \"dwellMs\": " << receiver.dwellTime.count() << ",\n"
            << "      \"hangMs\": " << receiver.hangTime.count() << ",\n"
            << "      \"targets\": [";
        if (!receiver.targets.empty()) {
            out << "\n";
        }
        for (std::size_t targetIndex = 0; targetIndex < receiver.targets.size(); ++targetIndex) {
            out << "        " << targetJson(receiver.targets[targetIndex]);
            if (targetIndex + 1 != receiver.targets.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << (receiver.targets.empty() ? "" : "      ")
            << "]\n"
            << "    }";
        if (receiverIndex + 1 != config.receivers.size()) {
            out << ",";
        }
        out << "\n";
    }

    out << "  ]\n"
        << "}\n";
    return out.str();
}

} // namespace whrepeater
