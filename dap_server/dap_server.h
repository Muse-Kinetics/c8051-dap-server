// dap_server/dap_server.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// DapServer — Winsock TCP listener with DAP Content-Length framing.
//
// Phase 1: handles 'initialize', 'configurationDone', 'disconnect'.
//          All other commands return a stub 'not implemented' response.
// Phase 2+: Launch handler calls into AgdiLoader / RunControl / BpManager.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// winsock2.h must come before windows.h to avoid the winsock1 conflict.
// agdi.h defines WIN32_LEAN_AND_MEAN which keeps windows.h from pulling in winsock.h,
// so the include order here is safe.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// DapServer
// ---------------------------------------------------------------------------

class DapServer {
public:
    explicit DapServer(int port);
    ~DapServer();

    // Block until a session completes (connect → initialize → ... → disconnect).
    // Returns true if the session ended cleanly, false on socket error.
    // Call repeatedly to serve multiple sequential sessions.
    bool RunSession();

    // Run indefinitely, serving sessions one after another.
    void RunForever();

    // Send an event to the connected client.
    // Thread-safe: may be called from the run-control callback thread.
    void SendEvent(const std::string& eventName, const nlohmann::json& body);

private:
    // Socket setup
    bool Init();
    void Cleanup();
    SOCKET Accept();

    // Message framing
    bool RecvMessage(nlohmann::json& out);
    void SendMessage(const nlohmann::json& msg);

    // Response helpers
    void SendResponse(int requestSeq,
                      const std::string& command,
                      const nlohmann::json& body = nlohmann::json(nullptr),
                      bool success = true,
                      const std::string& message = "");

    // Dispatch
    void Dispatch(const nlohmann::json& msg);

    // Command handlers
    void HandleInitialize      (int seq, const nlohmann::json& args);
    void HandleConfigurationDone(int seq, const nlohmann::json& args);
    void HandleLaunch          (int seq, const nlohmann::json& args);
    void HandleDisconnect      (int seq, const nlohmann::json& args);
    void HandleSetBreakpoints  (int seq, const nlohmann::json& args);
    void HandleSetExceptionBreakpoints(int seq, const nlohmann::json& args);
    void HandleThreads         (int seq, const nlohmann::json& args);
    void HandleContinue        (int seq, const nlohmann::json& args);
    void HandleNext            (int seq, const nlohmann::json& args);
    void HandleStepIn          (int seq, const nlohmann::json& args);
    void HandlePause           (int seq, const nlohmann::json& args);
    void HandleStackTrace      (int seq, const nlohmann::json& args);
    void HandleScopes          (int seq, const nlohmann::json& args);
    void HandleVariables       (int seq, const nlohmann::json& args);
    void HandleReadMemory      (int seq, const nlohmann::json& args);

    int  m_port;
    SOCKET m_listenSock  = INVALID_SOCKET;
    SOCKET m_clientSock  = INVALID_SOCKET;

    std::atomic<int>  m_seq{1};
    std::mutex        m_sendMutex;
    std::atomic<bool> m_disconnecting{false};
    bool               m_pendingEntryStop{false};

    int NextSeq() { return m_seq.fetch_add(1); }
};
