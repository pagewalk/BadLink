#ifndef BADLINK_SRC_JITTER_MODULE_H_
#define BADLINK_SRC_JITTER_MODULE_H_

#include "simulation_module.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <random>
#include <chrono>

namespace BadLink {

    class JitterModule : public SimulationModule {
    public:
        JitterModule();
        ~JitterModule() override;

        // Set jitter range in milliseconds
        void SetJitterRange(uint32_t min_ms, uint32_t max_ms);
        uint32_t GetMinJitter() const;
        uint32_t GetMaxJitter() const;

        // Enable/disable the module
        void SetEnabled(bool enabled);
        bool IsEnabled() const override;

        // Direction control
        void SetInboundEnabled(bool enabled) override;
        void SetOutboundEnabled(bool enabled) override;

        // Process packets
        std::vector<SimulatedPacket> ProcessBatch(
            std::vector<SimulatedPacket>&& packets) override;
        std::vector<SimulatedPacket> GetReleasablePackets() override;

    private:
        std::atomic<bool> enabled_{ false };
        std::atomic<bool> inbound_enabled_{ true };
        std::atomic<bool> outbound_enabled_{ true };
        std::atomic<uint32_t> min_jitter_ms_{ 0 };
        std::atomic<uint32_t> max_jitter_ms_{ 50 };

        // Priority queue for delayed packets
        struct PacketComparator {
            bool operator()(const SimulatedPacket& a, const SimulatedPacket& b) const {
                return a.release_time > b.release_time;  // Min-heap by release time
            }
        };

        mutable std::mutex buffer_mutex_;
        std::priority_queue<SimulatedPacket, std::vector<SimulatedPacket>, PacketComparator> delayed_packets_;

        // Thread-local random generator
        thread_local static std::mt19937 rng_;

        static uint64_t GetCurrentTimeNs();
        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;
        uint32_t GenerateJitter() const;
    };

}
#endif  // BADLINK_SRC_JITTER_MODULE_H_