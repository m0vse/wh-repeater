#include "whrepeater/config.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace whrepeater;

struct TestCase {
    std::string_view name;
    void (*run)();
};

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Exception = std::runtime_error, typename Callable>
void expectThrows(Callable&& callable, std::string_view message)
{
    try {
        callable();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{std::string{message}};
}

std::string_view gatewayConfigJson()
{
    return R"json({
      "mode": "pc-gateway",
      "statusIntervalMs": 250,
      "selection": {
        "minimumMerDb": 3.5,
        "minimumDNumberDb": 1.25
      },
      "pluto": {
        "address": "230.10.0.1",
        "port": 1234,
        "mqttEnabled": false,
        "mqttHost": "10.10.30.2",
        "mqttPort": 1883,
        "mqttProtocol": "pluto-ori",
        "mqttDeviceId": "pluto-a",
        "callsign": "GB3GV",
        "system": "dvbs",
        "txFrequencyHz": 13185000,
        "symbolRateS": 4000000,
        "txGainDb": -40,
        "ncoHz": 0,
        "digitalGainDb": -3,
        "firFilter": true,
        "pilots": false,
        "frame": "long",
        "fecMode": "fixed",
        "constellation": "qpsk",
        "audioBitrateKbps": 96,
        "outputAudioChannels": 2,
        "outputWidth": 1280,
        "outputHeight": 720,
        "outputFrameRate": 25,
        "h264Profile": "main",
        "h264Level": "auto",
        "fec": "1/2",
        "watermarkText": "GB3GV"
      },
      "fallback": {
        "enabled": true,
        "videoDirectory": "/var/lib/wh-repeater/videos",
        "slideDirectory": "/var/lib/wh-repeater/slides",
        "christmasSlideDirectory": "/var/lib/wh-repeater/slides/christmas",
        "slideDurationSeconds": 8,
        "inputTimeoutMs": 1250,
        "staticFrameRate": 2,
        "hardwareDecode": true,
        "videoPaths": ["/var/lib/wh-repeater/videos/fallback.mp4"]
      },
      "streaming": {
        "rtmp": {
          "enabled": true,
          "url": "rtmp://example.invalid/live/key"
        }
      },
      "media": {
        "backend": "ffmpeg"
      },
      "tsGateway": {
        "address": "10.10.20.1",
        "port": 5000
      },
      "gatewayInput": {
        "listenAddress": "0.0.0.0",
        "listenPort": 5000,
        "packetSize": 1316
      },
      "piStatus": {
        "enabled": true,
        "address": "10.10.20.2",
        "port": 8080,
        "pollIntervalMs": 75
      },
      "hardwarePtt": {
        "enabled": false,
        "mode": "pi-gpio",
        "chip": "/dev/gpiochip0",
        "line": 13,
        "activeHigh": true
      },
      "beaconSchedule": {
        "enabled": true,
        "startTime": "08:00",
        "endTime": "23:00"
      },
      "ident": {
        "enabled": true,
        "serviceName": "GB3GV",
        "intervalSeconds": 600,
        "morseToneHz": 650,
        "morseWpm": 10
      },
      "analogue": {
        "capture": {
          "enabled": false,
          "receiverId": 5,
          "deviceId": "usb-capture",
          "label": "USB analogue",
          "captureDevice": "/dev/video0",
          "captureStandard": "pal",
          "captureInputFormat": "mjpeg",
          "captureWidth": 720,
          "captureHeight": 576,
          "captureFrameRate": 25,
          "captureFrameRateNumerator": 30000,
          "captureFrameRateDenominator": 1001,
          "audioEnabled": true,
          "audioDevice": "hw:1,0",
          "audioSampleRate": 48000,
          "audioChannels": 2,
          "audioDelayMs": 120,
          "lockMode": "pi-gpio",
          "gpioChip": "/dev/gpiochip0",
          "gpioLine": 26,
          "gpioActiveHigh": true
        }
      },
      "receivers": [
        {
          "id": 1,
          "enabled": true,
          "dwellMs": 1500,
          "hangMs": 5000,
          "targets": [
            {
              "frequencyKhz": 1249000,
              "symbolRateKs": 4000,
              "localOscillatorKhz": 0,
              "antenna": "bottom",
              "system": "auto",
              "fec": "auto",
              "label": "23cm 4MS"
            }
          ]
        },
        {
          "id": 2,
          "enabled": true,
          "dwellMs": 1500,
          "hangMs": 5000,
          "targets": [
            {
              "frequencyKhz": 437000,
              "symbolRateKs": 2000,
              "localOscillatorKhz": 0,
              "antenna": "top",
              "system": "auto",
              "fec": "auto",
              "label": "70cm 2MS"
            }
          ]
        },
        {"id": 3, "enabled": true, "dwellMs": 1500, "hangMs": 5000, "targets": []},
        {"id": 4, "enabled": true, "dwellMs": 1500, "hangMs": 5000, "targets": []}
      ]
    })json";
}

void parsesGatewayAndAnalogueConfig()
{
    const auto config = configFromJson(gatewayConfigJson());

    expect(config.mode == "pc-gateway", "mode should parse");
    expect(config.statusInterval == std::chrono::milliseconds{250}, "status interval should parse");
    expect(config.piStatus.pollInterval == std::chrono::milliseconds{100}, "Pi poll interval should clamp to 100 ms");
    expect(config.tsGateway.address == "10.10.20.1", "TS gateway address should parse");
    expect(config.gatewayInput.listenAddress == "0.0.0.0", "gateway listen address should parse");
    expect(config.gatewayInput.packetSize == 1316, "gateway packet size should parse");
    expect(!config.analogue.capture.enabled, "analogue capture should remain disabled");
    expect(config.analogue.capture.captureInputFormat == "mjpeg", "analogue capture input format should parse");
    expect(config.analogue.capture.audioEnabled, "analogue audio enabled should parse");
    expect(config.analogue.capture.audioDevice == "hw:1,0", "analogue audio device should parse");
    expect(config.analogue.capture.audioSampleRate == 48000, "analogue audio sample rate should parse");
    expect(config.analogue.capture.audioChannels == 2, "analogue audio channels should parse");
    expect(config.analogue.capture.audioDelayMs == 120, "analogue audio delay should parse");
    expect(config.analogue.capture.lockMode == "pi-gpio", "analogue lock mode should parse");
    expect(config.hardwarePtt.mode == "pi-gpio", "hardware PTT mode should parse");
    expect(config.analogue.capture.captureFrameRateNumerator == 30000, "fractional analogue frame-rate numerator should parse");
    expect(config.analogue.capture.captureFrameRateDenominator == 1001, "fractional analogue frame-rate denominator should parse");
}

void enforcesReceiverAntennaInterlock()
{
    const auto config = configFromJson(gatewayConfigJson());

    expect(config.receivers.size() == 4, "four receivers should parse");
    expect(!config.receivers[0].targets.empty(), "receiver 1 should have a target");
    expect(!config.receivers[1].targets.empty(), "receiver 2 should have a target");
    expect(config.receivers[0].targets[0].antenna == Antenna::top, "receiver 1 target should be forced to top antenna");
    expect(config.receivers[1].targets[0].antenna == Antenna::bottom, "receiver 2 target should be forced to bottom antenna");
}

void serialisesRoundTripFields()
{
    const auto config = configFromJson(gatewayConfigJson());
    const auto roundTrip = configFromJson(configToJson(config));

    expect(roundTrip.mode == "pc-gateway", "mode should round-trip");
    expect(roundTrip.piStatus.address == "10.10.20.2", "Pi status address should round-trip");
    expect(roundTrip.gatewayInput.packetSize == 1316, "gateway packet size should round-trip");
    expect(roundTrip.fallback.hardwareDecode, "fallback hardware decode should round-trip");
    expect(roundTrip.fallback.videoPaths.size() == 1, "fallback video path should round-trip");
    expect(roundTrip.pluto.outputAudioChannels == 2, "output audio channels should round-trip");
    expect(roundTrip.pluto.digitalGainDb == -3, "Pluto digital gain should round-trip");
    expect(roundTrip.pluto.firFilter, "Pluto FIR filter should round-trip");
    expect(!roundTrip.analogue.capture.enabled, "disabled analogue state should round-trip");
    expect(roundTrip.analogue.capture.receiver.value == 5, "analogue receiver id should round-trip");
    expect(roundTrip.analogue.capture.captureInputFormat == "mjpeg", "analogue capture input format should round-trip");
    expect(roundTrip.analogue.capture.audioEnabled, "analogue audio enabled should round-trip");
    expect(roundTrip.analogue.capture.audioDevice == "hw:1,0", "analogue audio device should round-trip");
    expect(roundTrip.analogue.capture.audioSampleRate == 48000, "analogue audio sample rate should round-trip");
    expect(roundTrip.analogue.capture.audioChannels == 2, "analogue audio channels should round-trip");
    expect(roundTrip.analogue.capture.audioDelayMs == 120, "analogue audio delay should round-trip");
    expect(roundTrip.analogue.capture.lockMode == "pi-gpio", "analogue lock mode should round-trip");
    expect(roundTrip.hardwarePtt.mode == "pi-gpio", "hardware PTT mode should round-trip");
}

void calculatesPlutoRates()
{
    PlutoConfig dvbs;
    dvbs.system = "dvbs";
    dvbs.constellation = "qpsk";
    dvbs.symbolRateS = 4'000'000;
    dvbs.fec = "1/2";
    dvbs.audioBitrateKbps = 96;
    expect(calculatePlutoMuxRateKbps(dvbs) == 3686, "DVB-S 4 MS/s QPSK 1/2 mux rate should match TS payload rate");
    expect(calculatePlutoVideoBitrateKbps(dvbs) == 3590, "video bitrate should reserve configured audio bitrate");

    PlutoConfig starved;
    starved.system = "dvbs";
    starved.constellation = "qpsk";
    starved.symbolRateS = 1000;
    starved.fec = "1/2";
    starved.audioBitrateKbps = 96;
    expect(calculatePlutoVideoBitrateKbps(starved) == 1, "video bitrate should not underflow when audio exceeds mux rate");
}

void clampsOutputAudioChannels()
{
    const auto mono = configFromJson(R"json({
      "pluto": {"outputAudioChannels": 0},
      "receivers": []
    })json");
    expect(mono.pluto.outputAudioChannels == 1, "output audio channels should clamp low values to mono");

    const auto stereo = configFromJson(R"json({
      "pluto": {"outputAudioChannels": 6},
      "receivers": []
    })json");
    expect(stereo.pluto.outputAudioChannels == 2, "output audio channels should clamp high values to stereo");
}

void rejectsInvalidConfig()
{
    expectThrows([&] {
        (void)configFromJson(R"json({
          "gatewayInput": {"packetSize": 1000},
          "receivers": []
        })json");
    }, "gateway packet size must be a positive multiple of TS packet size");

    expectThrows([&] {
        (void)configFromJson(R"json({
          "analogue": {"capture": {"captureStandard": "pal-i"}},
          "receivers": []
        })json");
    }, "unsupported analogue standards should be rejected");

    expectThrows([&] {
        (void)configFromJson(R"json({
          "analogue": {"capture": {"captureInputFormat": "rgb"}},
          "receivers": []
        })json");
    }, "unsupported analogue capture formats should be rejected");

    expectThrows([&] {
        (void)configFromJson(R"json({
          "analogue": {"capture": {"lockMode": "guess"}},
          "receivers": []
        })json");
    }, "unsupported analogue lock modes should be rejected");

    expectThrows([&] {
        (void)configFromJson(R"json({
          "hardwarePtt": {"mode": "network"},
          "receivers": []
        })json");
    }, "unsupported hardware PTT modes should be rejected");
}

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"parses gateway and analogue config", parsesGatewayAndAnalogueConfig},
        {"enforces receiver antenna interlock", enforcesReceiverAntennaInterlock},
        {"serialises round-trip fields", serialisesRoundTripFields},
        {"calculates Pluto rates", calculatesPlutoRates},
        {"clamps output audio channels", clampsOutputAudioChannels},
        {"rejects invalid config", rejectsInvalidConfig},
    };

    int failures{};
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
        }
    }

    return failures == 0 ? 0 : 1;
}
