#include "duplicate_module.h"
#include <algorithm>

namespace BadLink {

    DuplicateModule::DuplicateModule() = default;
    DuplicateModule::~DuplicateModule() = default;

    void DuplicateModule::SetDuplicationRate(float duplication_percentage) {
        duplication_rate_.store(std::clamp(duplication_percentage, 0.0f, 100.0f));
    }

    float DuplicateModule::GetDuplicationRate() const {
        return duplication_rate_.load();
    }

    void DuplicateModule::SetDuplicateCount(uint32_t count) {
        duplicate_count_.store(std::clamp(count, 1u, 5u));
    }

    uint32_t DuplicateModule::GetDuplicateCount() const {
        return duplicate_count_.load();
    }

    void DuplicateModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
    }

    bool DuplicateModule::IsEnabled() const {
        return enabled_.load();
    }

    void DuplicateModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void DuplicateModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> DuplicateModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);
        }

        std::vector<SimulatedPacket> output_packets;
        output_packets.reserve(packets.size() * 2);  // Reserve space for duplicates

        for (auto&& packet : packets) {
            // Always include original packet
            output_packets.push_back(std::move(packet));

            // Check if we should duplicate this packet
            if (ShouldProcess(output_packets.back().addr) && ShouldDuplicate()) {
                const uint32_t count = duplicate_count_.load();
                for (uint32_t i = 0; i < count; ++i) {
                    // Create duplicate and deep copy the packet
                    SimulatedPacket duplicate = output_packets.back();
                    output_packets.push_back(std::move(duplicate));
                }
            }
        }

        return output_packets;
    }

    std::vector<SimulatedPacket> DuplicateModule::GetReleasablePackets() {
        return {};  // Duplication doesn't delay packets
    }

    bool DuplicateModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

    bool DuplicateModule::ShouldDuplicate() const {
        const float rate = duplication_rate_.load();
        if (rate <= 0.0f) return false;
        if (rate >= 100.0f) return true;
        return RandomUtils::GetPercentage() < rate;
    }

}