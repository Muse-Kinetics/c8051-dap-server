# SiLabs 8051 Debug

**GitHub:** https://github.com/KMIMusic/C8051_dap_server  
**Author:** Eric Bateman — [Muse Kinetics](https://www.musekinetics.com)  
**License:** MIT

VSCode debug adapter for **Silicon Laboratories C8051F-series MCUs** via the
[Silicon Labs 8-Bit USB Debug Adapter](https://www.silabs.com/development-tools/mcu/8-bit/8-bit-usb-debug-adapter?tab=overview).

Drives the `SiC8051F.dll` AGDI library that ships with Keil µVision, exposing it as a
standard [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server. Press **F5** to build, flash, and debug directly from VSCode.

---

## Requirements

- Windows 10 / 11
- **Keil µVision** installed (C51 toolchain) — required for `UV4.exe` (build) and the two
  adapter DLLs (`SiC8051F.dll`, `USBHID.dll`) which are copied automatically on first use
- Silicon Laboratories [**8-Bit USB Debug Adapter**](https://www.silabs.com/development-tools/mcu/8-bit/8-bit-usb-debug-adapter?tab=overview) connected to the target board
- Target: C8051F-series MCU (tested: C8051F380 / EFM8UB20F64G)

---

## First-time setup

1. Install this extension from the VSIX:
   **Extensions panel** (`Ctrl+Shift+X`) → `...` menu → **Install from VSIX…**

2. On the first debug session the extension will:
   - Locate your Keil installation automatically via the Windows registry
   - Copy `SiC8051F.dll` and `USBHID.dll` from your Keil folder into the extension's `bin\`
   - Start the DAP server in the background

   If Keil is not at a standard path, set it in settings:
   ```json
   // .vscode/settings.json
   { "silabs8051.keilPath": "C:\\path\\to\\Keil_v5" }
   ```

---

## Per-project setup

Open (or create) `.vscode/launch.json` in your firmware project, click
**Add Configuration…** at the bottom-right, and choose one of:

| Snippet | What it does |
|---------|-------------|
| **SiLabs 8051: Flash + Debug** | Build → flash → open a live debug session |
| **SiLabs 8051: Flash Only** | Build → erase → program → verify (no debug session) |
| **SiLabs 8051: Flash Only (no erase)** | Build → program → verify, skipping the erase pass |

Then replace `YourProject.uvproj` with the path to your Keil µVision project file.

---

## Usage

Press **F5** (or the play button in the Run & Debug panel) and select a configuration.
The extension handles everything automatically:

1. Starts the DAP server if not already running
2. Invokes `UV4.exe -b` to build the project (when `buildBeforeDebug: true`)
3. Invokes `UV4.exe -f` to flash the target (flash-only configs)
4. Connects VSCode to the DAP server and begins the debug session

---

## Launch configuration reference

| Field | Default | Description |
|-------|---------|-------------|
| `uvprojFile` | *(auto-detect)* | Path to the Keil project file (`.uvproj` / `.uvprojx`). Auto-detected if omitted. Supports `${workspaceFolder}` and `${workspaceFolder:Name}`. |
| `program` | *(from uvproj)* | Path to the Intel HEX file. Derived from the project file if omitted. |
| `buildBeforeDebug` | `false` | `true` → run `UV4.exe -b` before launching. |
| `buildTarget` | *(from .uvopt)* | µVision target name passed as `-t <name>` to UV4.exe. If omitted, UV4 uses whichever target was last active in the project. |
| `noDebug` | `false` | `true` → flash-only mode, no debug session. |
| `noErase` | `false` | `true` → skip erase pass (program + verify only, faster). |

---

## Debug features

| Feature | Notes |
|---------|-------|
| Breakpoints | Source-line and instruction breakpoints; max **4 active** (hardware limit) |
| Step over / into / out | Source-level and instruction granularity |
| Pause | Halts a running target |
| Variables / Locals | Local variables from debug info |
| Registers | R0–R7, ACC, B, SP, DPTR, PSW, PC — **Registers** panel in Debug sidebar |
| Edit values | Right-click any variable or watch → **Set Value** to write to hardware |
| Watch expressions | SFR names, register names, hex addresses (`ACC`, `0x20`, …) |
| Memory panels | **DATA / XDATA / IDATA / CODE** panels in Debug sidebar, auto-refresh on halt |
| Read/write memory | `DATA:0xNN`, `XDATA:0xNNNN`, `CODE:0xNNNN`, `IDATA:0xNN` in Watch |
| Disassembly | Right-click in editor → **Open Disassembly View** while paused |

> **Note:** The built-in VS Code Memory Inspector is not supported. Use the
> DATA / XDATA / IDATA / CODE sidebar panels instead.

---

## Troubleshooting

**"Keil installation not found"**
→ Set `silabs8051.keilPath` in VS Code settings to your Keil root folder.

**"Failed to connect to DAP server" / ECONNREFUSED**
→ The DAP server failed to start. Check that `dap_server.exe` is present in the
extension's `bin\` folder and that the DLLs were copied successfully.

**"Target not connected" / INITFEATURES returns 1**
→ Check that the USB Debug Adapter is plugged in and the debug header is connected.

**Build output not appearing**
→ Open the **SiLabs 8051 Build** output channel (`View → Output`).

**Port 4711 already in use**
→ Kill any existing `dap_server.exe` process via Task Manager, then retry.
