#ifndef BADLINK_SRC_DUPLICATE_MODULE_H_
#define BADLINK_SRC_DUPLICATE_MODULE_H_

#include "simulation_module.h"
#include "random_utils.h"
#include <atomic>

namespace BadLink {

    class DuplicateModule : public SimulationModule {
    public:
        DuplicateModule();
        ~DuplicateModule() override;

        // Set duplication percentage (0.0 - 100.0)
        void SetDuplicationRate(float duplication_percentage);
        float GetDuplicationRate() const;

        // Set number of duplicates per packet (1-5)
        void SetDuplicateCount(uint32_t count);
        uint32_t GetDuplicateCount() const;

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
        std::atomic<float> duplication_rate_{ 0.0f };
        std::atomic<uint32_t> duplicate_count_{ 1 };

        bool ShouldProcess(const WINDIVERT_ADDRESS& addr) const;
        bool ShouldDuplicate() const;
    };

}
#endif  // BADLINK_SRC_DUPLICATE_MODULE_H_