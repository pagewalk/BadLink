#ifndef BADLINK_SRC_SIMULATION_MODULE_H_
#define BADLINK_SRC_SIMULATION_MODULE_H_

#include <cstdint>
#include <vector>
#include <chrono>
#include <span>
#include <windivert.h>

namespace BadLink {

    struct SimulatedPacket {
        std::vector<uint8_t> data;
        WINDIVERT_ADDRESS addr{};
        std::chrono::steady_clock::time_point timestamp;
        std::chrono::steady_clock::time_point release_time;
    };

    class SimulationModule {
    public:
        virtual ~SimulationModule() = default;

        // Process a batch of packets, returns packets to send immediately
        // Packets not returned are either dropped or delayed
        virtual std::vector<SimulatedPacket> ProcessBatch(
            std::vector<SimulatedPacket>&& packets) = 0;

        // Get any packets that are ready to be released (for time-based effects)
        virtual std::vector<SimulatedPacket> GetReleasablePackets() = 0;

        // Check if module is enabled
        virtual bool IsEnabled() const = 0;

        // Set direction filters
        virtual void SetInboundEnabled(bool enabled) = 0;
        virtual void SetOutboundEnabled(bool enabled) = 0;
    };
}
#endif  // BADLINK_SRC_SIMULATION_MODULE_H_