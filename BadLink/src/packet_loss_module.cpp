#include "packet_loss_module.h"
#include <algorithm>

namespace BadLink {

    PacketLossModule::PacketLossModule() = default;
    PacketLossModule::~PacketLossModule() = default;

    void PacketLossModule::SetLossRate(float loss_percentage) {
        loss_rate_.store(std::clamp(loss_percentage, 0.0f, 100.0f));
    }

    float PacketLossModule::GetLossRate() const {
        return loss_rate_.load();
    }

    void PacketLossModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
    }

    bool PacketLossModule::IsEnabled() const {
        return enabled_.load();
    }

    void PacketLossModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void PacketLossModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> PacketLossModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);  // Pass through if disabled
        }

        std::vector<SimulatedPacket> surviving_packets;
        surviving_packets.reserve(packets.size());

        for (auto&& packet : packets) {
            if (ShouldProcess(packet.addr) && ShouldDrop()) {
                // Packet is dropped simply don't add it to surviving packets
                // Memory will be freed when packet goes out of scope
            }
            else {
                surviving_packets.push_back(std::move(packet));
            }
        }

        return surviving_packets;
    }

    std::vector<SimulatedPacket> PacketLossModule::GetReleasablePackets() {
        // Packet loss doesn't delay packets, so nothing to release
        return {};
    }

    bool PacketLossModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

    bool PacketLossModule::ShouldDrop() const {
        const float loss_rate = loss_rate_.load();
        if (loss_rate <= 0.0f) {
            return false;
        }
        if (loss_rate >= 100.0f) {
            return true;
        }
        return RandomUtils::GetPercentage() < loss_rate;
    }

}