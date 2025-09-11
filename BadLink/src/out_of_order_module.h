#ifndef BADLINK_SRC_OUT_OF_ORDER_MODULE_H_
#define BADLINK_SRC_OUT_OF_ORDER_MODULE_H_

#include "simulation_module.h"
#include "random_utils.h"
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>

namespace BadLink {

    class OutOfOrderModule : public SimulationModule {
    public:
        OutOfOrderModule();
        ~OutOfOrderModule() override;

        // Set reorder percentage (0.0 - 100.0)
        void SetReorderRate(float reorder_percentage);
        float GetReorderRate() const;

        // Set reorder gap (how many packets to buffer before reordering)
        void SetReorderGap(uint32_t gap);
        uint32_t GetReorderGap() const;

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
        std::atomic<float> reorder_rate_{ 0.0f };
        std::atomic<uint32_t> reorder_gap_{ 3 };

        mutable std::mutex buffer_mutex_;
        std::deque<SimulatedPacket> packet_buffer_;

        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;
        bool ShouldReorder() const;
        void ShuffleBuffer();
    };

}
#endif  // BADLINK_SRC_OUT_OF_ORDER_MODULE_H_