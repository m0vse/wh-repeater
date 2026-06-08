#pragma once

#include "whrepeater/config.hpp"
#include "whrepeater/ts_router.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace whrepeater {

class PlutoSink final : public TsSink {
public:
    explicit PlutoSink(PlutoConfig config);
    ~PlutoSink() override;

    PlutoSink(const PlutoSink&) = delete;
    PlutoSink& operator=(const PlutoSink&) = delete;

    PlutoSink(PlutoSink&& other) noexcept;
    PlutoSink& operator=(PlutoSink&& other) noexcept;

    void write(std::span<const std::byte> packet) override;
    void writeMuxData(std::span<const std::byte> data);
    void setTransmitEnabled(bool enabled);

private:
    void openSocket();
    void flushDatagram();
    void configureTransmitter();
    void publishControl(std::string_view suffix, std::string_view payload);

    PlutoConfig config_;
    int socket_{-1};
    std::vector<std::byte> datagram_;
    bool configured_{false};
    bool transmitEnabled_{false};
};

} // namespace whrepeater
