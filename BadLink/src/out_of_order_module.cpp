#include "out_of_order_module.h"
#include <algorithm>
#include <iterator>

namespace BadLink {

    OutOfOrderModule::OutOfOrderModule() = default;
    OutOfOrderModule::~OutOfOrderModule() = default;

    void OutOfOrderModule::SetReorderRate(float reorder_percentage) {
        reorder_rate_.store(std::clamp(reorder_percentage, 0.0f, 100.0f));
    }

    float OutOfOrderModule::GetReorderRate() const {
        return reorder_rate_.load();
    }

    void OutOfOrderModule::SetReorderGap(uint32_t gap) {
        reorder_gap_.store(std::clamp(gap, 2u, 10u));
    }

    uint32_t OutOfOrderModule::GetReorderGap() const {
        return reorder_gap_.load();
    }

    void OutOfOrderModule::SetEnabled(bool enabled) {
        enabled_.store(enabled);
    }

    bool OutOfOrderModule::IsEnabled() const {
        return enabled_.load();
    }

    void OutOfOrderModule::SetInboundEnabled(bool enabled) {
        inbound_enabled_.store(enabled);
    }

    void OutOfOrderModule::SetOutboundEnabled(bool enabled) {
        outbound_enabled_.store(enabled);
    }

    std::vector<SimulatedPacket> OutOfOrderModule::ProcessBatch(
        std::vector<SimulatedPacket>&& packets) {

        if (!enabled_.load()) {
            return std::move(packets);
        }

        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Add new packets to buffer
        for (auto&& packet : packets) {
            if (ShouldProcess(packet.addr)) {
                packet_buffer_.push_back(std::move(packet));
            }
            else {
                packet_buffer_.push_back(std::move(packet));
            }
        }

        std::vector<SimulatedPacket> output_packets;
        const uint32_t gap = reorder_gap_.load();

        // If buffer has enough packets, process them
        if (packet_buffer_.size() >= gap) {
            // Determine how many packets to release
            size_t release_count = packet_buffer_.size() - (gap / 2);

            // If we should reorder, shuffle the packets
            if (ShouldReorder()) {
                ShuffleBuffer();
            }

            // Move packets to output
            for (size_t i = 0; i < release_count && !packet_buffer_.empty(); ++i) {
                output_packets.push_back(std::move(packet_buffer_.front()));
                packet_buffer_.pop_front();
            }
        }

        return output_packets;
    }

    std::vector<SimulatedPacket> OutOfOrderModule::GetReleasablePackets() {
        if (!enabled_.load()) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            std::vector<SimulatedPacket> remaining;
            remaining.reserve(packet_buffer_.size());
            while (!packet_buffer_.empty()) {
                remaining.push_back(std::move(packet_buffer_.front()));
                packet_buffer_.pop_front();
            }
            return remaining;
        }
        return {};
    }

    bool OutOfOrderModule::ShouldProcess(const WINDIVERT_ADDRESS& addr) const {
        if (addr.Outbound && !outbound_enabled_.load()) {
            return false;
        }
        if (!addr.Outbound && !inbound_enabled_.load()) {
            return false;
        }
        return true;
    }

    bool OutOfOrderModule::ShouldReorder() const {
        const float rate = reorder_rate_.load();
        if (rate <= 0.0f) return false;
        if (rate >= 100.0f) return true;
        return RandomUtils::GetPercentage() < rate;
    }

    void OutOfOrderModule::ShuffleBuffer() {
        if (packet_buffer_.size() <= 1) {
            return;
        }

        // Vector has better cache locality for shuffling large buffers
        std::vector<SimulatedPacket> temp(
            std::make_move_iterator(packet_buffer_.begin()),
            std::make_move_iterator(packet_buffer_.end())
        );
        packet_buffer_.clear();

        std::shuffle(temp.begin(), temp.end(), RandomUtils::GetGenerator());

        packet_buffer_.assign(
            std::make_move_iterator(temp.begin()),
            std::make_move_iterator(temp.end())
        );
    }

}