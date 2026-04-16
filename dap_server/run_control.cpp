// dap_server/run_control.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Phase 2: HWND_MESSAGE window creation and AGDI registration chain.
// Phase 4/5: GoStep / WaitForHalt / SignalHalt.

#include "run_control.h"
#include "agdi_loader.h"
#include "bp_manager.h"
#include "registers.h"
#include "hex_loader.h"
#include "dap_server.h"
#include "log.h"

#include <cstring>
#include <string>

// Forward declaration — defined in dap_server.cpp, set when a session is active.
extern DapServer* g_pServer;

// Sends a DAP 'output' event to the client (safe to call from the DLL callback thread).
static void SendOutput(const std::string& msg)
{
    if (g_pServer) {
        g_pServer->SendEvent("output", {
            {"category", "console"},
            {"output",   msg},
        });
    }
}

RunControl g_runControl;

// ---------------------------------------------------------------------------
// SEH wrapper for INITFEATURES — must be a plain function (no C++ unwind)
// ---------------------------------------------------------------------------

// Filter runs in exception-filter context; logs crash address before handling.
static LONG InitFeaturesFilter(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR rw      = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR badAddr = ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr,
            "RunControl: INITFEATURES AV code=0x%08lX at=%p %s addr=0x%08lX\n",
            (unsigned long)code, addr,
            rw == 0 ? "read" : rw == 1 ? "write" : "DEP",
            (unsigned long)badAddr);
    } else {
        fprintf(stderr, "RunControl: INITFEATURES CRASHED code=0x%08lX at=%p\n",
            (unsigned long)code, addr);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG FlashTailFilter(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "RunControl: FLASH TAIL CRASHED code=0x%08lX at=%p\n",
            (unsigned long)code, addr);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool CallFlashTail(AgdiLoader& agdi)
{
    U32 ret = 0;
    __try {
        ret = agdi.AG_Init(AGDI_INITFLASHLOAD, nullptr);   // 0x0313
        LOGV("RunControl: INITFLASHLOAD returned %u\n", ret);
        LOG("[FLASH] Starting flash operation...\n");
        ret = agdi.AG_Init(AGDI_STARTFLASHLOAD, nullptr);  // 0x0314
        LOG("[FLASH] Flash complete (STARTFLASHLOAD returned %u)\n", ret);
    } __except (FlashTailFilter(GetExceptionInformation())) {
        return false;
    }
    return true;
}

static U32 CallInitFeatures(AgdiLoader& agdi, void* pFeatures)
{
    U32 ret = 0;
    __try {
        ret = agdi.AG_Init(AGDI_INITFEATURES, pFeatures);
    } __except (InitFeaturesFilter(GetExceptionInformation())) {
        return (U32)-1;  // sentinel: caller checks for -1
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Window class name for the hidden message-only window
// ---------------------------------------------------------------------------

static constexpr char kWndClassName[] = "DapServerMsgWnd";

// ---------------------------------------------------------------------------
// DapAgdiCallback — called on SiC8051F.dll's internal thread
// ---------------------------------------------------------------------------

extern "C" U32 DapAgdiCallback(U32 nCode, void* vp)
{
    // Log all codes so we can see what the DLL actually sends.
    LOG("[CBK]  nCode=0x%04X vp=%p\n", nCode, vp);
    switch (nCode) {
    case AG_CB_INITREGV:
        // The DLL sends this once during init with a stack-local RegDsc.
        // The internal pointers (RegArr, RegGet, etc.) reference DLL statics
        // that persist for the session.  Save a copy of the struct.
        if (vp) {
            RegDsc* rd = static_cast<RegDsc*>(vp);
            g_runControl.SaveRegDsc(rd);
            LOG("[REGS] Saved RegDsc: nRitems=%d  RegGet=%p  RegArr=%p\n",
                rd->nRitems, rd->RegGet, rd->RegArr);

            // Dump ALL rItems so we can see the correct nItem IDs for AG_RegAcc.
            if (rd->RegArr) {
                for (int i = 0; i < rd->nRitems; ++i) {
                    rItem& ri = rd->RegArr[i];
                    LOG("[REGS]   [%2d] szReg=%-8s isPC=%d nGi=%d nItem=0x%04X\n",
                        i, ri.szReg, ri.isPC, ri.nGi, ri.nItem);
                }
            }
        }
        break;

    case AG_CB_MSGSTRING:
        if (vp) {
            // Trim trailing whitespace the DLL often pads into its fixed-size string buffers.
            std::string msg = static_cast<const char*>(vp);
            while (!msg.empty() && (msg.back() == ' ' || msg.back() == '\r' || msg.back() == '\n'))
                msg.pop_back();
            LOG("[DLL]  %s\n", msg.c_str());
            SendOutput(msg + "\n");

            // The DLL does not reliably call AG_RUNSTOP or PostMessage for step/continue
            // completion.  "Target Halted" in the message stream is the authoritative
            // signal that the CPU has stopped.  Use it to unblock WaitForHalt().
            if (msg.find("Halted") != std::string::npos ||
                msg.find("halted") != std::string::npos) {
                LOG("[DEBUG] Halt detected via MSGSTRING — signalling halt event\n");
                g_runControl.SignalHalt("stopped");
            }
        }
        break;

    case AG_CB_GETFLASHPARAM: {
        // Iterator protocol (from KAN145 AGDI.CPP reference):
        //   vp == NULL  → first call: return a pointer to FLASHPARM with the first chunk.
        //   vp != NULL  → subsequent call: update FLASHPARM in-place for next chunk,
        //                 OR set many=0 to signal end-of-data.
        // We have one contiguous image chunk, so:
        //   - First call (vp==NULL): fill s_fp and return its address.
        //   - Any call with vp!=NULL: set many=0 to signal "no more data".
        static FLASHPARM s_fp = {};
        if (!vp) {
            // First call: fill FLASHPARM with our image.
            std::memset(&s_fp, 0, sizeof(s_fp));
            if (g_hexLoader.IsLoaded()) {
                g_hexLoader.FillFlashParm(s_fp);
                LOG("[FLASH] Providing image: base=0x%04X  size=%u bytes\n",
                        g_hexLoader.BaseAddress(), g_hexLoader.ByteCount());
            } else {
                LOG("[FLASH] WARNING: GETFLASHPARAM called but no image loaded\n");
            }
            return reinterpret_cast<U32>(&s_fp);
        } else {
            // Subsequent call: signal end-of-data by setting many=0.
            FLASHPARM* pF = static_cast<FLASHPARM*>(vp);
            pF->many = 0;
            LOGV("DapAgdiCallback: AG_CB_GETFLASHPARAM subsequent call — end\n");
            return reinterpret_cast<U32>(pF);
        }
    }

    case AG_CB_PROGRESS:
        // Progress updates during flash — ignore for now.
        break;

    case AG_CB_FORCEUPDATE:
        // Force UI update — nothing to do in a DAP server.
        break;

    case AG_CB_GETDEVINFO: {
        // The DLL passes a pre-zeroed DEV_X66* and asks us to fill device info.
        // Fill with C8051F380 specifics so the DLL has a valid memory map.
        if (vp) {
            DEV_X66* dev = static_cast<DEV_X66*>(vp);
            // Strings
            strncpy(reinterpret_cast<char*>(dev->Vendor), "Silicon Laboratories", sizeof(dev->Vendor) - 1);
            strncpy(reinterpret_cast<char*>(dev->Device), "C8051F380",            sizeof(dev->Device) - 1);
            // Core clock: 48 MHz (USB full-speed PLL)
            dev->Clock       = 48000000UL;
            dev->RestoreBp   = 1;
            dev->Rtos        = 0;
            dev->Mod167      = 0;  // not applicable for 8051
            dev->useOnChipRom = 1;
            // Code flash: 64 KB at CODE space (0xFF000000)
            dev->Irom.mTyp   = 1;  // ROM/Flash
            dev->Irom.nStart = 0xFF000000UL;
            dev->Irom.nSize  = 0x10000UL;
            // On-chip XRAM: 4 KB at XDATA space (0x01000000)
            dev->Xram1.mTyp   = 0;  // RAM
            dev->Xram1.nStart = 0x01000000UL;
            dev->Xram1.nSize  = 0x1000UL;
            // Internal DATA RAM: 256 bytes at DATA space (0xF0000000)
            dev->Iram.mTyp   = 0;  // RAM
            dev->Iram.nStart = 0xF0000000UL;
            dev->Iram.nSize  = 0x100UL;
            LOGV("DapAgdiCallback: AG_CB_GETDEVINFO filled DEV_X66 for C8051F380\n");
        }
        return AG_OK;
    }

    case 0x10: {
        // AG_CB_GETBOMPTR — DLL calls pCbFunc(0x10, NULL) expecting an ioc* return value.
        // The ioc struct is SiLabsIoc (extended MonConf): standard MonConf fields followed by
        // SiLabs USB fields (ECProtocol=1 for USB, Adapter=1 for first USB adapter).
        // ECProtocol=0 → serial (triggers COM port validation dialog).
        // ECProtocol=1 → USB (uses USBHID.dll; Adapter selects adapter index).
        static SiLabsIoc s_ioc = {};
        if (s_ioc.ECProtocol == 0 && s_ioc.Adapter == 0) {  // first-time init
            s_ioc.comnr      = 1;      // must be valid (1-512) to pass DLL validation
            s_ioc.baudrate   = 115200;
            s_ioc.Opt        = 0;  // set per-session below
            // MonPath contains command-line arguments parsed by the DLL's
            // AnalyzeMonParas jump table.  Key flags:
            //   -A<n> = adapter type (2=EC2 serial, 3=EC3 USB Debug Adapter)
            //   -J<n> = ECProtocol (0=JTAG, 1=C2)
            //   -K<n> = adapter index
            //   -L<n> = USBPower
            //   -P<n> = PowerTarget (0=off, 1=on)
            //   -U<s> = USB adapter serial number
            // The DLL USB init defaults adapter to 2 (EC2 serial), but the
            // actual hardware is an EC3 USB Debug Adapter which requires -A3
            // for the correct reset sequence and initialization delay.
            strncpy(s_ioc.MonPath,
                    "-J1 -K0 -L8 -A3 -P0",
                    sizeof(s_ioc.MonPath) - 1);
            s_ioc.ECProtocol = 1;      // 1 = USB mode (never read by DLL directly)
            s_ioc.Adapter    = 1;      // first USB debug adapter
            s_ioc.USBPower   = 8;      // normal USB power
        }
        // Update Opt each session (noErase may differ between calls).
        s_ioc.Opt = g_runControl.IsNoErase()
                    ? (FLASH_PROGRAM | FLASH_VERIFY)  // 0x0600 — skip erase pass
                    : 0;                              // 0x0000 — DLL default (erase+program+verify)
        LOGV("DapAgdiCallback: AG_CB_GETBOMPTR — ECProtocol=%lu Adapter=%lu Opt=0x%lX\n",
                s_ioc.ECProtocol, s_ioc.Adapter, s_ioc.Opt);
        return reinterpret_cast<U32>(&s_ioc);
    }

    default:
        break;
    }

    // AG_RUNSTOP (0x0011) signals that a Go/Step operation has completed.
    if (nCode == AG_RUNSTOP) {
        LOG("[DEBUG] AG_RUNSTOP — target halted\n");
        g_runControl.SignalHalt("step");
    }

    return AG_OK;
}

// Forward declaration — defined below in Go/Step section.
static DWORD CallGoStepSEH(WORD nCode, DWORD nSteps, GADR* pAddr);

// ---------------------------------------------------------------------------
// Hidden HWND_MESSAGE window
// ---------------------------------------------------------------------------

LRESULT CALLBACK RunControl::HwndMsgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    // The DLL calls PostMessage(hWnd, msgToken, ...) after GoStep completes.
    // We compare msg against m_msgToken — if it matches, signal halt.
    if (msg == g_runControl.GetMsgToken()) {
        LOGV("RunControl: halt PostMessage received\n");
        // Primary halt path is DapAgdiCallback(AG_RUNSTOP,...); this is secondary.
        // Only signal if callback hasn't already done so (WaitForHalt is tolerant
        // of double-set because the event is auto-reset only after WaitForHalt).
        g_runControl.SignalHalt("breakpoint");
        return 0;
    }

    // WM_USER+2: stop-request posted from the DAP reader thread (HandlePause).
    // This executes on the main thread (inside the DLL's internal message pump
    // during AG_GoStep), so calling AG_GoStep(AG_STOPRUN) is safe here.
    if (msg == WM_USER + 2) {
        LOG("[DEBUG] HwndMsgProc: stop-request received — calling AG_GoStep(AG_STOPRUN)\n");
        if (g_agdi.AG_GoStep) {
            g_agdi.AG_GoStep(AG_STOPRUN, 0, nullptr);
        }
        return 0;
    }

    // WM_USER+3: run-request posted from the DAP reader thread (HandleContinue).
    // Calls GoStep(AG_GOFORBRK) on the main thread so the reader thread stays
    // free for receiving DAP messages (e.g. pause).  The DLL's internal message
    // pump (active while GoStep blocks) will dispatch WM_USER+2 stop-requests.
    // WM_USER+3: run-request — execute AG_GOFORBRK on the main thread.
    // This blocks until the target halts (breakpoint or external STOPRUN).
    // For breakpoint hits the DLL fires AG_CB_INITREGV inside GOFORBRK.
    // For pause (STOPRUN from reader thread) it does not — the register
    // refresh is handled separately via WM_USER+4 after StopDirect returns.
    if (msg == WM_USER + 3) {
        LOG("[DEBUG] HwndMsgProc: run-request received — calling GoStep(AG_GOFORBRK)\n");
        if (g_agdi.AG_GoStep) {
            DWORD exc = CallGoStepSEH(AG_GOFORBRK, 0, nullptr);
            if (exc) {
                LOG("[CRASH] SEH exception 0x%08lX in AG_GoStep(AG_GOFORBRK) on main thread\n", exc);
            } else {
                LOG("[DEBUG] HwndMsgProc: GoStep(AG_GOFORBRK) returned\n");
            }
        }
        g_runControl.SignalHalt("stopped");
        return 0;
    }

    // WM_USER+4: register-refresh request from HandlePause.
    // By the time this message is dispatched, both GOFORBRK and STOPRUN have
    // returned, so the DLL is idle and it is safe to issue AG_NSTEP(1).
    // The single-step triggers AG_CB_INITREGV with the real PC/SP/ACC/PSW.
    if (msg == WM_USER + 4) {
        LOG("[DEBUG] HwndMsgProc: register-refresh — AG_NSTEP(1)\n");
        if (g_agdi.AG_GoStep) {
            g_agdi.AG_GoStep(AG_NSTEP, 1, nullptr);
        }
        g_runControl.SignalHalt("register-refresh");
        return 0;
    }

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hw, msg, wp, lp);
}

bool RunControl::CreateHwndWindow()
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSA wc{};
    wc.lpfnWndProc   = HwndMsgProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWndClassName;

    // RegisterClassA may return 0 if the class was already registered (e.g. after
    // a previous session on the same process).  GetLastError() == ERROR_CLASS_ALREADY_EXISTS
    // is not a fatal error.
    ATOM atom = RegisterClassA(&wc);
    if (!atom) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            OutputDebugStringA("RunControl: RegisterClassA failed\n");
            return false;
        }
    }

    // HWND_MESSAGE creates a message-only window: no desktop presence,
    // messages are queued for the creating thread only.
    m_hwnd = CreateWindowExA(0, kWndClassName, "DapServer",
                             0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!m_hwnd) {
        OutputDebugStringA("RunControl: CreateWindowExA failed\n");
        return false;
    }

    char msg[80];
    _snprintf_s(msg, sizeof(msg), "RunControl: HWND_MESSAGE window created = %p\n", (void*)m_hwnd);
    OutputDebugStringA(msg);
    return true;
}

// ---------------------------------------------------------------------------
// AGDI registration chain
// ---------------------------------------------------------------------------

bool RunControl::InitAgdiSession(bool isFlash, bool noErase)
{
    if (!g_agdi.Load()) {
        LOG("[ERROR] Failed to load SiC8051F.dll\n");
        return false;
    }
    LOG("[AGDI] SiC8051F.dll loaded OK\n");

    m_noErase      = noErase;
    m_isFlashOnly  = isFlash;

    m_hModule    = GetModuleHandleA(nullptr);
    m_curPC      = 0;
    m_pDoEvents  = AgdiDoEvents;
    m_pCBF       = DapAgdiCallback;
    std::memset(&m_features, 0, sizeof(m_features));

    // EnumUv351(pDbg, 2) registers a debug block pointer.
    // The DLL stores it globally and later dereferences offsets up to 0x1454.
    // Importantly, the DLL reads MonPath flags from pDbg+0x514 (offset 1300)
    // and a tool path from pDbg+0x410 (offset 1040).  The MonPath in the
    // SiLabsIoc BOM struct is NOT parsed by the DLL for connection config —
    // only the pDbg copy is used by the MonPath parser (sub_100458D0).
    // (No pre-session UNINIT needed — DLL is freshly loaded each session.)

    std::memset(m_dbgBlock, 0, sizeof(m_dbgBlock));

    // pDbg+0x410 (offset 1040): ToolOpt path — copied to internal global.
    // Use the directory containing SiC8051F.dll.
    {
        char toolPath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, toolPath, MAX_PATH)) {
            char* slash = strrchr(toolPath, '\\');
            if (slash) *(slash + 1) = '\0';
        }
        strncpy(reinterpret_cast<char*>(m_dbgBlock + 0x410), toolPath,
                sizeof(m_dbgBlock) - 0x410 - 1);
    }

    // pDbg+0x514 (offset 1300): MonPath flags parsed by sub_100458D0.
    //   -A3  = EC3 USB Debug Adapter (not EC2 serial)
    //   -J1  = C2 protocol (not JTAG)
    //   -K0  = adapter index 0
    //   -L8  = USBPower
    //   -P0  = PowerTarget off
    //   (no -U flag: connects to the first available EC3 adapter by index -K0)
    strncpy(reinterpret_cast<char*>(m_dbgBlock + 0x514),
            "-J1 -K0 -L8 -A3 -P0",
            sizeof(m_dbgBlock) - 0x514 - 1);

    LOGV("RunControl: ToolPath = \"%s\"\n", reinterpret_cast<char*>(m_dbgBlock + 0x410));
    LOGV("RunControl: MonPath  = \"%s\"\n", reinterpret_cast<char*>(m_dbgBlock + 0x514));
    if (g_agdi.EnumUv351) {
        U32 famCode = g_agdi.EnumUv351(m_dbgBlock, 2);
        LOGV("RunControl: EnumUv351 returned 0x%X\n", famCode);
    }

    U32 ret;
    HWND hwnd = m_hwnd;

    LOG("[AGDI] Loading SiC8051F.dll and registering session...\n");

    // Registration chain (common prefix for flash and debug sessions)
    ret = g_agdi.AG_Init(AGDI_INITPHANDLEP,   (void*)hwnd);
    LOGV("RunControl: INITPHANDLEP returned %u\n", ret);

    ret = g_agdi.AG_Init(AGDI_INITINSTHANDLE,  (void*)m_hModule);
    LOGV("RunControl: INITINSTHANDLE returned %u\n", ret);

    ret = g_agdi.AG_Init(AGDI_INITCURPC,       &m_curPC);
    LOGV("RunControl: INITCURPC returned %u\n", ret);

    ret = g_agdi.AG_Init(AGDI_INITDOEVENTS,    (void*)m_pDoEvents);
    LOGV("RunControl: INITDOEVENTS returned %u\n", ret);

    ret = g_agdi.AG_Init(AGDI_INITUSRMSG,      (void*)(DWORD)m_msgToken);
    LOGV("RunControl: INITUSRMSG returned %u\n", ret);

    ret = g_agdi.AG_Init(AGDI_INITCALLBACK,    (void*)m_pCBF);
    LOGV("RunControl: INITCALLBACK returned %u\n", ret);

    // INITMONPATH (0x030C) — SiC8051F extension: pass the project .wsp path so the DLL
    // can read MonConf.comnr, ECProtocol, Adapter serial, etc. via GetPrivateProfileString.
    // vp IS the char* path string (direct value, not a pointer-to-pointer).
    {
        char monpath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, monpath, MAX_PATH)) {
            char* slash = strrchr(monpath, '\\');
            if (slash) { *(slash + 1) = '\0'; }
            strncat_s(monpath, "SiC8051F.wsp", MAX_PATH - strlen(monpath) - 1);
        }
        ret = g_agdi.AG_Init(AGDI_INITMONPATH, static_cast<void*>(monpath));
        LOGV("RunControl: INITMONPATH returned %u (path=%s)\n", ret, monpath);
    }

    LOG("[AGDI] Registration chain complete — patching dialogs\n");

    // Suppress ALL DLL message-box dialogs by:
    // 1. Patching the DLL's internal ShowDialog wrapper (RVA 0x319B0) to RET.
    // 2. Replacing the DLL's IAT entry for MessageBoxA with a no-op stub.
    // This prevents the DLL from blocking on any dialog during connection.
    {
        BYTE* base = reinterpret_cast<BYTE*>(g_agdi.Module());
        DWORD oldProtect = 0;

        // (a) Patch ShowDialog wrapper at RVA 0x319B0 to just RET (cdecl, caller cleans stack)
        BYTE* dialogFunc = base + 0x319B0;
        if (VirtualProtect(dialogFunc, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            dialogFunc[0] = 0xC3;  // RET
            VirtualProtect(dialogFunc, 1, oldProtect, &oldProtect);
            LOGV("[PATCH] ShowDialog patched to RET at %p\n", dialogFunc);
        }

        // (b) Hook ShowWindow at RVA 0x843B8 to suppress the flash-progress dialog.
        //     The DLL creates its progress dialog via CreateDialogIndirectParamA and
        //     calls ShowWindow(hw, SW_SHOW) to display it.  We intercept ShowWindow
        //     and suppress SW_SHOW for dialog windows whose parent is our hidden HWND,
        //     silently redirecting to SW_HIDE so the dialog is never visible.
        {
            using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
            static ShowWindowFn s_realShowWindow = nullptr;
            void** pSWEntry = reinterpret_cast<void**>(base + 0x843B8);
            s_realShowWindow = reinterpret_cast<ShowWindowFn>(*pSWEntry);
            // Use a proper __stdcall (WINAPI) static method — lambdas are __cdecl
            // and would corrupt the stack on x86 when called via the IAT.
            struct SW {
                static BOOL WINAPI Stub(HWND hw, int nCmdShow) {
                    if (nCmdShow == SW_SHOW || nCmdShow == SW_SHOWNA || nCmdShow == SW_SHOWNORMAL) {
                        LOG("[SUPPRESS] DLL progress window hidden (ShowWindow SW_SHOW -> SW_HIDE, hwnd=%p)\n", (void*)hw);
                        nCmdShow = SW_HIDE;
                    }
                    return s_realShowWindow(hw, nCmdShow);
                }
            };
            if (VirtualProtect(pSWEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *pSWEntry = reinterpret_cast<void*>(&SW::Stub);
                VirtualProtect(pSWEntry, sizeof(void*), oldProtect, &oldProtect);
                LOGV("[PATCH] ShowWindow IAT hooked at %p\n", pSWEntry);
            }
        }

        // (c) Replace MessageBoxA in the DLL's IAT (RVA 0x84440) with a stub
        //     that returns IDOK (1) immediately.
        struct MB {
            static int WINAPI Stub(HWND, LPCSTR text, LPCSTR caption, UINT) {
                std::string msg = std::string(caption ? caption : "") + ": " + (text ? text : "");
                LOG("[SUPPRESS] DLL MessageBox: [%s] %s\n", caption ? caption : "", text ? text : "");
                SendOutput(msg + "\n");
                return IDOK;
            }
        };
        void** pIatEntry = reinterpret_cast<void**>(base + 0x84440);
        if (VirtualProtect(pIatEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *pIatEntry = reinterpret_cast<void*>(&MB::Stub);
            VirtualProtect(pIatEntry, sizeof(void*), oldProtect, &oldProtect);
            LOGV("[PATCH] MessageBoxA IAT hooked at %p\n", pIatEntry);
        }
    }

    LOG("[CONN] Connecting to target...\n");
    ret = CallInitFeatures(g_agdi, &m_features);
    if (ret == (U32)-1) {
        LOG("[ERROR] INITFEATURES crashed (access violation — check DLL state)\n");
        return false;
    }
    LOGV("RunControl: INITFEATURES returned %u\n", ret);
    if (ret != 0) {
        LOG("[ERROR] Target not connected (INITFEATURES=%u) — check EC3 debug connector\n", ret);
        g_agdi.AG_Init(AGDI_UNINIT, nullptr);
        return false;
    }
    LOG("[CONN] Target connected OK\n");

    // Capability queries (AG_GETFEATURE = 0x0200, sub-code = feature index 1–7)
    for (U16 feat = 1; feat <= 7; ++feat) {
        DWORD val = 0;
        g_agdi.AG_Init(static_cast<U16>(0x0200 | feat), &val);
    }
    LOGV("RunControl: AG_Init GETFEATURE queries done\n");

    if (isFlash) {
        // Flash tail: prepare then trigger (DLL calls CB_GETFLASHPARAM back).
        // Wrapped in a plain C SEH function because C++ unwind is incompatible with __try.
        if (!CallFlashTail(g_agdi)) {
            return false;
        }
    } else {
        // Debug tail: register BP list head then reset.
        // The DLL stores this pointer-to-pointer and dereferences it during
        // AG_GoStep(AG_GOFORBRK) to walk the breakpoint list.  It must be
        // the address of a persistent member — NOT a stack local.
        ret = g_agdi.AG_Init(AGDI_INITBPHEAD, g_bpManager.HeadPtr());  // 0x030E
        LOGV("RunControl: INITBPHEAD returned %u\n", ret);

        ResetEvent(m_haltEvent);
        ret = g_agdi.AG_Init(AGDI_RESET, nullptr);             // 0x040D
        LOGV("RunControl: RESET returned %u\n", ret);
        LOG("[DEBUG] Target reset — waiting for halt...\n");
    }

    m_sessionActive = true;
    LOG("[AGDI] Session ready\n");
    return true;
}

void RunControl::UninitAgdiSession()
{
    if (!m_sessionActive || !g_agdi.IsLoaded()) return;

    LOG("[AGDI] UNINIT — releasing adapter (may take up to 90s on first call)...\n");
    g_agdi.AG_Init(AGDI_UNINIT, nullptr);
    LOG("[AGDI] UNINIT complete\n");
    m_sessionActive = false;
    m_isFlashOnly   = false;

    // Unload and reload the DLL so the next session starts with completely fresh
    // internal state (resets byte_101DDF9C, USB handles, and all DLL globals).
    // Without this, the DLL's post-UNINIT state causes INITFEATURES to return 1
    // on every subsequent connection attempt within the same process lifetime.
    LOG("[AGDI] Reloading SiC8051F.dll for clean next session...\n");
    g_agdi.Unload();
    if (g_agdi.Load()) {
        LOG("[AGDI] DLL reloaded OK\n");
    } else {
        LOG("[ERROR] DLL reload failed\n");
    }
}

// ---------------------------------------------------------------------------
// RunControl lifecycle
// ---------------------------------------------------------------------------

bool RunControl::Init()
{
    m_haltEvent = CreateEventA(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    return m_haltEvent != nullptr;
}

void RunControl::Shutdown()
{
    UninitAgdiSession();
    g_agdi.Unload();

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_haltEvent) {
        CloseHandle(m_haltEvent);
        m_haltEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Go/Step and halt signalling
// ---------------------------------------------------------------------------

// SEH wrapper — must be in a plain-C function (no C++ dtors on stack).
static DWORD CallGoStepSEH(WORD nCode, DWORD nSteps, GADR* pAddr)
{
    __try {
        g_agdi.AG_GoStep(nCode, nSteps, pAddr);
        return 0;  // success
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

bool RunControl::GoStep(WORD nCode, DWORD nSteps, GADR* pAddr)
{
    if (!g_agdi.AG_GoStep) return false;
    ResetEvent(m_haltEvent);
    DWORD exc = CallGoStepSEH(nCode, nSteps, pAddr);
    if (exc) {
        LOG("[CRASH] SEH exception 0x%08lX in AG_GoStep(nCode=0x%04X)\n", exc, nCode);
        return false;
    }
    return true;
}

void RunControl::RequestStop()
{
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_USER + 2, 0, 0);
    }
}

void RunControl::StopDirect()
{
    // Cross-thread call: halt the target by calling AG_GoStep(AG_STOPRUN) directly.
    // This is called from the reader thread while GoStep(AG_GOFORBRK) is blocking
    // on the main thread.  The DLL may handle cross-thread STOPRUN by setting a
    // flag that the main-thread poll loop checks.
    if (g_agdi.AG_GoStep) {
        LOG("[DEBUG] StopDirect: calling AG_GoStep(AG_STOPRUN) from reader thread\n");
        g_agdi.AG_GoStep(AG_STOPRUN, 0, nullptr);
        LOG("[DEBUG] StopDirect: AG_GoStep(AG_STOPRUN) returned\n");
    }
}

void RunControl::RequestRun()
{
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_USER + 3, 0, 0);
    }
}

void RunControl::RequestRegRefresh()
{
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_USER + 4, 0, 0);
    }
}

void RunControl::ReadRegisters()
{
    if (!g_agdi.AG_RegAcc) {
        LOG("[REGS] ReadRegisters: AG_RegAcc not loaded\n");
        return;
    }

    // AG_RegAcc works for PC (mPC=0x500) and R0-R7 (0x00-0x07).
    // Other registers (ACC, B, SP, DPTR, PSW) return 0 via AG_RegAcc.
    // Read SFRs directly from DATA memory via AG_MemAcc instead.
    //   SP=0x81, ACC=0xE0, B=0xF0, PSW=0xD0, DPL=0x82, DPH=0x83

    RG51 regs{};
    GVAL val{};
    U32 err;

    // First, call AG_AllReg to ensure DLL's internal state is refreshed.
    if (g_agdi.AG_AllReg) {
        err = g_agdi.AG_AllReg(AG_READ, nullptr);
        LOG("[REGS] AG_AllReg(AG_READ) -> %u\n", err);
    }

    // PC via AG_RegAcc (mPC = 0x500)
    val = {};
    g_agdi.AG_RegAcc(AG_READ, 0x500, &val);
    regs.nPC = val.u32 & 0xFFFF;
    LOG("[REGS] PC = 0x%04X\n", regs.nPC);

    // R0-R7 via AG_RegAcc (nReg 0x00..0x07)
    for (int i = 0; i < 8; ++i) {
        val = {};
        g_agdi.AG_RegAcc(AG_READ, i, &val);
        regs.Rn[i] = val.uc;
    }
    LOG("[REGS] R0-R7: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        regs.Rn[0], regs.Rn[1], regs.Rn[2], regs.Rn[3],
        regs.Rn[4], regs.Rn[5], regs.Rn[6], regs.Rn[7]);

    // SFRs via AG_MemAcc from DATA space (amDATA=0xF0).
    // SFR addresses are 0x80-0xFF in the DATA address space.
    // AGDI encodes memory space in the TOP BYTE of Adr: (mSpace << 24) | offset
    if (g_agdi.AG_MemAcc) {
        GADR addr{};
        addr.mSpace = amDATA;
        UC8 byte = 0;

        auto ReadSfr = [&](UL32 sfrAddr) -> UC8 {
            addr.Adr = (static_cast<UL32>(amDATA) << 24) | sfrAddr;
            byte = 0;
            U32 merr = g_agdi.AG_MemAcc(AG_READ, &byte, &addr, 1);
            LOG("[REGS] MemAcc(DATA:0x%02X) -> err=%u val=0x%02X\n", sfrAddr, merr, byte);
            return byte;
        };

        regs.sp  = ReadSfr(0x81);  // SP
        regs.acc = ReadSfr(0xE0);  // ACC
        regs.b   = ReadSfr(0xF0);  // B
        regs.psw = ReadSfr(0xD0);  // PSW
        regs.dpl = ReadSfr(0x82);  // DPL
        regs.dph = ReadSfr(0x83);  // DPH
    } else {
        LOG("[REGS] AG_MemAcc not loaded — falling back to AG_RegAcc for SFRs\n");
        val = {}; g_agdi.AG_RegAcc(AG_READ, 0x10, &val); regs.acc = val.uc;
        val = {}; g_agdi.AG_RegAcc(AG_READ, 0x11, &val); regs.b   = val.uc;
        val = {}; g_agdi.AG_RegAcc(AG_READ, 0x12, &val); regs.sp  = val.uc;
        val = {}; g_agdi.AG_RegAcc(AG_READ, 0x100,&val); regs.psw = val.uc;
        val = {};
        g_agdi.AG_RegAcc(AG_READ, 0x13, &val);
        regs.dpl = static_cast<BYTE>(val.u16 & 0xFF);
        regs.dph = static_cast<BYTE>((val.u16 >> 8) & 0xFF);
    }

    g_registers.Update(regs);
    LOG("[REGS] FINAL: PC=0x%04X SP=0x%02X ACC=0x%02X PSW=0x%02X B=0x%02X DPH:DPL=%02X:%02X\n",
        regs.nPC, regs.sp, regs.acc, regs.psw, regs.b, regs.dph, regs.dpl);
}

void RunControl::SaveRegDsc(const RegDsc* rd)
{
    if (!rd) return;
    m_savedRegDsc = *rd;   // shallow copy — internal pointers reference DLL statics
    m_hasRegDsc = true;
}

void RunControl::ResetHaltEvent()
{
    if (m_haltEvent) {
        ResetEvent(m_haltEvent);
    }
}

bool RunControl::WaitForHalt(DWORD timeoutMs)
{
    if (!m_haltEvent) return false;
    return WaitForSingleObject(m_haltEvent, timeoutMs) == WAIT_OBJECT_0;
}

void RunControl::SignalHalt(const std::string& reason)
{
    m_haltReason = reason;
    if (m_haltEvent) {
        SetEvent(m_haltEvent);
    }
}

