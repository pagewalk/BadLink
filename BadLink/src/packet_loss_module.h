#ifndef BADLINK_SRC_PACKET_LOSS_MODULE_H_
#define BADLINK_SRC_PACKET_LOSS_MODULE_H_

#include "simulation_module.h"
#include "random_utils.h"
#include <atomic>

namespace BadLink {

    class PacketLossModule : public SimulationModule {
    public:
        PacketLossModule();
        ~PacketLossModule() override;

        // Set packet loss percentage (0.0 - 100.0)
        void SetLossRate(float loss_percentage);
        float GetLossRate() const;

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
        std::atomic<float> loss_rate_{ 0.0f };

        // Check if packet should be processed based on direction
        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;

        // Determine if packet should be dropped
        bool ShouldDrop() const;
    };

}
#endif  // BADLINK_SRC_PACKET_LOSS_MODULE_H_