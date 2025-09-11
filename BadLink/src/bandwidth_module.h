#ifndef BADLINK_SRC_BANDWIDTH_MODULE_H_
#define BADLINK_SRC_BANDWIDTH_MODULE_H_

#include "simulation_module.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <chrono>

namespace BadLink {

    class BandwidthModule : public SimulationModule {
    public:
        BandwidthModule();
        ~BandwidthModule() override;

        // Set bandwidth limit in kilobits per second
        void SetBandwidthLimit(uint32_t kbps);
        uint32_t GetBandwidthLimit() const;

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
        std::atomic<uint32_t> bandwidth_kbps_{ 1000 };  // Default 1 Mbps

        // Token bucket for rate limiting
        mutable std::mutex bucket_mutex_;
        std::chrono::steady_clock::time_point last_refill_time_;
        double available_bytes_;
        double max_burst_bytes_;

        // Packet queue
        std::queue<SimulatedPacket> packet_queue_;

        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;
        void RefillTokenBucket();
        bool ConsumeTokens(size_t bytes);
    };

}
#endif  // BADLINK_SRC_BANDWIDTH_MODULE_H_