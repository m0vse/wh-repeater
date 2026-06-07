#include "whrepeater/config.hpp"

#include <stdexcept>
#include <utility>

namespace whrepeater {

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
    return config;
}

RepeaterConfig loadConfig(const std::filesystem::path& path)
{
    // Placeholder for a real parser once the config file format is chosen.
    // Keeping this explicit prevents silent use of defaults when a file was requested.
    throw std::runtime_error{"config parser not implemented yet: " + path.string()};
}

} // namespace whrepeater
