#include "latency_module.h"
#include <windivert.h>

namespace BadLink {

    LatencyModule::LatencyModule() = default;
    LatencyModule::~LatencyModule() = default;

    void LatencyModule::SetLatency(uint32_t latency_ms) {
        latency_.store(std::chrono::milliseconds(latency_ms));
    }

    uint32_t LatencyModule::GetLatency() const {
        return static_cast<uint32_t>(latency_.load().count());
    }

    void LatencyModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
    }

    bool LatencyModule::IsEnabled() const {
        return enabled_.load();
    }

    void LatencyModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void LatencyModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> LatencyModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);  // Pass through if disabled
        }

        std::vector<SimulatedPacket> immediate_packets;
        const auto delay = latency_.load();
        const auto current_time = std::chrono::steady_clock::now();

        for (auto&& packet : packets) {
            bool should_delay = ShouldProcess(packet.addr);

            if (should_delay) {
                // Apply latency and set release time
                packet.release_time = current_time + delay;

                std::lock_guard<std::mutex> lock(buffer_mutex_);
                delayed_packets_.push(std::move(packet));
            }
            else {
                // No processing needed, send immediately
                immediate_packets.push_back(std::move(packet));
            }
        }

        return immediate_packets;
    }

    std::vector<SimulatedPacket> LatencyModule::GetReleasablePackets() {
        std::vector<SimulatedPacket> ready_packets;

        if (!enabled_.load()) {
            // If disabled, flush all delayed packets
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            while (!delayed_packets_.empty()) {
                ready_packets.push_back(std::move(const_cast<SimulatedPacket&>(delayed_packets_.top())));
                delayed_packets_.pop();
            }
            return ready_packets;
        }

        const auto current_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(buffer_mutex_);
        while (!delayed_packets_.empty() &&
            delayed_packets_.top().release_time <= current_time) {
            ready_packets.push_back(std::move(const_cast<SimulatedPacket&>(delayed_packets_.top())));
            delayed_packets_.pop();
        }

        return ready_packets;
    }

    bool LatencyModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

}