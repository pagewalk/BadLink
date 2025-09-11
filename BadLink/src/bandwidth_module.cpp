#define NOMINMAX
#include "bandwidth_module.h"

namespace BadLink {

    BandwidthModule::BandwidthModule()
        : last_refill_time_(std::chrono::steady_clock::now())
        , available_bytes_(0)
        , max_burst_bytes_(125000) {  // 1 second worth at 1Mbps
    }

    BandwidthModule::~BandwidthModule() = default;

    void BandwidthModule::SetBandwidthLimit(uint32_t kbps) {
        bandwidth_kbps_.store(kbps);
        std::lock_guard<std::mutex> lock(bucket_mutex_);
        // Update burst size to 1 second worth of data
        max_burst_bytes_ = (kbps * 1000.0) / 8.0;
    }

    uint32_t BandwidthModule::GetBandwidthLimit() const {
        return bandwidth_kbps_.load();
    }

    void BandwidthModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
        if (enabled) {
            std::lock_guard<std::mutex> lock(bucket_mutex_);
            last_refill_time_ = std::chrono::steady_clock::now();
            available_bytes_ = max_burst_bytes_ / 2;  // Start with half bucket
        }
    }

    bool BandwidthModule::IsEnabled() const {
        return enabled_.load();
    }

    void BandwidthModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void BandwidthModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> BandwidthModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);
        }

        std::lock_guard<std::mutex> lock(bucket_mutex_);
        RefillTokenBucket();

        std::vector<SimulatedPacket> output_packets;

        // Add new packets to queue
        for (auto&& packet : packets) {
            if (ShouldProcess(packet.addr)) {
                packet_queue_.push(std::move(packet));
            }
            else {
                output_packets.push_back(std::move(packet));
            }
        }

        // Process queued packets with bandwidth limit
        while (!packet_queue_.empty()) {
            auto& front_packet = packet_queue_.front();
            size_t packet_size = front_packet.data.size();

            if (ConsumeTokens(packet_size)) {
                output_packets.push_back(std::move(front_packet));
                packet_queue_.pop();
            }
            else {
                // Not enough bandwidth available
                break;
            }
        }

        return output_packets;
    }

    std::vector<SimulatedPacket> BandwidthModule::GetReleasablePackets() {
        if (!enabled_.load()) {
            std::lock_guard<std::mutex> lock(bucket_mutex_);
            std::vector<SimulatedPacket> remaining;
            while (!packet_queue_.empty()) {
                remaining.push_back(std::move(packet_queue_.front()));
                packet_queue_.pop();
            }
            return remaining;
        }

        std::lock_guard<std::mutex> lock(bucket_mutex_);
        RefillTokenBucket();

        std::vector<SimulatedPacket> output_packets;

        while (!packet_queue_.empty()) {
            auto& front_packet = packet_queue_.front();
            size_t packet_size = front_packet.data.size();

            if (ConsumeTokens(packet_size)) {
                output_packets.push_back(std::move(front_packet));
                packet_queue_.pop();
            }
            else {
                break;
            }
        }

        return output_packets;
    }

    bool BandwidthModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

    void BandwidthModule::RefillTokenBucket() {
        const auto current_time = std::chrono::steady_clock::now();
        const auto elapsed = current_time - last_refill_time_;

        // Need floating point for sub-millisecond precision
        // Integer math can cause stuttering at low bandwidth-rates
        const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();

        double bytes_per_second = (bandwidth_kbps_.load() * 1000.0) / 8.0;
        double bytes_to_add = bytes_per_second * elapsed_seconds;

        available_bytes_ = std::min(available_bytes_ + bytes_to_add, max_burst_bytes_);
        last_refill_time_ = current_time;
    }

    bool BandwidthModule::ConsumeTokens(size_t bytes) {
        if (available_bytes_ >= bytes) {
            available_bytes_ -= bytes;
            return true;
        }
        return false;
    }
}