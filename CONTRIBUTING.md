# Contributing to C8051 DAP Server

Thank you for your interest in contributing! This document explains how to build
the project from source, run tests, and package a release.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Windows 10/11 | — | x86 or x64 host |
| Visual Studio Build Tools 2022 | — | **C++ x86 (Win32)** workload required |
| CMake | ≥ 3.20 | Must be on PATH |
| Keil µVision (C51) | any | Required for `SiC8051F.dll` + `USBHID.dll` |
| Python | ≥ 3.10 | Optional — for integration tests only |

> **32-bit build is mandatory.** `SiC8051F.dll` is a 32-bit DLL and cannot be loaded
> by a 64-bit process.

---

## Getting the Vendor DLLs

`SiC8051F.dll` and `USBHID.dll` are not included in this repo. They ship with
[Keil µVision](https://www.keil.com/download/product/). After installation they are
typically found at:

- `<KeilRoot>\C51\Bin\SiC8051F.dll`
- `<KeilRoot>\UV4\USBHID.dll`

Copy them to `silabs_ref/debug_dll/` before building:

```
silabs_ref/
  debug_dll/
    SiC8051F.dll
    USBHID.dll
```

CMake copies them into the build output directory automatically.

---

## Build

```powershell
# From the repo root — configure (x86 mandatory)
cmake -B build -A Win32

# Build the DAP server
cmake --build build --target dap_server --config Debug
```

Output: `build\dap_server\bin\Debug\dap_server.exe`

To build a Release configuration (for distribution):

```powershell
cmake --build build --target dap_server --config Release
```

---

## Running Tests

Install Python dependencies once:

```powershell
pip install -r requirements.txt
```

Run the integration test suite (requires the DAP server running + USB Debug Adapter connected):

```powershell
.\scripts\start_server.ps1

.venv\Scripts\python.exe scripts\tests\test_flash.py
.venv\Scripts\python.exe scripts\tests\test_debug_launch.py
.venv\Scripts\python.exe scripts\tests\test_output_events.py
```

---

## Building the VSIX

The VSIX is built with `scripts\make_release.ps1` — no Node.js or `vsce` required.

```powershell
# Must have a successful Debug build first
.\scripts\make_release.ps1
```

Output:
```
Release\
  silabs-8051-debug-<version>.vsix   ← install this in VSCode
  bin\                                ← dap_server.exe + DLLs (for manual use)
  ...
```

The script reads the version from `vscode-extension\package.json`. To cut a new version,
update `"version"` there first.

> **Note:** `SiC8051F.dll` and `USBHID.dll` are **not** bundled in the VSIX — they are
> copied from the user's own Keil installation at first activation. `dap_server.exe` and
> `SiC8051F.wsp` are bundled.

---

## Code Signing

For distribution, sign `dap_server.exe` with your code signing certificate before
running `make_release.ps1`, so the bundled executable in the VSIX is signed:

```powershell
# Using Windows SDK signtool.exe
signtool sign /tr http://timestamp.digicert.com /td sha256 /fd sha256 `
    /n "Your Certificate Subject" `
    build\dap_server\bin\Release\dap_server.exe
```

---

## Extending for Other C8051 Variants

The DAP server is designed around the `SiC8051F.dll` AGDI interface which covers the
entire C8051F family. To adapt for a different Silicon Labs 8051 product:

1. **Check if the same DLL applies** — `SiC8051F.dll` covers a broad family including
   C8051F0xx through C8051F38x and EFM8 variants accessible via the USB Debug Adapter.
2. **Register block** — `dap_server/registers.h/.cpp` defines the SFR layout for the
   C8051F380. Add a new register map if your target has different SFRs.
3. **MonPath flags** — `dap_server/run_control.cpp` near the `pDbg+0x514` comment
   sets `-A3` (USB Debug Adapter). Other adapter types use `-A2` (EC2 serial).
4. **Family code** — `EnumUv351()` is called with a family code. The value `2` works for
   C8051F380; other targets may require a different value.

See `Documentation/S8051_DLL_findings.md` for detailed reverse-engineering notes on the
AGDI interface.

---

## Publishing a GitHub Release

The VSIX is distributed via GitHub Releases (it is excluded from the repo by `.gitignore`).

Prerequisites:
- `gh` CLI installed and authenticated — `winget install GitHub.cli` then `gh auth login`
- `make_release.ps1` has already been run (so `Release\` is populated)
- `"version"` in `vscode-extension\package.json` has been bumped
- `CHANGELOG.md` updated with the new version section

```powershell
# 1. Build the VSIX
.\scripts\make_release.ps1

# 2. Tag and publish to GitHub (creates the tag, pushes it, uploads the VSIX)
.\scripts\make_github_release.ps1

# Optional: create as a draft to review before publishing
.\scripts\make_github_release.ps1 -Draft
```

The script reads the version from `package.json`, pulls the top section of `CHANGELOG.md`
as the release notes, creates + pushes the git tag, and uploads the VSIX as a release asset.

---

## Pull Requests

- Keep PRs focused — one feature or fix per PR
- Follow the existing code style (C++17, 4-space indent, `LOG`/`LOGV` for output)
- Test against a real USB Debug Adapter if possible; document any hardware-only behaviour
- Update `CHANGELOG.md` with a summary of changes
