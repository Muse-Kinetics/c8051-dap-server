// dap_server/run_control.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// RunControl — HWND management, AGDI registration chain, run-control wrappers.
//
// Thread model (Phase 2+):
//   Main thread  — creates the HWND_MESSAGE window and pumps Win32 messages
//   Worker thread — runs DapServer::RunForever; calls InitAgdiSession, GoStep
//   DLL thread   — calls DapAgdiCallback; calls AgdiDoEvents
//
// The HWND must be created on the message-pump thread because Win32 message
// queues are per-thread.  All other RunControl methods are thread-safe via
// the halt event.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

#include "agdi.h"

// ---------------------------------------------------------------------------
// The global AGDI callback function.
// Must be a plain C function registered via AGDI_INITCALLBACK.
// Called from SiC8051F.dll's internal thread.
// ---------------------------------------------------------------------------

extern "C" U32 DapAgdiCallback(U32 nCode, void* vp);

// ---------------------------------------------------------------------------
// RunControl
// ---------------------------------------------------------------------------

class RunControl {
public:
    RunControl()  = default;
    ~RunControl() = default;

    // Phase 1: create the halt event.  Call once at startup before RunForever.
    bool Init();

    // Phase 2: create a hidden HWND_MESSAGE window for DLL halt notification.
    // MUST be called from the thread that will pump Win32 messages (main thread).
    bool CreateHwndWindow();

    // Phase 2: load SiC8051F.dll (via g_agdi) and run the AG_Init registration
    // chain.  isFlash=true → flash tail (0x0313/0x0314).
    //         isFlash=false → debug tail (0x030E bpHead + 0x040D reset).
    // noErase=true → set Opt=FLASH_PROGRAM|FLASH_VERIFY, skipping the erase pass.
    // Returns true if the registration chain completed without error.
    bool InitAgdiSession(bool isFlash, bool noErase = false);

    // Phase 2: clean up the active AGDI session (AG_UNINIT).
    void UninitAgdiSession();

    // Phase 4/5: issue a Go/Step command (non-blocking).
    // Call WaitForHalt() afterwards to block until the target stops.
    bool GoStep(WORD nCode, DWORD nSteps, GADR* pAddr);

    // Phase 4/5: block until the DLL fires halt notification or timeout.
    bool WaitForHalt(DWORD timeoutMs = 30000);

    // Thread-safe: signal the halt event.
    // Called from DapAgdiCallback when AG_RUNSTOP arrives.
    void SignalHalt(const std::string& reason);

    // Shut down all resources.  Call once at process exit.
    void Shutdown();

    const std::string& LastHaltReason() const { return m_haltReason; }
    HWND               GetHwnd()     const { return m_hwnd; }
    UINT               GetMsgToken() const { return m_msgToken; }
    bool               IsSessionActive() const { return m_sessionActive; }
    bool               IsNoErase()       const { return m_noErase; }

    // DoEvents stub: DLL calls this pointer periodically during flash/debug ops.
    // Implementation must not block.
    static void AgdiDoEvents() {}

private:
    static LRESULT CALLBACK HwndMsgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);

    HANDLE m_haltEvent    = nullptr;
    HWND   m_hwnd         = nullptr;
    // The DLL uses PostMessage(hwnd, msgToken, ...) for halt notification.
    // We choose WM_USER+1 as our token; we pass &m_msgToken to the DLL so that
    // HwndMsgProc can identify the halt message without knowing a fixed constant.
    UINT   m_msgToken     = WM_USER + 1;

    // Values kept alive for the duration of the session (DLL holds pointers).
    HMODULE  m_hModule    = nullptr;
    DWORD    m_curPC      = 0;
    void   (*m_pDoEvents)() = nullptr;
    pCBF     m_pCBF       = DapAgdiCallback;
    SUPP     m_features{};

    // Zero-filled debug block passed to EnumUv351(pDbg, 2).
    // The DLL stores this pointer and reads offsets 0x410, 0x514, 0x1454 from it.
    // Must remain valid for the lifetime of the session.
    static constexpr size_t kDbgBlockSize = 0x2000;  // 8 KB — well above max known offset 0x1454
    BYTE     m_dbgBlock[kDbgBlockSize] = {};

    bool        m_sessionActive = false;
    bool        m_noErase       = false;
    bool        m_isFlashOnly   = false;
    std::string m_haltReason    = "entry";
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern RunControl g_runControl;

