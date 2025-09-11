#define NOMINMAX
#include "jitter_module.h"
#include <chrono>
#include <random>
#include <mutex>
#include <queue>

namespace BadLink {

    thread_local std::mt19937 JitterModule::rng_{ std::random_device{}() };

    JitterModule::JitterModule() = default;
    JitterModule::~JitterModule() = default;

    void JitterModule::SetJitterRange(uint32_t min_ms, uint32_t max_ms) {
        min_jitter_ms_.store(std::min(min_ms, max_ms));
        max_jitter_ms_.store(std::max(min_ms, max_ms));
    }

    uint32_t JitterModule::GetMinJitter() const {
        return min_jitter_ms_.load();
    }

    uint32_t JitterModule::GetMaxJitter() const {
        return max_jitter_ms_.load();
    }

    void JitterModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
    }

    bool JitterModule::IsEnabled() const {
        return enabled_.load();
    }

    void JitterModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void JitterModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> JitterModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);
        }

        std::vector<SimulatedPacket> immediate_packets;
        const auto current_time = std::chrono::steady_clock::now();

        for (auto&& packet : packets) {
            if (ShouldProcess(packet.addr)) {
                // Apply jitter delay between min and max milliseconds
                uint32_t jitter_ms = GenerateJitter();
                std::chrono::milliseconds delay(jitter_ms);

                packet.release_time = current_time + delay;

                std::lock_guard<std::mutex> lock(buffer_mutex_);
                delayed_packets_.push(std::move(packet));
            }
            else {
                immediate_packets.push_back(std::move(packet));
            }
        }

        return immediate_packets;
    }

    std::vector<SimulatedPacket> JitterModule::GetReleasablePackets() {
        std::vector<SimulatedPacket> ready_packets;

        if (!enabled_.load()) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            while (!delayed_packets_.empty()) {
                ready_packets.push_back(std::move(const_cast<SimulatedPacket&>(delayed_packets_.top())));
                delayed_packets_.pop();
            }
            return ready_packets;
        }

        const auto current_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(buffer_mutex_);
        while (!delayed_packets_.empty() && delayed_packets_.top().release_time <= current_time) {
            ready_packets.push_back(std::move(const_cast<SimulatedPacket&>(delayed_packets_.top())));
            delayed_packets_.pop();
        }

        return ready_packets;
    }

    bool JitterModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

    uint32_t JitterModule::GenerateJitter() const {
        const uint32_t min_ms = min_jitter_ms_.load();
        const uint32_t max_ms = max_jitter_ms_.load();

        if (min_ms == max_ms) {
            return min_ms;
        }

        std::uniform_int_distribution<uint32_t> dist(min_ms, max_ms);
        return dist(rng_);
    }

}