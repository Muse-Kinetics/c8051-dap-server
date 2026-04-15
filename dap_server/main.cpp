// dap_server/main.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Entry point for the AGDI DAP server.
//
// Thread model (Phase 2+):
//   Main thread  — creates the HWND_MESSAGE window; runs Win32 GetMessage loop.
//                  The DLL uses PostMessage(hwnd, msgToken, ...) for halt
//                  notification, so GetMessage must run on the HWND owner thread.
//   Worker thread — runs DapServer::RunForever (blocks on TCP socket I/O).

#include "run_control.h"   // WIN32_LEAN_AND_MEAN + windows.h
#include "dap_server.h"    // winsock2.h
#include "dap_types.h"     // kDapPort
#include "bp_manager.h"
#include "log.h"

#include <thread>

// ---------------------------------------------------------------------------
// Worker thread: TCP DAP server
// ---------------------------------------------------------------------------

static void ServerThread()
{
    DapServer server(kDapPort);
    server.RunForever();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    LOG("========================================\n");
    LOG("  Silabs 8051 DAP Server  (Debug build)\n");
    LOG("  AGDI backend: SiC8051F.dll\n");
    LOG("  DAP port:     %d\n", kDapPort);
    LOG("========================================\n");

    // Initialise halt event and breakpoint pool.
    g_runControl.Init();
    g_bpManager.Init();

    // Create the HWND_MESSAGE window on the main thread so that this thread's
    // message queue receives messages posted by the DLL.
    if (!g_runControl.CreateHwndWindow()) {
        LOG("[ERROR] Failed to create HWND_MESSAGE window\n");
        return 1;
    }
    LOG("[INIT] Win32 message window ready\n");

    // Start the DAP server on a worker thread.
    std::thread serverThread(ServerThread);
    serverThread.detach();

    LOG("[INIT] Waiting for DAP client on 127.0.0.1:%d ...\n", kDapPort);

    // Main thread: pump Win32 messages indefinitely.
    // WM_QUIT is posted (e.g. from Shutdown via DestroyWindow) to exit.
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_runControl.Shutdown();
    return 0;
}

