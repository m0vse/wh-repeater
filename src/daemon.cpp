#include "whrepeater/daemon.hpp"

#include "whrepeater/ident.hpp"
#include "whrepeater/nim_controller.hpp"
#include "whrepeater/pluto_sink.hpp"
#include "whrepeater/scan_scheduler.hpp"
#include "whrepeater/signal_arbitrator.hpp"
#include "whrepeater/ts_router.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace whrepeater {

Daemon::Daemon(RepeaterConfig config)
    : config_{std::move(config)}
{
}

int Daemon::run()
{
    StubNimController nim;
    ScanScheduler scanner{config_.receivers};
    SignalArbitrator arbitrator{config_};
    TsRouter router;
    PlutoSink pluto{config_.pluto};
    IdentInserter ident{config_.ident};

    nim.initialise();

    std::cout << "wh-repeater daemon starting with " << config_.receivers.size() << " receivers\n";

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        scanner.tick(nim, now);
        auto statuses = nim.pollStatus();
        auto active = arbitrator.choose(statuses);

        if (active.has_value()) {
            std::cout << "active RX" << active->receiver.value << " "
                      << active->target.frequencyKhz << " kHz SR"
                      << active->target.symbolRateKs << '\n';
        }

        ident.update(active, now);
        router.select(std::move(active));
        router.pump(pluto);

        std::this_thread::sleep_for(config_.statusInterval);
    }
}

} // namespace whrepeater
