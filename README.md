# NetMon — Lightweight Windows Network Monitor

A tiny, zero-overhead system-tray widget that shows the real-time state of every network adapter on your PC. Written in pure C with the Win32 API — single binary, no runtime, no dependencies.

## Why

The Windows "Network Connections" panel is slow to open and only refreshes when you ask it to. NetMon gives you instant, always-available visibility:

- See which adapters are connected, disconnected, or disabled at a glance.
- Cable yanked? The status flips to **DISCONNECTED** the moment Windows notices — no polling, no refresh button.
- Hover the tray icon for a one-line summary, click it for the full widget.

## Features

- **Real-time state** — uses `NotifyIpInterfaceChange` so the OS pushes events the moment any interface goes up or down. Zero polling.
- **Per-adapter detail** — IPv4, CIDR prefix, subnet mask, DHCP/Static, link speed, default gateway, DNS servers, MAC, IPv6.
- **Wi-Fi enrichment** — SSID, signal strength (0-100%), frequency band (2.4 / 5 / 6 GHz). Icon arcs light up by signal level.
- **Live throughput** — per-connected-adapter download/upload rate in MB/s, sampled at 1 Hz only while the widget is open.
- **Public IP** — fetched in the background via WinHTTP, displayed in the header.
- **Tray tooltip** — primary connection summary without opening the widget.
- **Click to copy** — click any IP / MAC / SSID / gateway / DNS field to copy it to the clipboard.
- **ncpa.cpl-accurate filtering** — uses the same hidden-adapter rules (`NCF_HIDDEN`, `*NdisDeviceType`, NDIS filter binding detection) as the Windows control panel, so you see exactly what `ncpa.cpl` shows — no ghost or filter-layer junk.
- **Show / hide disabled adapters** with a fixed bottom toggle.
- **Mouse-wheel scrolling** for long adapter lists.
- **Dark theme**, double-buffered GDI rendering, DWM rounded corners (Windows 11).
- **Single-instance** via named mutex.
- **Tray-only** — no taskbar entry, never steals focus.
- Tiny: ~150 KB EXE, a few hundred KB resident.

## Installation

Download `NetMon_Setup.exe` from the [Releases page](../../releases) and run it. The installer:

- Copies `netmon.exe` to `Program Files\NetMon`
- Adds inbound/outbound Windows Firewall exceptions
- Creates a Start Menu shortcut
- Registers a "Start with Windows" entry in `HKCU\...\Run`
- Adds an Add/Remove Programs entry with a clean uninstaller

After install, open the tray icon — left-click toggles the widget, right-click opens the menu.

## Building from source

Requires Visual Studio 2022 (any edition). From a regular PowerShell prompt:

```powershell
.\compile.ps1
```

Or from a *Developer Command Prompt for VS 2022*:

```cmd
compile_msvc.bat
```

To rebuild the installer (requires [NSIS](https://nsis.sourceforge.io/)):

```powershell
& "C:\Program Files (x86)\NSIS\makensis.exe" installer.nsi
```

## Project layout

| File | Purpose |
|------|---------|
| `netmon.c` | Entire application — single source file (~1500 lines) |
| `netmon.rc` / `netmon.manifest` | Version info + application manifest |
| `compile.ps1` | PowerShell build script (preferred — handles MSVC paths with spaces) |
| `compile_msvc.bat` | Same build, from a VS Developer prompt |
| `build.bat` | MinGW / gcc fallback build |
| `installer.nsi` | NSIS installer script |

## Architecture notes

- **No polling.** `NotifyIpInterfaceChange` registers an OS callback that fires the moment any IP interface state changes. The only timer is the 1 Hz throughput sampler, which is started on widget show and killed on hide — when the widget is in the tray, the process is fully event-driven.
- **Adapter visibility.** Real adapters are isolated from NDIS filter binding layers (QoS, Npcap, WFP) via `MIB_IF_ROW2.InterfaceAndOperStatusFlags.FilterInterface`, and from hidden adapters (WAN miniports, kernel debugger, Wi-Fi Direct, vSwitch extensions) via the `NCF_HIDDEN` / `*NdisDeviceType` characteristics in the network driver class registry key. Ghost adapters with no `Connection` subkey are excluded.
- **Wi-Fi info.** Pulled via the WLAN API (`WlanOpenHandle` / `WlanQueryInterface`) and matched to the IP-adapter list by interface GUID.
- **Public IP fetch.** Spawned as a one-shot background thread on widget show / network change. 5-second timeout, never blocks the UI thread.

## License

Copyright © 2026 Ibrahim Mohammed. All rights reserved.
