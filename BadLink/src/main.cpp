#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <tchar.h>
#include <memory>
#include <vector>
#include <format>
#include <variant>
#include <string>

#include "windivert.h"
#include "config.h"

#include "network_capture.h"

namespace BadLink {
    constexpr int NUM_FRAMES_IN_FLIGHT = 2;
    constexpr int NUM_BACK_BUFFERS = 2;
    constexpr int SRV_HEAP_SIZE = 64;
    constexpr const wchar_t* WINDOW_TITLE = L"BadLink - Network condition testing tool for Windows";
    constexpr int DEFAULT_WIDTH = 1440;
    constexpr int DEFAULT_HEIGHT = 1080;
}

struct FrameContext
{
    ID3D12CommandAllocator* CommandAllocator;
    UINT64                   FenceValue;
};

struct ExampleDescriptorHeapAllocator
{
    ID3D12DescriptorHeap* Heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
    UINT                        HeapHandleIncrement;
    ImVector<int>               FreeIndices;

    void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
    {
        IM_ASSERT(Heap == nullptr && FreeIndices.empty());
        Heap = heap;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        HeapType = desc.Type;
        HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
        HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
        HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
        FreeIndices.reserve((int)desc.NumDescriptors);
        for (int n = desc.NumDescriptors; n > 0; n--)
            FreeIndices.push_back(n - 1);
    }
    void Destroy()
    {
        Heap = nullptr;
        FreeIndices.clear();
    }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
    {
        IM_ASSERT(FreeIndices.Size > 0);
        int idx = FreeIndices.back();
        FreeIndices.pop_back();
        out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
        out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
    {
        int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
        int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
        IM_ASSERT(cpu_idx == gpu_idx);
        FreeIndices.push_back(cpu_idx);
    }
};

// DX12 stuff - mostly from ImGui examples, don't touch unless you know what you're doing
static FrameContext                 g_frameContext[BadLink::NUM_FRAMES_IN_FLIGHT] = {};
static UINT                         g_frameIndex = 0;
static ID3D12Device* g_pd3dDevice = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static ExampleDescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE                       g_fenceEvent = nullptr;
static UINT64                       g_fenceLastSignaledValue = 0;
static IDXGISwapChain3* g_pSwapChain = nullptr;
static bool                         g_SwapChainOccluded = false;
static HANDLE                       g_hSwapChainWaitableObject = nullptr;
static ID3D12Resource* g_mainRenderTargetResource[BadLink::NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE  g_mainRenderTargetDescriptor[BadLink::NUM_BACK_BUFFERS] = {};

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void WaitForLastSubmittedFrame();
FrameContext* WaitForNextFrameResources();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct WinDivertStatus {
    bool dll_loaded = false;
    bool driver_available = false;
    std::string error_message;
    HANDLE test_handle = INVALID_HANDLE_VALUE;
    uint32_t driver_major = 0;
    uint32_t driver_minor = 0;
};

struct ApplicationState {
    // Window visibility
    bool show_control_panel = true;
    bool show_capture_window = true;
    bool show_packet_table = true;

    // UI colors
    ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.12f, 1.00f);

    // Auto-scroll for packet table
    bool packet_table_auto_scroll = true;

    // Network capture
    std::unique_ptr<BadLink::NetworkCapture> capture;
    std::vector<BadLink::PacketInfo> packets;
    char filter_buffer[256] = "true";
    std::string capture_error;

    // Configuration
    BadLink::Config::Configuration config;
    bool config_dirty = false;

    // Filter management
    int selected_preset = -1;

    // Hotkey management
    bool capturing_hotkey = false;
    bool pending_ctrl = false;
    bool pending_shift = false;
    bool pending_alt = false;

    // Simulation settings
    struct SimulationSettings {
        // Packet Loss
        bool packet_loss_enabled = false;
        bool packet_loss_inbound = true;
        bool packet_loss_outbound = true;
        float packet_loss_rate = 0.0f;

        // Latency
        bool latency_enabled = false;
        bool latency_inbound = true;
        bool latency_outbound = true;
        int latency_ms = 0;

        // Packet Duplication
        bool duplicate_enabled = false;
        bool duplicate_inbound = true;
        bool duplicate_outbound = true;
        float duplicate_rate = 0.0f;
        int duplicate_count = 1;

        // Out of Order
        bool out_of_order_enabled = false;
        bool out_of_order_inbound = true;
        bool out_of_order_outbound = true;
        float out_of_order_rate = 0.0f;
        int reorder_gap = 3;

        // Jitter
        bool jitter_enabled = false;
        bool jitter_inbound = true;
        bool jitter_outbound = true;
        int jitter_min_ms = 0;
        int jitter_max_ms = 50;

        // Bandwidth Limiting
        bool bandwidth_enabled = false;
        bool bandwidth_inbound = true;
        bool bandwidth_outbound = true;
        int bandwidth_kbps = 1000;
    } simulation;
};

static WinDivertStatus CheckWinDivertStatus() {
    WinDivertStatus status;
    status.dll_loaded = true;  // If we got here, the DLL loaded

    // Try to open a simple test filter
    status.test_handle = WinDivertOpen("false", WINDIVERT_LAYER_NETWORK, 0, 0);

    if (status.test_handle == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        status.driver_available = false;

        switch (lastError) {
        case ERROR_ACCESS_DENIED:
            status.error_message = "Access denied. Run as Administrator.";
            break;
        case ERROR_SERVICE_DOES_NOT_EXIST:
            status.error_message = "WinDivert driver not installed.";
            break;
        case ERROR_FILE_NOT_FOUND:
            status.error_message = "WinDivert driver files not found.";
            break;
        default:
            status.error_message = std::format("Failed to initialize WinDivert. Error: {}", lastError);
            break;
        }
    }
    else {
        status.driver_available = true;
        status.error_message = "WinDivert ready";

        // Get driver version
        UINT64 major = 0, minor = 0;
        WinDivertGetParam(status.test_handle, WINDIVERT_PARAM_VERSION_MAJOR, &major);
        WinDivertGetParam(status.test_handle, WINDIVERT_PARAM_VERSION_MINOR, &minor);
        status.driver_major = static_cast<uint32_t>(major);
        status.driver_minor = static_cast<uint32_t>(minor);

        WinDivertClose(status.test_handle);
        status.test_handle = INVALID_HANDLE_VALUE;
    }

    return status;
}

static void ToggleCapture(ApplicationState& state) {
    bool is_capturing = state.capture && state.capture->IsCapturing();

    if (!is_capturing) {
        // Start capture
        if (!state.capture) {
            state.capture = std::make_unique<BadLink::NetworkCapture>();
        }

        auto result = state.capture->Start(state.filter_buffer, state.config.params);
        if (!result.has_value()) {
            auto error = state.capture->GetLastErrorMessage();
            state.capture_error = error.value_or("Unknown error");
        }
        else {
            state.capture_error.clear();

            // Apply current simulation settings
            state.capture->SetPacketLossEnabled(state.simulation.packet_loss_enabled);
            state.capture->SetPacketLossRate(state.simulation.packet_loss_rate);
            state.capture->SetPacketLossInbound(state.simulation.packet_loss_inbound);
            state.capture->SetPacketLossOutbound(state.simulation.packet_loss_outbound);

            state.capture->SetLatencyEnabled(state.simulation.latency_enabled);
            state.capture->SetLatency(state.simulation.latency_ms);
            state.capture->SetLatencyInbound(state.simulation.latency_inbound);
            state.capture->SetLatencyOutbound(state.simulation.latency_outbound);

            state.capture->SetDuplicateEnabled(state.simulation.duplicate_enabled);
            state.capture->SetDuplicateRate(state.simulation.duplicate_rate);
            state.capture->SetDuplicateCount(state.simulation.duplicate_count);
            state.capture->SetDuplicateInbound(state.simulation.duplicate_inbound);
            state.capture->SetDuplicateOutbound(state.simulation.duplicate_outbound);

            state.capture->SetOutOfOrderEnabled(state.simulation.out_of_order_enabled);
            state.capture->SetOutOfOrderRate(state.simulation.out_of_order_rate);
            state.capture->SetReorderGap(state.simulation.reorder_gap);
            state.capture->SetOutOfOrderInbound(state.simulation.out_of_order_inbound);
            state.capture->SetOutOfOrderOutbound(state.simulation.out_of_order_outbound);

            state.capture->SetJitterEnabled(state.simulation.jitter_enabled);
            state.capture->SetJitterRange(state.simulation.jitter_min_ms, state.simulation.jitter_max_ms);
            state.capture->SetJitterInbound(state.simulation.jitter_inbound);
            state.capture->SetJitterOutbound(state.simulation.jitter_outbound);

            state.capture->SetBandwidthEnabled(state.simulation.bandwidth_enabled);
            state.capture->SetBandwidthLimit(state.simulation.bandwidth_kbps);
            state.capture->SetBandwidthInbound(state.simulation.bandwidth_inbound);
            state.capture->SetBandwidthOutbound(state.simulation.bandwidth_outbound);
        }
    }
    else {
        // Stop capture
        state.capture->Stop();
        state.packets.clear();
        state.capture_error.clear();
    }
}

static void RenderControlPanel(ApplicationState& state, const WinDivertStatus& divert_status) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(450, 700), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Control Panel", &state.show_control_panel, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Configuration")) {
                BadLink::Config::Save(state.config);
                state.config_dirty = false;
            }
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Packet Monitor", nullptr, &state.show_packet_table);
            ImGui::MenuItem("Network Capture", nullptr, &state.show_capture_window);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // WinDivert Status Section
    if (ImGui::CollapsingHeader("WinDivert Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("DLL Loaded:");
        ImGui::SameLine(150);
        ImGui::TextColored(divert_status.dll_loaded ?
            ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
            divert_status.dll_loaded ? "Yes" : "No");

        ImGui::Text("Driver Available:");
        ImGui::SameLine(150);
        if (divert_status.driver_available) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Yes (v%d.%d)",
                divert_status.driver_major, divert_status.driver_minor);
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No");
        }

        ImGui::Text("Status:");
        ImGui::SameLine(150);
        ImGui::TextColored(divert_status.driver_available ?
            ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
            "%s", divert_status.error_message.c_str());
    }

    ImGui::Separator();

    // Hotkey Configuration
    if (ImGui::CollapsingHeader("Hotkey Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Hotkey", &state.config.capture_hotkey.enabled);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            state.config_dirty = true;
        }

        ImGui::Text("Current Hotkey:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s",
            state.config.capture_hotkey.ToString().c_str());

        if (state.capturing_hotkey) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                state.capturing_hotkey = false;
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::Text("Press any key combination...");

            // Capture key combination
            ImGuiIO& io = ImGui::GetIO();

            // Check modifiers
            state.pending_ctrl = io.KeyCtrl;
            state.pending_shift = io.KeyShift;
            state.pending_alt = io.KeyAlt;

            // Check for key press (excluding modifiers)
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
                if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                    // Skip standalone modifier keys
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
                        key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                        key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
                        key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) {
                        continue;
                    }

                    // Set the new hotkey
                    state.config.capture_hotkey.key = (ImGuiKey)key;
                    state.config.capture_hotkey.ctrl = state.pending_ctrl;
                    state.config.capture_hotkey.shift = state.pending_shift;
                    state.config.capture_hotkey.alt = state.pending_alt;
                    state.config_dirty = true;
                    state.capturing_hotkey = false;
                    break;
                }
            }

            // Allow Escape to cancel
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.capturing_hotkey = false;
            }
        }
        else {
            if (ImGui::Button("Set Hotkey", ImVec2(100, 0))) {
                state.capturing_hotkey = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                state.config.capture_hotkey.key = ImGuiKey_None;
                state.config.capture_hotkey.ctrl = false;
                state.config.capture_hotkey.shift = false;
                state.config.capture_hotkey.alt = false;
                state.config_dirty = true;
            }
        }

        if (state.config.capture_hotkey.enabled) {
            ImGui::TextWrapped("The hotkey will toggle capture on/off when pressed.");
            if (!divert_status.driver_available) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                    "Note: Hotkey won't work until WinDivert is available");
            }
        }
    }

    ImGui::Separator();

    // WinDivert Parameters
    if (ImGui::CollapsingHeader("WinDivert Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool is_capturing = state.capture && state.capture->IsCapturing();

        int queue_length = static_cast<int>(state.config.params.queue_length);
        if (ImGui::SliderInt("Queue Length", &queue_length, 32, 16384)) {
            state.config.params.queue_length = queue_length;
            state.config_dirty = true;
            if (is_capturing) {
                state.capture->SetQueueLength(queue_length);
            }
        }

        int queue_time = static_cast<int>(state.config.params.queue_time);
        if (ImGui::SliderInt("Queue Time (ms)", &queue_time, 100, 16000)) {
            state.config.params.queue_time = queue_time;
            state.config_dirty = true;
            if (is_capturing) {
                state.capture->SetQueueTime(queue_time);
            }
        }

        float queue_size_mb = state.config.params.queue_size / (1024.0f * 1024.0f);
        if (ImGui::SliderFloat("Queue Size (MB)", &queue_size_mb, 0.065f, 32.0f)) {
            state.config.params.queue_size = static_cast<UINT64>(queue_size_mb * 1024 * 1024);
            state.config_dirty = true;
            if (is_capturing) {
                state.capture->SetQueueSize(state.config.params.queue_size);
            }
        }
    }

    ImGui::Separator();

    // Performance Parameters
    if (ImGui::CollapsingHeader("Performance Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        int batch_size = static_cast<int>(state.config.params.batch_size);
        if (ImGui::SliderInt("Batch Size", &batch_size, 1, 255)) {
            state.config.params.batch_size = batch_size;
            state.config_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Requires restart");

        int worker_threads = static_cast<int>(state.config.params.worker_threads);
        if (ImGui::SliderInt("Worker Threads", &worker_threads, 1, 8)) {
            state.config.params.worker_threads = worker_threads;
            state.config_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Requires restart");

        int packet_buffer_kb = state.config.params.packet_buffer_size / 1024;
        if (ImGui::SliderInt("Packet Buffer (KB)", &packet_buffer_kb, 1, 128)) {
            state.config.params.packet_buffer_size = packet_buffer_kb * 1024;
            state.config_dirty = true;
        }

        int visual_buffer = static_cast<int>(state.config.params.visual_packet_buffer);
        if (ImGui::SliderInt("Visual Buffer", &visual_buffer, 100, 5000)) {
            state.config.params.visual_packet_buffer = visual_buffer;
            state.config_dirty = true;
        }

        int ring_buffer = static_cast<int>(state.config.params.ring_packet_buffer);
        if (ImGui::SliderInt("Ring Buffer", &ring_buffer, 1000, 50000)) {
            state.config.params.ring_packet_buffer = ring_buffer;
            state.config_dirty = true;
            if (state.capture) {
                state.capture->SetMaxPackets(ring_buffer);
            }
        }
    }

    ImGui::Separator();

    // Network Parameters
    if (ImGui::CollapsingHeader("Network Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mtu = static_cast<int>(state.config.params.mtu_size);
        if (ImGui::SliderInt("MTU Size", &mtu, 576, 9000)) {
            state.config.params.mtu_size = mtu;
            state.config_dirty = true;
        }

        int max_packet = static_cast<int>(state.config.params.max_packet_size);
        if (ImGui::SliderInt("Max Packet Size", &max_packet, 1500, 65535)) {
            state.config.params.max_packet_size = max_packet;
            state.config_dirty = true;
        }
    }

    ImGui::Separator();

    // Configuration Management
    if (ImGui::CollapsingHeader("Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Config File: %s", BadLink::Config::CONFIG_FILE);

        if (state.config_dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[Unsaved]");
        }

        if (ImGui::Button("Save Configuration", ImVec2(-1, 0))) {
            if (BadLink::Config::Save(state.config)) {
                state.config_dirty = false;
            }
        }

        if (ImGui::Button("Reload Configuration", ImVec2(-1, 0))) {
            if (BadLink::Config::Load(state.config)) {
                state.config_dirty = false;
            }
        }

        if (ImGui::Button("Reset to Defaults", ImVec2(-1, 0))) {
            state.config.params = BadLink::CaptureParameters{};
            state.config.filter_presets = BadLink::Config::GetDefaultPresets();
            state.config.capture_hotkey = BadLink::Config::HotkeyConfig{};
            state.config_dirty = true;
        }
    }

    ImGui::Separator();

    // Performance Statistics
    if (ImGui::CollapsingHeader("Performance Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (state.capture) {
            auto stats = state.capture->GetStats();

            ImGui::Text("Packets Captured: %llu", stats.packets_captured);
            ImGui::Text("Packets Dropped: %llu", stats.packets_dropped);
            ImGui::Text("Packets Injected: %llu", stats.packets_injected);
            ImGui::Text("Bytes Captured: %llu", stats.bytes_captured);
            ImGui::Text("Batch Operations: %llu", stats.batch_count);
            ImGui::Text("Avg Batch Size: %.2f packets", stats.avg_batch_size);
        }
        else {
            ImGui::TextDisabled("No capture session");
        }
    }

    ImGui::End();
}

static void RenderCaptureWindow(ApplicationState& state, const WinDivertStatus& divert_status) {
    ImGui::SetNextWindowPos(ImVec2(470, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(960, 380), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Network Capture & Simulation", &state.show_capture_window,
        ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!divert_status.driver_available) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
            "Capture unavailable: check Control Panel for WinDivert status");
        ImGui::End();
        return;
    }

    // Capture Controls Section
    if (ImGui::CollapsingHeader("Capture Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Filter controls
        ImGui::Text("Filter:");
        ImGui::SameLine();

        float available_width = ImGui::GetContentRegionAvail().x;
        float button_width = 100.0f;
        float preset_width = 150.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float filter_width = available_width - button_width - preset_width - (spacing * 4);

        ImGui::SetNextItemWidth(filter_width);
        ImGui::InputText("##Filter", state.filter_buffer, sizeof(state.filter_buffer));

        // Filter presets
        ImGui::SameLine();
        ImGui::SetNextItemWidth(preset_width);

        const char* current_preset_name = "Presets...";
        if (state.selected_preset >= 0 && state.selected_preset < state.config.filter_presets.size()) {
            current_preset_name = state.config.filter_presets[state.selected_preset].name.c_str();
        }

        if (ImGui::BeginCombo("##Presets", current_preset_name)) {
            for (int i = 0; i < state.config.filter_presets.size(); ++i) {
                bool is_selected = (state.selected_preset == i);
                if (ImGui::Selectable(state.config.filter_presets[i].name.c_str(), is_selected)) {
                    state.selected_preset = i;
                    strcpy_s(state.filter_buffer, sizeof(state.filter_buffer),
                        state.config.filter_presets[i].filter.c_str());
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Start/Stop button with hotkey hint
        ImGui::SameLine();
        bool is_capturing = state.capture && state.capture->IsCapturing();
        if (!is_capturing) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
            if (ImGui::Button("Start Capture", ImVec2(button_width, 0))) {
                ToggleCapture(state);
            }
            // Show hotkey tooltip
            if (ImGui::IsItemHovered() && state.config.capture_hotkey.enabled &&
                state.config.capture_hotkey.key != ImGuiKey_None) {
                ImGui::SetTooltip("Hotkey: %s", state.config.capture_hotkey.ToString().c_str());
            }
            ImGui::PopStyleColor(2);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Stop Capture", ImVec2(button_width, 0))) {
                ToggleCapture(state);
            }
            // Show hotkey tooltip
            if (ImGui::IsItemHovered() && state.config.capture_hotkey.enabled &&
                state.config.capture_hotkey.key != ImGuiKey_None) {
                ImGui::SetTooltip("Hotkey: %s", state.config.capture_hotkey.ToString().c_str());
            }
            ImGui::PopStyleColor(2);
        }

        // Status indicators
        if (is_capturing) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[CAPTURING]");
            ImGui::SameLine();
            auto stats = state.capture->GetStats();
            ImGui::Text("Captured: %llu | Dropped: %llu",
                stats.packets_captured, stats.packets_dropped);

            // Show hotkey hint if enabled
            if (state.config.capture_hotkey.enabled && state.config.capture_hotkey.key != ImGuiKey_None) {
                ImGui::SameLine();
                ImGui::TextDisabled("(Press %s to stop)", state.config.capture_hotkey.ToString().c_str());
            }
        }
        else {
            // Show hotkey hint if enabled and not capturing
            if (state.config.capture_hotkey.enabled && state.config.capture_hotkey.key != ImGuiKey_None) {
                ImGui::TextDisabled("Press %s to start capture", state.config.capture_hotkey.ToString().c_str());
            }
        }

        if (!state.capture_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                "Error: %s", state.capture_error.c_str());
        }
    }

    ImGui::Separator();

    // Network Simulation Controls
    if (ImGui::CollapsingHeader("Network Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool is_capturing = state.capture && state.capture->IsCapturing();

        // Packet Loss
        ImGui::Text("Packet Loss:");
        ImGui::PushID("PacketLoss");
        ImGui::Checkbox("Enable", &state.simulation.packet_loss_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetPacketLossEnabled(state.simulation.packet_loss_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.packet_loss_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("##Rate", &state.simulation.packet_loss_rate, 0.0f, 100.0f, "%.1f%%");
        if (is_capturing && state.capture) {
            state.capture->SetPacketLossRate(state.simulation.packet_loss_rate);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.packet_loss_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetPacketLossInbound(state.simulation.packet_loss_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.packet_loss_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetPacketLossOutbound(state.simulation.packet_loss_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        // Latency
        ImGui::Text("Latency:");
        ImGui::PushID("Latency");
        ImGui::Checkbox("Enable", &state.simulation.latency_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetLatencyEnabled(state.simulation.latency_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.latency_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::SliderInt("##Delay", &state.simulation.latency_ms, 0, 5000, "%d ms");
        if (is_capturing && state.capture) {
            state.capture->SetLatency(state.simulation.latency_ms);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.latency_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetLatencyInbound(state.simulation.latency_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.latency_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetLatencyOutbound(state.simulation.latency_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        // Packet Duplication
        ImGui::Text("Duplicate Packets:");
        ImGui::PushID("Duplicate");
        ImGui::Checkbox("Enable", &state.simulation.duplicate_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetDuplicateEnabled(state.simulation.duplicate_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.duplicate_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("##DupRate", &state.simulation.duplicate_rate, 0.0f, 100.0f, "%.1f%%");
        if (is_capturing && state.capture) {
            state.capture->SetDuplicateRate(state.simulation.duplicate_rate);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("##Count", &state.simulation.duplicate_count, 1, 5, "%d");
        if (is_capturing && state.capture) {
            state.capture->SetDuplicateCount(state.simulation.duplicate_count);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of duplicate copies");
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.duplicate_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetDuplicateInbound(state.simulation.duplicate_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.duplicate_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetDuplicateOutbound(state.simulation.duplicate_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        // Out of Order
        ImGui::Text("Out of Order:");
        ImGui::PushID("OutOfOrder");
        ImGui::Checkbox("Enable", &state.simulation.out_of_order_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetOutOfOrderEnabled(state.simulation.out_of_order_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.out_of_order_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("##ReorderRate", &state.simulation.out_of_order_rate, 0.0f, 100.0f, "%.1f%%");
        if (is_capturing && state.capture) {
            state.capture->SetOutOfOrderRate(state.simulation.out_of_order_rate);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("##Gap", &state.simulation.reorder_gap, 2, 10, "%d");
        if (is_capturing && state.capture) {
            state.capture->SetReorderGap(state.simulation.reorder_gap);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Buffer size before reordering");
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.out_of_order_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetOutOfOrderInbound(state.simulation.out_of_order_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.out_of_order_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetOutOfOrderOutbound(state.simulation.out_of_order_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        // Jitter
        ImGui::Text("Network Jitter:");
        ImGui::PushID("Jitter");
        ImGui::Checkbox("Enable", &state.simulation.jitter_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetJitterEnabled(state.simulation.jitter_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.jitter_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragInt("##MinJitter", &state.simulation.jitter_min_ms, 1.0f, 0, 1000, "%d ms min");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragInt("##MaxJitter", &state.simulation.jitter_max_ms, 1.0f, 0, 5000, "%d ms max");
        if (is_capturing && state.capture) {
            state.capture->SetJitterRange(state.simulation.jitter_min_ms,
                state.simulation.jitter_max_ms);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.jitter_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetJitterInbound(state.simulation.jitter_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.jitter_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetJitterOutbound(state.simulation.jitter_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        // Bandwidth Limiting
        ImGui::Text("Bandwidth Limit:");
        ImGui::PushID("Bandwidth");
        ImGui::Checkbox("Enable", &state.simulation.bandwidth_enabled);
        if (is_capturing && state.capture) {
            state.capture->SetBandwidthEnabled(state.simulation.bandwidth_enabled);
        }

        ImGui::BeginDisabled(!state.simulation.bandwidth_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("##Bandwidth", &state.simulation.bandwidth_kbps, 56, 100000, "%d kbps");
        if (is_capturing && state.capture) {
            state.capture->SetBandwidthLimit(state.simulation.bandwidth_kbps);
        }
        if (ImGui::IsItemHovered()) {
            float mbps = state.simulation.bandwidth_kbps / 1000.0f;
            ImGui::SetTooltip("%.2f Mbps", mbps);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Inbound", &state.simulation.bandwidth_inbound);
        if (is_capturing && state.capture) {
            state.capture->SetBandwidthInbound(state.simulation.bandwidth_inbound);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Outbound", &state.simulation.bandwidth_outbound);
        if (is_capturing && state.capture) {
            state.capture->SetBandwidthOutbound(state.simulation.bandwidth_outbound);
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        ImGui::Separator();

        // Simulation Status Summary
        if (is_capturing) {
            ImGui::Text("Active Simulations:");
            int active_count = 0;

            if (state.simulation.packet_loss_enabled) {
                ImGui::BulletText("Packet Loss: %.1f%% (%s%s%s)",
                    state.simulation.packet_loss_rate,
                    state.simulation.packet_loss_inbound ? "IN" : "",
                    (state.simulation.packet_loss_inbound && state.simulation.packet_loss_outbound) ? "/" : "",
                    state.simulation.packet_loss_outbound ? "OUT" : "");
                active_count++;
            }
            if (state.simulation.latency_enabled) {
                ImGui::BulletText("Latency: %d ms (%s%s%s)%s",
                    state.simulation.latency_ms,
                    state.simulation.latency_inbound ? "IN" : "",
                    (state.simulation.latency_inbound&& state.simulation.latency_outbound) ? "/" : "",
                    state.simulation.latency_outbound ? "OUT" : "");
                active_count++;
            }
            if (state.simulation.duplicate_enabled) {
                ImGui::BulletText("Duplicate: %.1f%% x%d (%s%s%s)",
                    state.simulation.duplicate_rate,
                    state.simulation.duplicate_count,
                    state.simulation.duplicate_inbound ? "IN" : "",
                    (state.simulation.duplicate_inbound && state.simulation.duplicate_outbound) ? "/" : "",
                    state.simulation.duplicate_outbound ? "OUT" : "");
                active_count++;
            }
            if (state.simulation.out_of_order_enabled) {
                ImGui::BulletText("Out of Order: %.1f%% gap:%d (%s%s%s)",
                    state.simulation.out_of_order_rate,
                    state.simulation.reorder_gap,
                    state.simulation.out_of_order_inbound ? "IN" : "",
                    (state.simulation.out_of_order_inbound && state.simulation.out_of_order_outbound) ? "/" : "",
                    state.simulation.out_of_order_outbound ? "OUT" : "");
                active_count++;
            }
            if (state.simulation.jitter_enabled) {
                ImGui::BulletText("Jitter: %d-%d ms (%s%s%s)",
                    state.simulation.jitter_min_ms,
                    state.simulation.jitter_max_ms,
                    state.simulation.jitter_inbound ? "IN" : "",
                    (state.simulation.jitter_inbound && state.simulation.jitter_outbound) ? "/" : "",
                    state.simulation.jitter_outbound ? "OUT" : "");
                active_count++;
            }
            if (state.simulation.bandwidth_enabled) {
                ImGui::BulletText("Bandwidth: %d kbps (%s%s%s)",
                    state.simulation.bandwidth_kbps,
                    state.simulation.bandwidth_inbound ? "IN" : "",
                    (state.simulation.bandwidth_inbound && state.simulation.bandwidth_outbound) ? "/" : "",
                    state.simulation.bandwidth_outbound ? "OUT" : "");
                active_count++;
            }

            if (active_count == 0) {
                ImGui::TextDisabled("No simulations active");
            }
        }
        else {
            ImGui::TextDisabled("Start capture to apply simulation settings");
        }
    }

    ImGui::End();
}

void RenderPacketTable(ApplicationState& state) {
    ImGui::SetNextWindowPos(ImVec2(470, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(960, 490), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Packet Monitor", &state.show_packet_table,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    // Menu bar for packet table specific actions
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Actions")) {
            if (ImGui::MenuItem("Clear All Packets")) {
                state.packets.clear();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Auto-scroll", nullptr, &state.packet_table_auto_scroll);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Status bar
    auto stats = state.capture ? state.capture->GetStats() : BadLink::NetworkCapture::Stats{};
    ImGui::Text("Total Captured: %llu | Dropped: %llu | Buffer: %zu/%zu | Displaying: %zu packets",
        stats.packets_captured, stats.packets_dropped,
        state.packets.size(), state.config.params.visual_packet_buffer,
        state.packets.size());

    ImGui::Separator();

    // Packet table
    float avail_height = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginChild("PacketTableChild", ImVec2(0, avail_height), true)) {
        if (ImGui::BeginTable("PacketTable", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingFixedFit)) {

            // Setup columns with better defaults
            ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Proto", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupScrollFreeze(0, 1);  // Freeze header row
            ImGui::TableHeadersRow();

            // Render packet rows
            for (const auto& packet : state.packets) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (packet.loopback)
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 1.0f, 1.0f), "LOOP");
                else
                    ImGui::Text(packet.outbound ? "OUT" : "IN");

                ImGui::TableSetColumnIndex(1);
                const char* proto_name = "OTHER";
                if (packet.protocol == 6) proto_name = "TCP";
                else if (packet.protocol == 17) proto_name = "UDP";
                else if (packet.protocol == 1) proto_name = "ICMP";
                else if (packet.protocol == 58) proto_name = "ICMPv6";
                ImGui::Text("%s", proto_name);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("v%d", packet.ip_version);

                ImGui::TableSetColumnIndex(3);
                std::string src_str = std::visit([](const auto& addr) {
                    return addr.ToString();
                    }, packet.src_addr);
                ImGui::TextUnformatted(src_str.c_str());

                ImGui::TableSetColumnIndex(4);
                std::string dst_str = std::visit([](const auto& addr) {
                    return addr.ToString();
                    }, packet.dst_addr);
                ImGui::TextUnformatted(dst_str.c_str());

                ImGui::TableSetColumnIndex(5);
                if (packet.src_port > 0 || packet.dst_port > 0)
                    ImGui::Text("%d->%d", packet.src_port, packet.dst_port);
                else
                    ImGui::Text("-");

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%d", packet.length);
            }

            // Auto-scroll, doesn't work
            if (state.packet_table_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

static void RenderUI(ApplicationState& state, const WinDivertStatus& divert_status) {
    if (state.show_control_panel) {
        RenderControlPanel(state, divert_status);
    }

    if (state.show_capture_window) {
        RenderCaptureWindow(state, divert_status);
    }

    if (state.show_packet_table) {
        RenderPacketTable(state);
    }

    // Update captured packets if capturing
    if (state.capture && state.capture->IsCapturing()) {
        auto new_packets = state.capture->GetPackets();
        state.packets.insert(state.packets.end(), new_packets.begin(), new_packets.end());

        // Keep only last N packets for UI performance
        if (state.packets.size() > state.config.params.visual_packet_buffer) {
            state.packets.erase(state.packets.begin(),
                state.packets.begin() + (state.packets.size() - state.config.params.visual_packet_buffer));
        }
    }
}

static void CheckHotkey(ApplicationState& state, const WinDivertStatus& divert_status) {
    if (!state.config.capture_hotkey.enabled || state.capturing_hotkey) {
        return;
    }

    // Check if the hotkey is pressed
    bool key_pressed = ImGui::IsKeyPressed(state.config.capture_hotkey.key, false);
    bool ctrl_match = state.config.capture_hotkey.ctrl == ImGui::GetIO().KeyCtrl;
    bool shift_match = state.config.capture_hotkey.shift == ImGui::GetIO().KeyShift;
    bool alt_match = state.config.capture_hotkey.alt == ImGui::GetIO().KeyAlt;

    if (key_pressed && ctrl_match && shift_match && alt_match) {
        // Only toggle if WinDivert is available
        if (divert_status.driver_available) {
            ToggleCapture(state);
        }
    }
}

// Main function
int main(int, char**)
{
    // DPI awareness
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr,
                       nullptr, L"BadLink", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, BadLink::WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW, 100, 100,
        (int)(BadLink::DEFAULT_WIDTH * main_scale),
        (int)(BadLink::DEFAULT_HEIGHT * main_scale),
        nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Initialize ImGui backends
    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = g_pd3dDevice;
    init_info.CommandQueue = g_pd3dCommandQueue;
    init_info.NumFramesInFlight = BadLink::NUM_FRAMES_IN_FLIGHT;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = g_pd3dSrvDescHeap;
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) {
        return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle);
        };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
        return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle);
        };
    ImGui_ImplDX12_Init(&init_info);

    // Application state
    ApplicationState app_state;

    // Load configuration including presets
    if (!BadLink::Config::Load(app_state.config)) {
        // Create default config file with default presets
        BadLink::Config::CreateDefault();
        // Load the defaults
        app_state.config.filter_presets = BadLink::Config::GetDefaultPresets();
    }

    // Check WinDivert status at startup
    WinDivertStatus divert_status = CheckWinDivertStatus();

    // Main loop
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window minimized/occluded
        if ((g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd))
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Start ImGui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Check for hotkey press
        CheckHotkey(app_state, divert_status);

        // Render application UI
        RenderUI(app_state, divert_status);

        // Rendering
        ImGui::Render();

        FrameContext* frameCtx = WaitForNextFrameResources();
        UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
        frameCtx->CommandAllocator->Reset();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);

        const float clear_color_with_alpha[4] = {
            app_state.clear_color.x * app_state.clear_color.w,
            app_state.clear_color.y * app_state.clear_color.w,
            app_state.clear_color.z * app_state.clear_color.w,
            app_state.clear_color.w
        };
        g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
        g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();

        g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        UINT64 fenceValue = g_fenceLastSignaledValue + 1;
        g_pd3dCommandQueue->Signal(g_fence, fenceValue);
        g_fenceLastSignaledValue = fenceValue;
        frameCtx->FenceValue = fenceValue;
    }

    WaitForLastSubmittedFrame();

    // Save configuration if there are unsaved changes
    if (app_state.config_dirty) {
        BadLink::Config::Save(app_state.config);
    }

    // Cleanup
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC1 sd;
    {
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = BadLink::NUM_BACK_BUFFERS;
        sd.Width = 0;
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;
    }

#ifdef DX12_ENABLE_DEBUG_LAYER
    ID3D12Debug* pdx12Debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
        pdx12Debug->EnableDebugLayer();
#endif

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    if (D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&g_pd3dDevice)) != S_OK)
        return false;

#ifdef DX12_ENABLE_DEBUG_LAYER
    if (pdx12Debug != nullptr)
    {
        ID3D12InfoQueue* pInfoQueue = nullptr;
        g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        pInfoQueue->Release();
        pdx12Debug->Release();
    }
#endif

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = BadLink::NUM_BACK_BUFFERS;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
            return false;

        SIZE_T rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < BadLink::NUM_BACK_BUFFERS; i++)
        {
            g_mainRenderTargetDescriptor[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = BadLink::SRV_HEAP_SIZE;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
            return false;
        g_pd3dSrvDescHeapAlloc.Create(g_pd3dDevice, g_pd3dSrvDescHeap);
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK)
            return false;
    }

    for (UINT i = 0; i < BadLink::NUM_FRAMES_IN_FLIGHT; i++)
        if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
            return false;

    if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
        g_pd3dCommandList->Close() != S_OK)
        return false;

    if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
        return false;

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fenceEvent == nullptr)
        return false;

    {
        IDXGIFactory4* dxgiFactory = nullptr;
        IDXGISwapChain1* swapChain1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
            return false;
        if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK)
            return false;
        if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK)
            return false;
        swapChain1->Release();
        dxgiFactory->Release();
        g_pSwapChain->SetMaximumFrameLatency(BadLink::NUM_BACK_BUFFERS);
        g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->SetFullscreenState(false, nullptr); g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_hSwapChainWaitableObject != nullptr) { CloseHandle(g_hSwapChainWaitableObject); }
    for (UINT i = 0; i < BadLink::NUM_FRAMES_IN_FLIGHT; i++)
        if (g_frameContext[i].CommandAllocator) { g_frameContext[i].CommandAllocator->Release(); g_frameContext[i].CommandAllocator = nullptr; }
    if (g_pd3dCommandQueue) { g_pd3dCommandQueue->Release(); g_pd3dCommandQueue = nullptr; }
    if (g_pd3dCommandList) { g_pd3dCommandList->Release(); g_pd3dCommandList = nullptr; }
    if (g_pd3dRtvDescHeap) { g_pd3dRtvDescHeap->Release(); g_pd3dRtvDescHeap = nullptr; }
    if (g_pd3dSrvDescHeap) { g_pd3dSrvDescHeap->Release(); g_pd3dSrvDescHeap = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }

#ifdef DX12_ENABLE_DEBUG_LAYER
    IDXGIDebug1* pDebug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
    {
        pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
        pDebug->Release();
    }
#endif
}

void CreateRenderTarget()
{
    for (UINT i = 0; i < BadLink::NUM_BACK_BUFFERS; i++)
    {
        ID3D12Resource* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
        g_mainRenderTargetResource[i] = pBackBuffer;
    }
}

void CleanupRenderTarget()
{
    WaitForLastSubmittedFrame();

    for (UINT i = 0; i < BadLink::NUM_BACK_BUFFERS; i++)
        if (g_mainRenderTargetResource[i]) { g_mainRenderTargetResource[i]->Release(); g_mainRenderTargetResource[i] = nullptr; }
}

void WaitForLastSubmittedFrame()
{
    FrameContext* frameCtx = &g_frameContext[g_frameIndex % BadLink::NUM_FRAMES_IN_FLIGHT];

    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue == 0)
        return;

    frameCtx->FenceValue = 0;
    if (g_fence->GetCompletedValue() >= fenceValue)
        return;

    g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameResources()
{
    UINT nextFrameIndex = g_frameIndex + 1;
    g_frameIndex = nextFrameIndex;

    HANDLE waitableObjects[] = { g_hSwapChainWaitableObject, nullptr };
    DWORD numWaitableObjects = 1;

    FrameContext* frameCtx = &g_frameContext[nextFrameIndex % BadLink::NUM_FRAMES_IN_FLIGHT];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue != 0)
    {
        frameCtx->FenceValue = 0;
        g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
        waitableObjects[1] = g_fenceEvent;
        numWaitableObjects = 2;
    }

    WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);

    return frameCtx;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            WaitForLastSubmittedFrame();
            CleanupRenderTarget();
            HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            assert(SUCCEEDED(result) && "Failed to resize swapchain.");
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}