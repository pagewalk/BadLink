#ifndef BADLINK_SRC_NETWORK_CAPTURE_H_
#define BADLINK_SRC_NETWORK_CAPTURE_H_

#include <windivert.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <cstdint>
#include <variant>
#include <array>
#include <memory>
#include <expected>
#include <optional>
#include <chrono>
#include <span>

namespace BadLink {

    // Forward declarations
    class LatencyModule;
    class PacketLossModule;
    class DuplicateModule;
    class OutOfOrderModule;
    class JitterModule;
    class BandwidthModule;
    struct SimulatedPacket;

    // Configuration constants with defaults
    struct ConfigConstants {
        // Network constants
        static constexpr uint32_t DEFAULT_MTU_SIZE = 1500;              // Standard Ethernet MTU
        static constexpr uint32_t DEFAULT_MAX_PACKET_SIZE = 65535;      // Max IP packet size

        // WinDivert parameters defaults
        static constexpr uint64_t DEFAULT_QUEUE_LENGTH = 8192;          // Min: 32, Max: 16384
        static constexpr uint64_t DEFAULT_QUEUE_TIME = 2000;
        static constexpr uint64_t DEFAULT_QUEUE_SIZE = 4194304;         // Min: 65535, Max: 33554432 (4MB)

        // Performance parameters defaults
        static constexpr uint32_t DEFAULT_BATCH_SIZE = 10;              // 1-255
        static constexpr uint32_t DEFAULT_WORKER_THREADS = 1;           // 1-8
        static constexpr uint32_t DEFAULT_PACKET_BUFFER_SIZE = 16384;   // Must be large enough for any valid packet
        static constexpr size_t DEFAULT_VISUAL_PACKET_BUFFER = 1000;    // UI display limit
        static constexpr size_t DEFAULT_RING_PACKET_BUFFER = 1024;      // Internal ring buffer
    };

    // IPv4 and IPv6 address storage
    struct IPv4Address {
        uint32_t addr;
        std::string ToString() const;
    };

    struct IPv6Address {
        std::array<uint32_t, 4> addr;
        std::string ToString() const;
    };

    using IPAddress = std::variant<IPv4Address, IPv6Address>;

    struct PacketInfo {
        IPAddress   src_addr;       // Source IP (IPv4 or IPv6)
        IPAddress   dst_addr;       // Destination IP
        uint16_t    src_port;       // Source port (if TCP/UDP)
        uint16_t    dst_port;       // Destination port (if TCP/UDP)
        uint8_t     protocol;       // IPPROTO_TCP, IPPROTO_UDP, etc.
        uint32_t    length;         // Packet length
        std::chrono::steady_clock::time_point timestamp;  // Capture timestamp
        bool        outbound;       // Direction flag
        bool        loopback;       // Loopback flag
        uint32_t    if_idx;         // Interface index
        uint64_t    endpoint_id;    // Endpoint ID
        uint8_t     ip_version;     // 4 or 6
    };

    // WinDivert runtime parameters
    struct CaptureParameters {
        // WinDivert queue parameters
        uint64_t queue_length = ConfigConstants::DEFAULT_QUEUE_LENGTH;
        uint64_t queue_time = ConfigConstants::DEFAULT_QUEUE_TIME;
        uint64_t queue_size = ConfigConstants::DEFAULT_QUEUE_SIZE;

        // Performance parameters
        uint32_t batch_size = ConfigConstants::DEFAULT_BATCH_SIZE;
        uint32_t worker_threads = ConfigConstants::DEFAULT_WORKER_THREADS;
        uint32_t packet_buffer_size = ConfigConstants::DEFAULT_PACKET_BUFFER_SIZE;

        // Buffer management
        size_t visual_packet_buffer = ConfigConstants::DEFAULT_VISUAL_PACKET_BUFFER;
        size_t ring_packet_buffer = ConfigConstants::DEFAULT_RING_PACKET_BUFFER;

        // Network parameters
        uint32_t mtu_size = ConfigConstants::DEFAULT_MTU_SIZE;
        uint32_t max_packet_size = ConfigConstants::DEFAULT_MAX_PACKET_SIZE;
    };

    class NetworkCapture {
    public:
        NetworkCapture();
        ~NetworkCapture();

        // Start capturing with specified filter and parameters
        std::expected<void, std::string> Start(const std::string& filter = "true",
            const CaptureParameters& params = {});

        // Stop capturing
        void Stop();

        // Simulation control methods - Latency
        void SetLatencyEnabled(bool enabled);
        bool IsLatencyEnabled() const;
        void SetLatency(uint32_t latency_ms);
        uint32_t GetLatency() const;
        void SetLatencyInbound(bool enabled);
        void SetLatencyOutbound(bool enabled);

        // Simulation control methods - Packet Loss
        void SetPacketLossEnabled(bool enabled);
        bool IsPacketLossEnabled() const;
        void SetPacketLossRate(float loss_percentage);
        float GetPacketLossRate() const;
        void SetPacketLossInbound(bool enabled);
        void SetPacketLossOutbound(bool enabled);

        // Simulation control methods - Duplicate
        void SetDuplicateEnabled(bool enabled);
        bool IsDuplicateEnabled() const;
        void SetDuplicateRate(float duplicate_percentage);
        float GetDuplicateRate() const;
        void SetDuplicateCount(uint32_t count);
        uint32_t GetDuplicateCount() const;
        void SetDuplicateInbound(bool enabled);
        void SetDuplicateOutbound(bool enabled);

        // Simulation control methods - Out of Order
        void SetOutOfOrderEnabled(bool enabled);
        bool IsOutOfOrderEnabled() const;
        void SetOutOfOrderRate(float reorder_percentage);
        float GetOutOfOrderRate() const;
        void SetReorderGap(uint32_t gap);
        uint32_t GetReorderGap() const;
        void SetOutOfOrderInbound(bool enabled);
        void SetOutOfOrderOutbound(bool enabled);

        // Simulation control methods - Jitter
        void SetJitterEnabled(bool enabled);
        bool IsJitterEnabled() const;
        void SetJitterRange(uint32_t min_ms, uint32_t max_ms);
        uint32_t GetJitterMin() const;
        uint32_t GetJitterMax() const;
        void SetJitterInbound(bool enabled);
        void SetJitterOutbound(bool enabled);

        // Simulation control methods - Bandwidth
        void SetBandwidthEnabled(bool enabled);
        bool IsBandwidthEnabled() const;
        void SetBandwidthLimit(uint32_t kbps);
        uint32_t GetBandwidthLimit() const;
        void SetBandwidthInbound(bool enabled);
        void SetBandwidthOutbound(bool enabled);

        // Runtime parameter adjustment
        bool SetQueueLength(uint64_t length);
        bool SetQueueTime(uint64_t time_ms);
        bool SetQueueSize(uint64_t size);

        // Get current parameters
        CaptureParameters GetParameters() const;

        // Set max packets for ring buffer
        void SetMaxPackets(size_t max) {
            std::lock_guard<std::mutex> lock(packets_mutex_);
            max_packets_ = max;
        }

        // Get WinDivert version info
        struct VersionInfo {
            uint64_t major;
            uint64_t minor;
        };
        VersionInfo GetDriverVersion() const;

        // Check if currently capturing
        bool IsCapturing() const { return is_capturing_.load(); }

        // Get and clear captured packets (for UI display)
        std::vector<PacketInfo> GetPackets();

        // Get last error message
        std::optional<std::string> GetLastErrorMessage() const {
            std::lock_guard<std::mutex> lock(error_mutex_);
            return last_error_.empty() ? std::nullopt : std::optional(last_error_);
        }

        // Get statistics
        struct Stats {
            uint64_t packets_captured;
            uint64_t packets_dropped;
            uint64_t packets_injected;
            uint64_t bytes_captured;
            uint64_t batch_count;       // Number of batch operations
            double   avg_batch_size;    // Average packets per batch
        };
        Stats GetStats() const;

    private:
        // Batch capture thread function
        void CaptureThreadBatch();

        // Release threads for time-based modules
        void LatencyReleaseThread();
        void JitterReleaseThread();
        void BandwidthReleaseThread();

        // Parse single packet from batch
        PacketInfo ParsePacket(std::span<const uint8_t> packet_data,
            const WINDIVERT_ADDRESS& addr);

        // Thread management
        std::atomic<bool> is_capturing_{ false };
        std::atomic<bool> should_stop_{ false };
        std::vector<std::jthread> capture_threads_;
        std::jthread latency_thread_;
        std::jthread jitter_thread_;
        std::jthread bandwidth_thread_;

        // WinDivert handle
        HANDLE divert_handle_ = INVALID_HANDLE_VALUE;

        // Current parameters
        CaptureParameters current_params_;
        mutable std::mutex params_mutex_;

        // Packet buffer (thread-safe)
        std::mutex packets_mutex_;
        std::deque<PacketInfo> packets_;
        size_t max_packets_ = ConfigConstants::DEFAULT_RING_PACKET_BUFFER;

        // Statistics
        std::atomic<uint64_t> packets_captured_{ 0 };
        std::atomic<uint64_t> packets_dropped_{ 0 };
        std::atomic<uint64_t> packets_injected_{ 0 };
        std::atomic<uint64_t> bytes_captured_{ 0 };
        std::atomic<uint64_t> batch_count_{ 0 };
        std::atomic<uint64_t> total_batch_packets_{ 0 };

        // Error handling
        mutable std::mutex error_mutex_;
        std::string last_error_;

        // Simulation modules
        std::unique_ptr<LatencyModule> latency_module_;
        std::unique_ptr<PacketLossModule> packet_loss_module_;
        std::unique_ptr<DuplicateModule> duplicate_module_;
        std::unique_ptr<OutOfOrderModule> out_of_order_module_;
        std::unique_ptr<JitterModule> jitter_module_;
        std::unique_ptr<BandwidthModule> bandwidth_module_;

        void SetError(const std::string& error);
    };

}
#endif  // BADLINK_SRC_NETWORK_CAPTURE_H_