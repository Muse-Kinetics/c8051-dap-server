# Debugging SOP

## Purpose
Standard operating procedure for launching the SoftStep Bootloader debug session and recovering the Silicon Labs EC3 adapter when the AGDI session gets stuck.

## How to Start a Debug Session (Autonomous Workflow)
This sequence works without user intervention. Follow it exactly.

1. **Check breakpoints** — keep the total at 3 or fewer.
   - Use `mcp_debug-mcp_get_breakpoints` to list current breakpoints.
2. **Run `reset_silabs.ps1`** — always reset the adapter before launching.
   - `& "c:\Users\temp\Documents\00_Firmware\_utilities\C8051_dap_server\scripts\reset_silabs.ps1"`
   - Wait for `OK done`.
3. **Ensure the DAP server is running** — use `ensure_server.ps1` (no-op if already up).
   - `& "c:\Users\temp\Documents\00_Firmware\_utilities\C8051_dap_server\scripts\ensure_server.ps1"`
4. **Launch via VS Code command** — this is the equivalent of pressing F5.
   - Use `run_vscode_command` with `commandId: workbench.action.debug.start`.
   - Do NOT use `mcp_debug-mcp_start_debugging` — it fails because `launch.json` is embedded in the workspace file, not a `.vscode/launch.json`.
5. **Confirm the session is live** — wait ~8 s then check:
   - `mcp_debug-mcp_get_debug_state` should show `is_paused: true`.
   - Log tail (`dap_server.log`, last 15 lines) should show `configurationDone` and `event stopped`.

## Recovery Procedure (Wedged Adapter)
Trigger: log shows any of:
- `[SUPPRESS] DLL popup window: SiC8051F: OK Communication failure.`
- `[ERROR] Target not connected (INITFEATURES=1)`
- `[ERROR] AGDI session init failed`
- Server stuck at `[AGDI] Loading SiC8051F.dll and registering session...`

**Important:** Kill all `dap_server.exe` instances **before** resetting the EC3 adapter —
the server holds the USB handle and the adapter won't reset while it's open.
`reset_silabs.ps1` handles this automatically, but if running steps manually, stop
the server first.

Steps:
1. Stop the debug session in VS Code (disconnect).
2. `& "...\scripts\stop_server.ps1"` — stops `dap_server.exe`.
3. `& "...\scripts\reset_silabs.ps1"` — power-cycles the EC3. Wait for `OK done`.
4. `& "...\scripts\start_server.ps1"` — starts a fresh server.
5. Re-launch via `run_vscode_command workbench.action.debug.start`.

## Normal Session Flow
1. Continue to a breakpoint with `mcp_debug-mcp_continue_debugging`.
2. Check the call stack in the `stop_event_data.call_stack` field of the response.
   - The call stack uses a **shadow stack** maintained by step operations.
   - After `continue` (free-run), the shadow stack resets — only the current function is shown.
   - After stepping (step-in, step-over, step-out), the full call chain is preserved.
3. Step in/out/over with `mcp_debug-mcp_step_execution`.
4. The call stack is most useful during step sequences — step into a function to see
   caller frames build up, step out to see them unwind.

## Key Constraints
- **Max 3 breakpoints** — the hardware path is unreliable beyond that.
- **Never use `mcp_debug-mcp_start_debugging`** — always use `run_vscode_command workbench.action.debug.start`.
- **`reset_silabs.ps1`** contacts a remote USB power-cycle server at `192.168.1.67:9275`. It must return `OK done` before proceeding.
- After any abnormal termination, always run the full recovery sequence before relaunching.
- **Multi-root workspace terminal CWD** — this repo is typically opened alongside the
  firmware project in a multi-root workspace. When you open a new terminal in VS Code,
  the default CWD may be the firmware folder, not the DAP server folder. Always verify
  the working directory (`$PWD` / `Get-Location`) before running scripts.

## Popup Capture Procedure
If a popup appears:
1. Note the title: **Visual Studio Code** = frontend error; **SiC8051F** = native DLL popup.
2. Check the log for `[SUPPRESS]` or `[ERROR]` lines.
3. Copy the full popup text and treat it as a bug if it escaped suppression.

## DAP Server Log
[dap_server.log](../dap_server.log) — session state, stack unwind traces, connection failures.
