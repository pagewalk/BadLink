#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "network_capture.h"
#include "latency_module.h"
#include "packet_loss_module.h"
#include "duplicate_module.h"
#include "out_of_order_module.h"
#include "jitter_module.h"
#include "bandwidth_module.h"
#include <chrono>
#include <string>
#include <format>
#include <ranges>

#pragma comment(lib, "ws2_32.lib")

#ifndef ERROR_NO_DATA
#define ERROR_NO_DATA 232L
#endif

namespace BadLink {

    [[nodiscard]] std::string IPv4Address::ToString() const {
        // WinDivert gives us network byte order (big-endian)
        // most significant byte is actually in bits 24-31, not 0-7
        return std::format("{}.{}.{}.{}",
            (addr >> 24) & 0xFF,
            (addr >> 16) & 0xFF,
            (addr >> 8) & 0xFF,
            addr & 0xFF);
    }

    [[nodiscard]] std::string IPv6Address::ToString() const {
        char buffer[INET6_ADDRSTRLEN];
        struct in6_addr in6 {};

        // IMPORTANT: addr[] must already be in network byte order from WinDivert
        // Don't try to "fix" byte order here
        std::memcpy(in6.s6_addr, addr.data(), sizeof(addr));

        // inet_ntop handles the IPv6 compression (:: notation)
        if (inet_ntop(AF_INET6, &in6, buffer, sizeof(buffer))) {
            return std::string(buffer);
        }

        return "::";  // fallback on error
    }

    NetworkCapture::NetworkCapture()
        : latency_module_(std::make_unique<LatencyModule>())
        , packet_loss_module_(std::make_unique<PacketLossModule>())
        , duplicate_module_(std::make_unique<DuplicateModule>())
        , out_of_order_module_(std::make_unique<OutOfOrderModule>())
        , jitter_module_(std::make_unique<JitterModule>())
        , bandwidth_module_(std::make_unique<BandwidthModule>()) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    NetworkCapture::~NetworkCapture() {
        Stop();
        WSACleanup();
    }

    std::expected<void, std::string> NetworkCapture::Start(const std::string& filter, const CaptureParameters& params) {
        if (is_capturing_.load()) {
            return std::unexpected("Already capturing");
        }

        // Store parameters
        {
            std::lock_guard<std::mutex> lock(params_mutex_);
            current_params_ = params;
            max_packets_ = params.ring_packet_buffer;
        }

        // Open WinDivert handle
        divert_handle_ = WinDivertOpen(filter.c_str(), WINDIVERT_LAYER_NETWORK, 0, 0);
        if (divert_handle_ == INVALID_HANDLE_VALUE) {
            DWORD error = ::GetLastError();
            return std::unexpected(std::format("Failed to open WinDivert: {}", error));
        }

        // Configure WinDivert parameters
        if (!WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_LENGTH, params.queue_length)) {
            WinDivertClose(divert_handle_);
            divert_handle_ = INVALID_HANDLE_VALUE;
            return std::unexpected("Failed to set queue length");
        }

        if (!WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_TIME, params.queue_time)) {
            WinDivertClose(divert_handle_);
            divert_handle_ = INVALID_HANDLE_VALUE;
            return std::unexpected("Failed to set queue time");
        }

        if (!WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_SIZE, params.queue_size)) {
            WinDivertClose(divert_handle_);
            divert_handle_ = INVALID_HANDLE_VALUE;
            return std::unexpected("Failed to set queue size");
        }

        // Reset state
        should_stop_.store(false);
        packets_captured_.store(0);
        packets_dropped_.store(0);
        packets_injected_.store(0);
        bytes_captured_.store(0);
        batch_count_.store(0);
        total_batch_packets_.store(0);

        // Start capture threads
        is_capturing_.store(true);
        capture_threads_.reserve(params.worker_threads);
        for (uint32_t i = 0; i < params.worker_threads; ++i) {
            capture_threads_.emplace_back(&NetworkCapture::CaptureThreadBatch, this);
        }

        // Start release threads for time-based modules if enabled
        if (latency_module_->IsEnabled()) {
            latency_thread_ = std::jthread(&NetworkCapture::LatencyReleaseThread, this);
        }
        if (jitter_module_->IsEnabled()) {
            jitter_thread_ = std::jthread(&NetworkCapture::JitterReleaseThread, this);
        }
        if (bandwidth_module_->IsEnabled()) {
            bandwidth_thread_ = std::jthread(&NetworkCapture::BandwidthReleaseThread, this);
        }

        return {};
    }

    void NetworkCapture::Stop() {
        if (!is_capturing_.load()) {
            return;
        }

        // Signal threads to stop
        should_stop_.store(true);

        // Gracefully shutdown WinDivert handle
        if (divert_handle_ != INVALID_HANDLE_VALUE) {
            // First shutdown receive to unblock WinDivertRecvEx calls
            WinDivertShutdown(divert_handle_, WINDIVERT_SHUTDOWN_RECV);

            // Give threads time to process remaining packets
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            // Then close the handle
            WinDivertClose(divert_handle_);
            divert_handle_ = INVALID_HANDLE_VALUE;
        }

        // jthread automatically joins on destruction
        capture_threads_.clear();

        // Stop release threads (jthread automatically joins)
        latency_thread_ = {};
        jitter_thread_ = {};
        bandwidth_thread_ = {};

        // Flush any remaining delayed packets
        [[maybe_unused]] auto remaining_latency = latency_module_->GetReleasablePackets();
        [[maybe_unused]] auto remaining_jitter = jitter_module_->GetReleasablePackets();
        [[maybe_unused]] auto remaining_bandwidth = bandwidth_module_->GetReleasablePackets();
        [[maybe_unused]] auto remaining_order = out_of_order_module_->GetReleasablePackets();

        is_capturing_.store(false);
    }

    // Latency control methods
    void NetworkCapture::SetLatencyEnabled(bool enabled) {
        latency_module_->SetEnabled(enabled);

        // Start or stop the latency release thread as needed
        if (enabled && is_capturing_.load() && !latency_thread_.joinable()) {
            latency_thread_ = std::jthread(&NetworkCapture::LatencyReleaseThread, this);
        }
    }

    bool NetworkCapture::IsLatencyEnabled() const {
        return latency_module_->IsEnabled();
    }

    void NetworkCapture::SetLatency(uint32_t latency_ms) {
        latency_module_->SetLatency(latency_ms);
    }

    uint32_t NetworkCapture::GetLatency() const {
        return latency_module_->GetLatency();
    }

    void NetworkCapture::SetLatencyInbound(bool enabled) {
        latency_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetLatencyOutbound(bool enabled) {
        latency_module_->SetOutboundEnabled(enabled);
    }

    // Packet Loss control methods
    void NetworkCapture::SetPacketLossEnabled(bool enabled) {
        packet_loss_module_->SetEnabled(enabled);
    }

    bool NetworkCapture::IsPacketLossEnabled() const {
        return packet_loss_module_->IsEnabled();
    }

    void NetworkCapture::SetPacketLossRate(float loss_percentage) {
        packet_loss_module_->SetLossRate(loss_percentage);
    }

    float NetworkCapture::GetPacketLossRate() const {
        return packet_loss_module_->GetLossRate();
    }

    void NetworkCapture::SetPacketLossInbound(bool enabled) {
        packet_loss_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetPacketLossOutbound(bool enabled) {
        packet_loss_module_->SetOutboundEnabled(enabled);
    }

    // Duplicate control methods
    void NetworkCapture::SetDuplicateEnabled(bool enabled) {
        duplicate_module_->SetEnabled(enabled);
    }

    bool NetworkCapture::IsDuplicateEnabled() const {
        return duplicate_module_->IsEnabled();
    }

    void NetworkCapture::SetDuplicateRate(float duplicate_percentage) {
        duplicate_module_->SetDuplicationRate(duplicate_percentage);
    }

    float NetworkCapture::GetDuplicateRate() const {
        return duplicate_module_->GetDuplicationRate();
    }

    void NetworkCapture::SetDuplicateCount(uint32_t count) {
        duplicate_module_->SetDuplicateCount(count);
    }

    uint32_t NetworkCapture::GetDuplicateCount() const {
        return duplicate_module_->GetDuplicateCount();
    }

    void NetworkCapture::SetDuplicateInbound(bool enabled) {
        duplicate_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetDuplicateOutbound(bool enabled) {
        duplicate_module_->SetOutboundEnabled(enabled);
    }

    // Out of Order control methods
    void NetworkCapture::SetOutOfOrderEnabled(bool enabled) {
        out_of_order_module_->SetEnabled(enabled);
    }

    bool NetworkCapture::IsOutOfOrderEnabled() const {
        return out_of_order_module_->IsEnabled();
    }

    void NetworkCapture::SetOutOfOrderRate(float reorder_percentage) {
        out_of_order_module_->SetReorderRate(reorder_percentage);
    }

    float NetworkCapture::GetOutOfOrderRate() const {
        return out_of_order_module_->GetReorderRate();
    }

    void NetworkCapture::SetReorderGap(uint32_t gap) {
        out_of_order_module_->SetReorderGap(gap);
    }

    uint32_t NetworkCapture::GetReorderGap() const {
        return out_of_order_module_->GetReorderGap();
    }

    void NetworkCapture::SetOutOfOrderInbound(bool enabled) {
        out_of_order_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetOutOfOrderOutbound(bool enabled) {
        out_of_order_module_->SetOutboundEnabled(enabled);
    }

    // Jitter control methods
    void NetworkCapture::SetJitterEnabled(bool enabled) {
        jitter_module_->SetEnabled(enabled);

        // Start or stop the jitter release thread as needed
        if (enabled && is_capturing_.load() && !jitter_thread_.joinable()) {
            jitter_thread_ = std::jthread(&NetworkCapture::JitterReleaseThread, this);
        }
    }

    bool NetworkCapture::IsJitterEnabled() const {
        return jitter_module_->IsEnabled();
    }

    void NetworkCapture::SetJitterRange(uint32_t min_ms, uint32_t max_ms) {
        jitter_module_->SetJitterRange(min_ms, max_ms);
    }

    uint32_t NetworkCapture::GetJitterMin() const {
        return jitter_module_->GetMinJitter();
    }

    uint32_t NetworkCapture::GetJitterMax() const {
        return jitter_module_->GetMaxJitter();
    }

    void NetworkCapture::SetJitterInbound(bool enabled) {
        jitter_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetJitterOutbound(bool enabled) {
        jitter_module_->SetOutboundEnabled(enabled);
    }

    // Bandwidth control methods
    void NetworkCapture::SetBandwidthEnabled(bool enabled) {
        bandwidth_module_->SetEnabled(enabled);

        // Start or stop the bandwidth release thread as needed
        if (enabled && is_capturing_.load() && !bandwidth_thread_.joinable()) {
            bandwidth_thread_ = std::jthread(&NetworkCapture::BandwidthReleaseThread, this);
        }
    }

    bool NetworkCapture::IsBandwidthEnabled() const {
        return bandwidth_module_->IsEnabled();
    }

    void NetworkCapture::SetBandwidthLimit(uint32_t kbps) {
        bandwidth_module_->SetBandwidthLimit(kbps);
    }

    uint32_t NetworkCapture::GetBandwidthLimit() const {
        return bandwidth_module_->GetBandwidthLimit();
    }

    void NetworkCapture::SetBandwidthInbound(bool enabled) {
        bandwidth_module_->SetInboundEnabled(enabled);
    }

    void NetworkCapture::SetBandwidthOutbound(bool enabled) {
        bandwidth_module_->SetOutboundEnabled(enabled);
    }

    // Runtime parameter methods
    bool NetworkCapture::SetQueueLength(uint64_t length) {
        if (divert_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        bool result = WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_LENGTH, length);
        if (result) {
            std::lock_guard<std::mutex> lock(params_mutex_);
            current_params_.queue_length = length;
        }
        return result;
    }

    bool NetworkCapture::SetQueueTime(uint64_t time_ms) {
        if (divert_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        bool result = WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_TIME, time_ms);
        if (result) {
            std::lock_guard<std::mutex> lock(params_mutex_);
            current_params_.queue_time = time_ms;
        }
        return result;
    }

    bool NetworkCapture::SetQueueSize(uint64_t size) {
        if (divert_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        bool result = WinDivertSetParam(divert_handle_, WINDIVERT_PARAM_QUEUE_SIZE, size);
        if (result) {
            std::lock_guard<std::mutex> lock(params_mutex_);
            current_params_.queue_size = size;
        }
        return result;
    }

    CaptureParameters NetworkCapture::GetParameters() const {
        std::lock_guard<std::mutex> lock(params_mutex_);
        return current_params_;
    }

    NetworkCapture::VersionInfo NetworkCapture::GetDriverVersion() const {
        VersionInfo info = { 0, 0 };

        if (divert_handle_ != INVALID_HANDLE_VALUE) {
            WinDivertGetParam(divert_handle_, WINDIVERT_PARAM_VERSION_MAJOR, &info.major);
            WinDivertGetParam(divert_handle_, WINDIVERT_PARAM_VERSION_MINOR, &info.minor);
        }

        return info;
    }

    std::vector<PacketInfo> NetworkCapture::GetPackets() {
        std::lock_guard<std::mutex> lock(packets_mutex_);
        std::vector<PacketInfo> result(packets_.begin(), packets_.end());
        packets_.clear();
        return result;
    }

    NetworkCapture::Stats NetworkCapture::GetStats() const {
        Stats stats{};
        stats.packets_captured = packets_captured_.load();
        stats.packets_dropped = packets_dropped_.load();
        stats.packets_injected = packets_injected_.load();
        stats.bytes_captured = bytes_captured_.load();
        stats.batch_count = batch_count_.load();

        uint64_t total_packets = total_batch_packets_.load();
        uint64_t batches = batch_count_.load();
        stats.avg_batch_size = (batches > 0) ?
            static_cast<double>(total_packets) / batches : 0.0;

        return stats;
    }

    void NetworkCapture::CaptureThreadBatch() {
        CaptureParameters params;
        {
            std::lock_guard<std::mutex> lock(params_mutex_);
            params = current_params_;
        }

        // Allocate batch buffers
        const uint32_t batch_size = params.batch_size;
        std::vector<uint8_t> packet_buffer(params.packet_buffer_size);
        std::vector<WINDIVERT_ADDRESS> addr_buffer(batch_size);

        while (!should_stop_.load()) {
            UINT recv_len = 0;
            UINT addr_len = static_cast<UINT>(sizeof(WINDIVERT_ADDRESS) * batch_size);

            // Receive batch of packets using WinDivertRecvEx
            if (!WinDivertRecvEx(divert_handle_,
                packet_buffer.data(),
                static_cast<UINT>(packet_buffer.size()),
                &recv_len,
                0,  // flags
                addr_buffer.data(),
                &addr_len,
                nullptr)) {  // No overlapped I/O for now

                DWORD error = ::GetLastError();

                // Check if we're stopping
                if (should_stop_.load() || error == ERROR_NO_DATA) {
                    break;
                }

                // Handle other errors
                if (error != ERROR_NO_DATA) {
                    SetError(std::format("WinDivertRecvEx failed: {}", error));
                }
                continue;
            }

            // Calculate number of packets received
            UINT num_packets = addr_len / sizeof(WINDIVERT_ADDRESS);
            if (num_packets == 0) continue;

            // Update batch statistics
            batch_count_.fetch_add(1);
            total_batch_packets_.fetch_add(num_packets);

            // Convert received packets to SimulatedPackets
            std::vector<SimulatedPacket> sim_packets;
            sim_packets.reserve(num_packets);

            const uint8_t* packet_ptr = packet_buffer.data();
            UINT bytes_processed = 0;
            const auto current_time = std::chrono::steady_clock::now();

            for (UINT i = 0; i < num_packets; ++i) {
                // Parse packet headers to get length
                WINDIVERT_IPHDR* ip_header = nullptr;
                WINDIVERT_IPV6HDR* ipv6_header = nullptr;

                WinDivertHelperParsePacket(packet_ptr, recv_len - bytes_processed,
                    &ip_header, &ipv6_header,
                    nullptr, nullptr, nullptr,
                    nullptr, nullptr,
                    nullptr, nullptr, nullptr, nullptr);

                UINT packet_len = 0;
                if (ip_header != nullptr) {
                    packet_len = ntohs(ip_header->Length);
                }
                else if (ipv6_header != nullptr) {
                    packet_len = ntohs(ipv6_header->Length) + 40;  // IPv6 header is 40 bytes
                }

                if (packet_len > 0) {
                    // Store packet info for monitoring
                    PacketInfo info = ParsePacket(
                        std::span<const uint8_t>(packet_ptr, packet_len),
                        addr_buffer[i]
                    );
                    {
                        std::lock_guard<std::mutex> lock(packets_mutex_);
                        packets_.push_back(info);
                        if (packets_.size() > max_packets_) {
                            packets_.pop_front();
                            packets_dropped_.fetch_add(1);
                        }
                    }

                    // Create SimulatedPacket for processing
                    SimulatedPacket sim_packet;
                    sim_packet.data.assign(packet_ptr, packet_ptr + packet_len);
                    sim_packet.addr = addr_buffer[i];
                    sim_packet.timestamp = current_time;
                    sim_packets.push_back(std::move(sim_packet));

                    // Update statistics
                    packets_captured_.fetch_add(1);
                    bytes_captured_.fetch_add(packet_len);

                    // Move to next packet
                    packet_ptr += packet_len;
                    bytes_processed += packet_len;
                }
            }

            // Apply simulation effects in order
            // 1. Packet loss (drops packets)
            if (packet_loss_module_->IsEnabled()) {
                sim_packets = packet_loss_module_->ProcessBatch(std::move(sim_packets));
            }

            // 2. Duplicate (creates duplicates)
            if (duplicate_module_->IsEnabled()) {
                sim_packets = duplicate_module_->ProcessBatch(std::move(sim_packets));
            }

            // 3. Out of order (reorders packets)
            if (out_of_order_module_->IsEnabled()) {
                sim_packets = out_of_order_module_->ProcessBatch(std::move(sim_packets));
            }

            // 4. Jitter (adds variable delay)
            if (jitter_module_->IsEnabled()) {
                sim_packets = jitter_module_->ProcessBatch(std::move(sim_packets));
            }

            // 5. Bandwidth limiting (rate limits)
            if (bandwidth_module_->IsEnabled()) {
                sim_packets = bandwidth_module_->ProcessBatch(std::move(sim_packets));
            }

            // 6. Latency (adds fixed delay)
            if (latency_module_->IsEnabled()) {
                sim_packets = latency_module_->ProcessBatch(std::move(sim_packets));
            }

            // Send packets that aren't delayed
            if (!sim_packets.empty()) {
                // Calculate total size needed
                size_t total_bytes = 0;
                for (const auto& sim_packet : sim_packets) {
                    total_bytes += sim_packet.data.size();
                }

                // Rebuild packet buffer for sending with proper reservation
                std::vector<uint8_t> send_buffer;
                std::vector<WINDIVERT_ADDRESS> send_addrs;
                send_buffer.reserve(total_bytes);
                send_addrs.reserve(sim_packets.size());

                for (const auto& sim_packet : sim_packets) {
                    send_buffer.insert(send_buffer.end(),
                        sim_packet.data.begin(),
                        sim_packet.data.end());
                    send_addrs.push_back(sim_packet.addr);
                }

                UINT send_len = 0;
                if (WinDivertSendEx(divert_handle_,
                    send_buffer.data(),
                    static_cast<UINT>(send_buffer.size()),
                    &send_len,
                    0,  // flags
                    send_addrs.data(),
                    static_cast<UINT>(send_addrs.size() * sizeof(WINDIVERT_ADDRESS)),
                    nullptr)) {
                    packets_injected_.fetch_add(sim_packets.size());
                }
            }
        }
    }

    void NetworkCapture::LatencyReleaseThread() {
        using namespace std::chrono_literals;

        while (!should_stop_.load()) {
            // Check every 10ms for packets ready to be released
            std::this_thread::sleep_for(10ms);

            auto releasable = latency_module_->GetReleasablePackets();
            if (!releasable.empty() && divert_handle_ != INVALID_HANDLE_VALUE) {
                // Send released packets
                std::vector<uint8_t> send_buffer;
                std::vector<WINDIVERT_ADDRESS> send_addrs;

                for (const auto& packet : releasable) {
                    send_buffer.insert(send_buffer.end(),
                        packet.data.begin(),
                        packet.data.end());
                    send_addrs.push_back(packet.addr);
                }

                UINT send_len = 0;
                WinDivertSendEx(divert_handle_,
                    send_buffer.data(),
                    static_cast<UINT>(send_buffer.size()),
                    &send_len,
                    0,
                    send_addrs.data(),
                    static_cast<UINT>(send_addrs.size() * sizeof(WINDIVERT_ADDRESS)),
                    nullptr);

                packets_injected_.fetch_add(releasable.size());
            }
        }
    }

    void NetworkCapture::JitterReleaseThread() {
        using namespace std::chrono_literals;

        while (!should_stop_.load()) {
            // Check every 10ms for packets ready to be released
            std::this_thread::sleep_for(10ms);

            auto releasable = jitter_module_->GetReleasablePackets();
            if (!releasable.empty() && divert_handle_ != INVALID_HANDLE_VALUE) {
                // Send released packets
                std::vector<uint8_t> send_buffer;
                std::vector<WINDIVERT_ADDRESS> send_addrs;

                for (const auto& packet : releasable) {
                    send_buffer.insert(send_buffer.end(),
                        packet.data.begin(),
                        packet.data.end());
                    send_addrs.push_back(packet.addr);
                }

                UINT send_len = 0;
                WinDivertSendEx(divert_handle_,
                    send_buffer.data(),
                    static_cast<UINT>(send_buffer.size()),
                    &send_len,
                    0,
                    send_addrs.data(),
                    static_cast<UINT>(send_addrs.size() * sizeof(WINDIVERT_ADDRESS)),
                    nullptr);

                packets_injected_.fetch_add(releasable.size());
            }
        }
    }

    void NetworkCapture::BandwidthReleaseThread() {
        using namespace std::chrono_literals;

        while (!should_stop_.load()) {
            // Check every 10ms for packets ready to be released
            std::this_thread::sleep_for(10ms);

            auto releasable = bandwidth_module_->GetReleasablePackets();
            if (!releasable.empty() && divert_handle_ != INVALID_HANDLE_VALUE) {
                // Send released packets
                std::vector<uint8_t> send_buffer;
                std::vector<WINDIVERT_ADDRESS> send_addrs;

                for (const auto& packet : releasable) {
                    send_buffer.insert(send_buffer.end(),
                        packet.data.begin(),
                        packet.data.end());
                    send_addrs.push_back(packet.addr);
                }

                UINT send_len = 0;
                WinDivertSendEx(divert_handle_,
                    send_buffer.data(),
                    static_cast<UINT>(send_buffer.size()),
                    &send_len,
                    0,
                    send_addrs.data(),
                    static_cast<UINT>(send_addrs.size() * sizeof(WINDIVERT_ADDRESS)),
                    nullptr);

                packets_injected_.fetch_add(releasable.size());
            }
        }
    }

    PacketInfo NetworkCapture::ParsePacket(std::span<const uint8_t> packet_data,
        const WINDIVERT_ADDRESS& addr) {
        PacketInfo info = {};
        info.length = static_cast<uint32_t>(packet_data.size());
        info.timestamp = std::chrono::steady_clock::now();
        info.outbound = addr.Outbound ? true : false;
        info.loopback = addr.Loopback ? true : false;
        info.if_idx = addr.Network.IfIdx;

        // Parse headers using WinDivert helpers
        WINDIVERT_IPHDR* ip_header = nullptr;
        WINDIVERT_IPV6HDR* ipv6_header = nullptr;
        WINDIVERT_TCPHDR* tcp_header = nullptr;
        WINDIVERT_UDPHDR* udp_header = nullptr;

        WinDivertHelperParsePacket(packet_data.data(),
            static_cast<UINT>(packet_data.size()),
            &ip_header, &ipv6_header,
            nullptr, nullptr, nullptr, &tcp_header, &udp_header,
            nullptr, nullptr, nullptr, nullptr);

        // Extract IP information
        if (ip_header != nullptr) {
            // IPv4
            info.ip_version = 4;
            info.src_addr = IPv4Address{ ntohl(ip_header->SrcAddr) };
            info.dst_addr = IPv4Address{ ntohl(ip_header->DstAddr) };
            info.protocol = ip_header->Protocol;
        }
        else if (ipv6_header != nullptr) {
            // IPv6
            info.ip_version = 6;
            IPv6Address src{}, dst{};

            // Convert from network byte order
            for (int i = 0; i < 4; ++i) {
                src.addr[i] = ntohl(ipv6_header->SrcAddr[i]);
                dst.addr[i] = ntohl(ipv6_header->DstAddr[i]);
            }

            info.src_addr = src;
            info.dst_addr = dst;
            info.protocol = ipv6_header->NextHdr;
        }

        // Extract port information if TCP or UDP
        if (tcp_header != nullptr) {
            info.src_port = ntohs(tcp_header->SrcPort);
            info.dst_port = ntohs(tcp_header->DstPort);
        }
        else if (udp_header != nullptr) {
            info.src_port = ntohs(udp_header->SrcPort);
            info.dst_port = ntohs(udp_header->DstPort);
        }

        return info;
    }

    void NetworkCapture::SetError(const std::string& error) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error;
    }

}