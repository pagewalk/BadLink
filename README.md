# BadLink

Network condition testing tool for Windows. Deliberately degrade your network to test how applications handle poor connectivity. Inspired by [Clumsy](https://jagt.github.io/clumsy/) but built with performance, control & customization in mind. 

## Features

BadLink intercepts network packets at the kernel level using WinDivert and can simulate various network problems:
| Feature | Description | Range/Options |
|---------|-------------|---------------|
| Packet Loss | Drop random packets | 0-100% |
| Latency | Adds a fixed delay to packets | 0-5000 ms |
| Bandwidth Limiting | Uses token bucket throttling to limit bandwidth | 56kbps to 100Mbps |
| Packet Duplication | Clone Packets | 1-5 copies |
| Out of Order Delivery | Shuffle packet order | Configurable gap |
| Jitter | Adds a variable delay to packets | Separate min/max ms |

## Screenshots:
<img width="1414" height="704" alt="2025-09-09_16-12" src="https://github.com/user-attachments/assets/ffaa2a60-43f9-46cd-822f-3ca56a1ce7ee" />


## Built with

Written in C++ using Visual Studio 2022 IDE with C++ Desktop Development
- [WinDivert 2.2](https://reqrypt.org/windivert.html) for the packet interception driver
- [imgui](https://github.com/ocornut/imgui) + DirectX 12 for the UI and hardware rendering
- [tomlplusplus](https://github.com/marzer/tomlplusplus) for the configuration file

## Requirements

- Windows 10/11 (64/32-bit)
- Administrator privileges (required by WinDivert driver)
- Visual C++ Redistributables 2022
- DirectX 12 compatible GPU

## Building from source

Requirements:
- Visual Studio 2022 with C++ Desktop Development
- Windows SDK 10.0+

```bash
git clone https://github.com/pagewalk/badlink.git
cd badlink
# Open BadLink.sln in Visual Studio 2022
# Build in Release x64
```

## Dependencies

All dependencies are included in the `external/` folder:
- WinDivert 2.2.2 driver, libraries and headers
- imgui with DirectX 12
- tomlplusplus header-only library

## Usage

1. Run `badlink.exe` as Administrator
2. Configure network conditions in the GUI:
   - Enable desired conditions (latency, loss, etc.)
   - Set parameters for each condition
   - Choose direction (inbound/outbound/both)
3. Set WinDivert packet filter (default "true" captures all)
4. Click "Start Capture" or use configured hotkey
5. Your network will behave as configured
6. Click "Stop Capture" to restore normal network

### Preset Filters

| Filter | Description | Use Case |
|--------|-------------|----------|
| `true` | All traffic | Capture everything |
| `tcp` | Only TCP packets | TCP protocol analysis |
| `udp` | Only UDP packets | UDP protocol analysis |
| `udp.DstPort == 53` | Only DNS queries | DNS monitoring/blocking |
| `ip.DstAddr >= 192.168.0.0 and ip.DstAddr <= 192.168.255.255` | Local network only | LAN traffic isolation |
| `tcp.DstPort == 443` | HTTPS traffic only | SSL/TLS traffic filtering |
| `outbound` | Only outgoing traffic | Monitor outbound connections |
| `inbound` | Only inbound traffic | Monitor inbound connections |

See [WinDivert Filter Language](https://reqrypt.org/windivert-doc.html#filter_language) for full syntax and how to construct your own filters.

## Configuration

BadLink saves settings to a `badlink.toml` file in the applications current directory, these settings include:
- WinDivert specific parameters (queue size, timeout, etc.)
- Performance tuning (worker threads, batch size)
- Filter presets
- Hotkey configuration

## Known Issues

- Some games anti-cheat systems can flag the WinDivert driver
- Requires admin privileges (WinDivert limitation)
- Windows Defender may flag as suspicious (packet interception behavior)
- WinDivert issues and limitations can be found here: [WinDivert: Known Issues](https://github.com/basil00/WinDivert/wiki/WinDivert-Documentation#known_issues)

## Third-party libraries

This project includes the following third-party libraries in the `external/` folder:

- **WinDivert** - LGPL v3 or GPL v2 © basil  
  Windows kernel driver for packet capture/injection  
  https://github.com/basil00/Divert

- **Dear ImGui** - MIT License © Omar Cornut  
  Immediate mode graphical user interface  
  https://github.com/ocornut/imgui

- **tomlplusplus** - MIT License © Mark Gillard  
  Header-only TOML parser and serializer  
  https://github.com/marzer/tomlplusplus