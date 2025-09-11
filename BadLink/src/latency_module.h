#ifndef BADLINK_SRC_LATENCY_MODULE_H_
#define BADLINK_SRC_LATENCY_MODULE_H_

#include "simulation_module.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <chrono>
#include <span>

namespace BadLink {

    class LatencyModule : public SimulationModule {
    public:
        LatencyModule();
        ~LatencyModule() override;

        void SetLatency(uint32_t latency_ms);
        uint32_t GetLatency() const;

        void SetEnabled(bool enabled);
        bool IsEnabled() const override;

        void SetInboundEnabled(bool enabled) override;
        void SetOutboundEnabled(bool enabled) override;

        std::vector<SimulatedPacket> ProcessBatch(std::vector<SimulatedPacket>&& packets) override;
        std::vector<SimulatedPacket> GetReleasablePackets() override;

    private:
        // Comparator struct
        struct PacketCompare {
            bool operator()(const SimulatedPacket& a, const SimulatedPacket& b) const {
                return a.release_time > b.release_time;
            }
        };

        using PacketQueue = std::priority_queue<
            SimulatedPacket,
            std::vector<SimulatedPacket>,
            PacketCompare
        >;

        std::atomic<bool> enabled_{ false };
        std::atomic<bool> inbound_enabled_{ true };
        std::atomic<bool> outbound_enabled_{ true };
        std::atomic<std::chrono::milliseconds> latency_{ std::chrono::milliseconds::zero() };

        mutable std::mutex buffer_mutex_;
        PacketQueue delayed_packets_;

        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;
    };
}
#endif  // BADLINK_SRC_LATENCY_MODULE_H_