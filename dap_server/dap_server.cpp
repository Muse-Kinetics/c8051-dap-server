// dap_server/dap_server.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Winsock TCP listener with DAP Content-Length framing and command dispatch.
//
// Phase 1: initialize / configurationDone / disconnect round-trip over TCP.
//          All hardware-facing handlers (launch, continue, stepIn, ...) are stubs
//          that respond with a not-implemented error body so that VSCode does not
//          hang waiting for a response.
//
// Phase 2+: Replace the stub bodies with real AGDI calls.

#include "dap_server.h"
#include "dap_types.h"
#include "agdi_loader.h"
#include "run_control.h"
#include "bp_manager.h"
#include "registers.h"
#include "hex_loader.h"
#include "symtab.h"
#include "opcodes8051.h"
#include "log.h"

#include <cstdio>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// Global DapServer pointer — set during RunSession, used by run-control
// helpers to send asynchronous stopped events from background threads.
DapServer* g_pServer = nullptr;

// Generation counter — incremented each time the target is resumed.
// Background threads capture the current value before waiting; if it has
// changed by the time WaitForHalt returns, the wait is stale and the
// stopped event must NOT be sent (a newer command owns the halt).
static std::atomic<uint32_t> g_runGeneration{0};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DapServer::DapServer(int port)
    : m_port(port)
{}

DapServer::~DapServer()
{
    Cleanup();
}

// ---------------------------------------------------------------------------
// Winsock lifetime
// ---------------------------------------------------------------------------

bool DapServer::Init()
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        LOG("[ERROR] WSAStartup failed\n");
        return false;
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        LOG("[ERROR] socket() failed\n");
        WSACleanup();
        return false;
    }

    // SO_REUSEADDR so the port is immediately reusable after the previous session
    BOOL yes = TRUE;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(m_port));

    if (bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG("[ERROR] bind() failed on port %d (already in use?)\n", m_port);
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(m_listenSock, 1) != 0) {
        LOG("[ERROR] listen() failed\n");
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    LOG("[TCP]  Listening on 127.0.0.1:%d\n", m_port);
    return true;
}

void DapServer::Cleanup()
{
    if (m_clientSock != INVALID_SOCKET) {
        closesocket(m_clientSock);
        m_clientSock = INVALID_SOCKET;
    }
    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        WSACleanup();
    }
}

// ---------------------------------------------------------------------------
// Session loop
// ---------------------------------------------------------------------------

SOCKET DapServer::Accept()
{
    sockaddr_in peer{};
    int peerLen = sizeof(peer);
    SOCKET s = accept(m_listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (s != INVALID_SOCKET) {
        LOG("[TCP]  DAP client connected\n");
    }
    return s;
}

bool DapServer::RunSession()
{
    if (m_listenSock == INVALID_SOCKET && !Init()) {
        return false;
    }

    m_clientSock    = Accept();
    m_disconnecting = false;
    m_seq           = 1;
    g_pServer       = this;

    if (m_clientSock == INVALID_SOCKET) {
        return false;
    }

    // Spawn a reader thread that receives DAP messages and dispatches them.
    // This allows the worker thread to block inside AG_GoStep(AG_GOFORBRK)
    // while the reader thread can still receive new commands (e.g. pause).
    std::thread reader([this]() {
        nlohmann::json msg;
        while (!m_disconnecting && RecvMessage(msg)) {
            Dispatch(msg);
        }
        // If the reader exits (client disconnected or error), signal the
        // worker thread in case it's blocked in WaitForHalt.
        m_disconnecting = true;
        g_runControl.RequestStop();
    });

    // This thread waits for the reader to finish.
    reader.join();

    closesocket(m_clientSock);
    m_clientSock = INVALID_SOCKET;
    g_pServer    = nullptr;
    LOG("[TCP]  DAP client disconnected\n");

    // If the client dropped without sending 'disconnect' (crash / F5 Stop),
    // clean up the AGDI session now so the next session starts fresh.
    if (g_runControl.IsSessionActive()) {
        LOG("[AGDI] Client dropped without disconnect — cleaning up session\n");
        g_runControl.UninitAgdiSession();
    }
    g_hexLoader.Unload();

    LOG("[INIT] Waiting for next DAP client on 127.0.0.1:%d ...\n", m_port);
    return true;
}

void DapServer::RunForever()
{
    if (!Init()) return;
    while (true) {
        RunSession();
    }
}

// ---------------------------------------------------------------------------
// DAP Content-Length framing — receive
// ---------------------------------------------------------------------------
//
// DAP wire format:
//   Content-Length: <N>\r\n
//   \r\n
//   <N bytes of UTF-8 JSON>

bool DapServer::RecvMessage(nlohmann::json& out)
{
    // Read headers one byte at a time until we see \r\n\r\n
    std::string headers;
    char c;
    while (true) {
        int n = recv(m_clientSock, &c, 1, 0);
        if (n <= 0) return false;
        headers += c;
        if (headers.size() >= 4 &&
            headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    // Extract Content-Length value
    const char* key = "Content-Length: ";
    size_t pos = headers.find(key);
    if (pos == std::string::npos) {
        OutputDebugStringA("dap_server: missing Content-Length header\n");
        return false;
    }
    int contentLength = std::stoi(headers.substr(pos + strlen(key)));
    if (contentLength <= 0 || contentLength > 4 * 1024 * 1024) {
        OutputDebugStringA("dap_server: bogus Content-Length\n");
        return false;
    }

    // Read exactly contentLength bytes
    std::string body(static_cast<size_t>(contentLength), '\0');
    int total = 0;
    while (total < contentLength) {
        int n = recv(m_clientSock, &body[total], contentLength - total, 0);
        if (n <= 0) return false;
        total += n;
    }

    out = nlohmann::json::parse(body, nullptr, /*exceptions=*/false);
    if (out.is_discarded()) {
        OutputDebugStringA("dap_server: JSON parse error\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// DAP Content-Length framing — send
// ---------------------------------------------------------------------------

void DapServer::SendMessage(const nlohmann::json& msg)
{
    std::string body = msg.dump();
    std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    // Log every outgoing DAP message for debugging (use LOGV to avoid
    // recursion — LOG calls DapLogSend → SendEvent → SendMessage → LOG …)
    std::string type = msg.value("type", "?");
    if (type == "response") {
        LOGV("[WIRE] -> response cmd=%s req_seq=%d success=%s body_size=%d\n",
            msg.value("command", "?").c_str(),
            msg.value("request_seq", -1),
            msg.value("success", false) ? "true" : "false",
            (int)body.size());
    } else if (type == "event") {
        std::string ev = msg.value("event", "?");
        if (ev != "output")   // suppress output-event logging (chatty)
            LOGV("[WIRE] -> event %s\n", ev.c_str());
    }

    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_clientSock == INVALID_SOCKET) return;

    int total = 0;
    int len   = static_cast<int>(frame.size());
    while (total < len) {
        int n = send(m_clientSock, frame.c_str() + total, len - total, 0);
        if (n <= 0) return;
        total += n;
    }
}

// ---------------------------------------------------------------------------
// Response / event helpers
// ---------------------------------------------------------------------------

void DapServer::SendResponse(int requestSeq,
                              const std::string& command,
                              const nlohmann::json& body,
                              bool success,
                              const std::string& message)
{
    nlohmann::json resp = {
        {"seq",         NextSeq()},
        {"type",        "response"},
        {"request_seq", requestSeq},
        {"success",     success},
        {"command",     command},
    };
    if (!body.is_null()) {
        resp["body"] = body;
    }
    if (!message.empty()) {
        resp["message"] = message;
    }
    SendMessage(resp);
}

void DapServer::SendEvent(const std::string& eventName, const nlohmann::json& body)
{
    nlohmann::json evt = {
        {"seq",   NextSeq()},
        {"type",  "event"},
        {"event", eventName},
    };
    if (!body.is_null()) {
        evt["body"] = body;
    }
    SendMessage(evt);
}

// ---------------------------------------------------------------------------
// DapLogSend — routes LOG() text to VS Code Debug Console via DAP output
// ---------------------------------------------------------------------------
void DapLogSend(const char* fmt, ...)
{
    if (!g_pServer) return;

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_pServer->SendEvent("output", {
        {"category", "console"},
        {"output",   buf},
    });
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void DapServer::Dispatch(const nlohmann::json& msg)
{
    if (!msg.contains("command") || !msg.contains("seq")) return;

    const std::string command = msg["command"].get<std::string>();
    const int         seq     = msg["seq"].get<int>();
    const nlohmann::json& args = msg.contains("arguments") ? msg["arguments"]
                                                            : nlohmann::json::object();

    LOG("[DAP]  -> %s (seq=%d)\n", command.c_str(), seq);

    if      (command == "initialize")        HandleInitialize       (seq, args);
    else if (command == "configurationDone") HandleConfigurationDone(seq, args);
    else if (command == "launch")            HandleLaunch           (seq, args);
    else if (command == "disconnect")        HandleDisconnect       (seq, args);
    else if (command == "terminate")         HandleDisconnect       (seq, args);
    else if (command == "setBreakpoints")          HandleSetBreakpoints         (seq, args);
    else if (command == "setExceptionBreakpoints")   HandleSetExceptionBreakpoints(seq, args);
    else if (command == "threads")                    HandleThreads                (seq, args);
    else if (command == "continue")          HandleContinue         (seq, args);
    else if (command == "next")              HandleNext             (seq, args);
    else if (command == "stepIn")            HandleStepIn           (seq, args);
    else if (command == "stepOut")           HandleStepOut          (seq, args);
    else if (command == "pause")             HandlePause            (seq, args);
    else if (command == "stackTrace")        HandleStackTrace       (seq, args);
    else if (command == "scopes")            HandleScopes           (seq, args);
    else if (command == "variables")         HandleVariables        (seq, args);
    else if (command == "readMemory")        HandleReadMemory       (seq, args);
    else if (command == "evaluate")          HandleEvaluate         (seq, args);
    else if (command == "source")            HandleSource           (seq, args);
    else {
        LOG("[DAP]  <- %s: not implemented\n", command.c_str());
        SendResponse(seq, command, nlohmann::json(nullptr),
                     false, "not implemented: " + command);
    }
}

// ---------------------------------------------------------------------------
// Phase 1 — initialize
// ---------------------------------------------------------------------------

void DapServer::HandleInitialize(int seq, const nlohmann::json& /*args*/)
{
    // Respond with capability advertise
    SendResponse(seq, "initialize", MakeCapabilities());

    // Immediately signal that we are ready to receive launch/attach
    SendEvent("initialized", nlohmann::json(nullptr));
}

// ---------------------------------------------------------------------------
// Phase 1 — configurationDone
// ---------------------------------------------------------------------------

void DapServer::HandleConfigurationDone(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "configurationDone");

    // Now that the client has finished configuration, send the deferred
    // entry-stop event.  A short delay ensures the response is flushed
    // to the client before the event arrives (matches mudap pattern).
    if (m_pendingEntryStop) {
        m_pendingEntryStop = false;
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            SendEvent("stopped", {
                {"reason",            "step"},
                {"threadId",          kThreadId},
                {"allThreadsStopped", true},
            });
        }).detach();
    }
}

// ---------------------------------------------------------------------------
// Phase 1 — disconnect
// ---------------------------------------------------------------------------

void DapServer::HandleDisconnect(int seq, const nlohmann::json& /*args*/)
{
    // Clean up the AGDI session before disconnecting.
    if (g_runControl.IsSessionActive()) {
        // If the target is running (GoStep blocking on the main thread), stop
        // it first.  Without this the DLL crashes when AG_Init(AGDI_UNINIT) is
        // called while GoStep is still active.
        ++g_runGeneration;              // suppress stale WaitAndSendStopped
        g_runControl.StopDirect();      // halt target (no-op if already halted)
        g_runControl.UninitAgdiSession();
    }
    g_hexLoader.Unload();
    g_symtab.Clear();

    SendResponse(seq, "disconnect");
    m_disconnecting = true;
}

// ---------------------------------------------------------------------------
// Phase 2+ stubs — respond so the client does not hang
// ---------------------------------------------------------------------------

void DapServer::HandleLaunch(int seq, const nlohmann::json& args)
{
    if (!args.contains("program") || !args["program"].is_string()) {
        SendResponse(seq, "launch", nlohmann::json(nullptr),
                     false, "launch requires 'program' (path to HEX file)");
        return;
    }

    const std::string hexPath = args["program"].get<std::string>();
    const bool noDebug  = args.value("noDebug",  false);
    const bool noErase  = args.value("noErase",  false);
    const bool isFlash  = noDebug;

    LOG("[LAUNCH] Program : %s\n", hexPath.c_str());
    LOG("[LAUNCH] Mode    : %s%s\n",
        isFlash ? "flash-only" : "debug",
        noErase ? " (noErase)"  : " (full erase+program+verify)");

    // If a session is already active, uninit it before starting a new one.
    if (g_runControl.IsSessionActive()) {
        LOG("[AGDI] Closing previous session...\\n");
        g_runControl.UninitAgdiSession();
    }

    if (!g_hexLoader.LoadFile(hexPath)) {
        LOG("[ERROR] Could not parse HEX file: %s\n", hexPath.c_str());
        SendResponse(seq, "launch", nlohmann::json(nullptr),
                     false, "could not parse HEX file: " + hexPath);
        return;
    }
    LOG("[HEX]  Loaded %u bytes at base 0x%04X\n",
        g_hexLoader.ByteCount(), g_hexLoader.BaseAddress());

    // Load symbol table for source-level debugging (optional — no-op if files absent).
    if (!isFlash) {
        g_symtab.Load(hexPath);
    }

    // Run the AGDI registration chain.
    if (!g_runControl.InitAgdiSession(isFlash, noErase)) {
        LOG("[ERROR] AGDI session init failed\n");
        SendResponse(seq, "launch", nlohmann::json(nullptr),
                     false, "AGDI registration chain failed — target not connected?");
        return;
    }

    // Send a successful launch response.
    SendResponse(seq, "launch");

    if (!isFlash) {
        // Debug session: wait for the AG_RESET halt notification.
        if (!g_runControl.WaitForHalt(10000)) {
            LOGV("HandleLaunch: timeout waiting for reset halt\n");
        }

        g_runControl.ReadRegisters();
        uint32_t pc = g_registers.PC();
        LOG("[DEBUG] Target halted at PC=0x%04X\n", pc);

        // Defer the stopped event until configurationDone (DAP protocol
        // requirement — the client ignores stopped events received before
        // configurationDone has been acknowledged).
        m_pendingEntryStop = true;
    } else {
        g_hexLoader.Unload();
        g_symtab.Clear();
        LOG("[LAUNCH] Flash complete — session terminated\n");
        SendEvent("terminated", nlohmann::json(nullptr));
    }
}

void DapServer::HandleSetBreakpoints(int seq, const nlohmann::json& args)
{
    // Extract breakpoint addresses from the request.
    // DAP sends: { source: { path: "..." }, breakpoints: [ { line: N }, ... ] }
    //
    // Resolution order for each breakpoint:
    //   1. 'address' field (explicit hex string) — raw code address, always honoured.
    //   2. 'line' field + source.path  — resolved via the symbol table line map.
    //   3. 'line' field alone (no source path) — treated as a raw code address
    //      (legacy behaviour, useful before source mapping is available).
    std::vector<uint32_t> addresses;
    nlohmann::json verified = nlohmann::json::array();

    // Source file path from the top-level 'source' object (applies to all BPs).
    std::string sourcePath;
    if (args.contains("source") && args["source"].is_object()) {
        sourcePath = args["source"].value("path", "");
    }

    if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
        for (auto& bp : args["breakpoints"]) {
            uint32_t addr = 0;
            bool resolved = false;

            if (bp.contains("address") && bp["address"].is_string()) {
                // Explicit hex address override.
                try {
                    addr = static_cast<uint32_t>(
                        std::stoul(bp["address"].get<std::string>(), nullptr, 0));
                    resolved = true;
                } catch (...) {}
            }

            if (!resolved && bp.contains("line") && bp["line"].is_number_integer()) {
                int line = bp["line"].get<int>();

                // Try symbol table lookup if we have a source path.
                if (!sourcePath.empty() && g_symtab.IsLoaded()) {
                    auto maybeAddr = g_symtab.LookupAddress(sourcePath, line);
                    if (maybeAddr) {
                        addr = *maybeAddr;
                        resolved = true;
                        LOG("[BP]   %s:%d -> 0x%04X\n", sourcePath.c_str(), line, addr);
                    }
                }

                if (!resolved) {
                    // Fallback: treat line number as a raw code address.
                    addr = static_cast<uint32_t>(line);
                }
            }

            addresses.push_back(addr);
        }
    }

    int count = 0;
    if (g_runControl.IsSessionActive()) {
        if (!addresses.empty()) {
            count = g_bpManager.SetFileBreakpoints(sourcePath,
                                                   addresses.data(),
                                                   static_cast<int>(addresses.size()),
                                                   amCODE);
        } else {
            // Empty breakpoints array — clear all BPs for this file.
            g_bpManager.SetFileBreakpoints(sourcePath, nullptr, 0, amCODE);
            LOG("[BP]   Cleared all breakpoints for %s\n", sourcePath.c_str());
        }
    }

    // Build the verified breakpoints response.
    for (size_t i = 0; i < addresses.size(); ++i) {
        bool ok = (static_cast<int>(i) < count);
        verified.push_back({
            {"verified", ok},
            {"id",       static_cast<int>(i + 1)},
        });
    }

    SendResponse(seq, "setBreakpoints", {{"breakpoints", verified}});
}

void DapServer::HandleSetExceptionBreakpoints(int seq, const nlohmann::json& /*args*/)
{
    // No exception breakpoints on bare-metal 8051 — acknowledge as empty.
    SendResponse(seq, "setExceptionBreakpoints", {{"breakpoints", nlohmann::json::array()}});
}

void DapServer::HandleThreads(int seq, const nlohmann::json& /*args*/)
{
    // Single MCU thread always present.
    nlohmann::json body = {
        {"threads", {{{"id", kThreadId}, {"name", "C8051F380"}}}}
    };
    SendResponse(seq, "threads", body);
}

// ---------------------------------------------------------------------------
// Helper: wait for halt on a background thread and send a DAP stopped event.
// ---------------------------------------------------------------------------

static void WaitAndSendStopped(const std::string& reason, uint32_t gen)
{
    if (!g_runControl.WaitForHalt(30000)) {
        LOG("[DEBUG] Timeout waiting for halt (%s)\n", reason.c_str());
    }

    // If a newer run command has been issued while we waited, this halt
    // belongs to a different resume cycle — discard silently.
    if (g_runGeneration.load() != gen) {
        LOG("[DEBUG] Stale halt (gen %u vs current %u) — suppressed\n",
            gen, g_runGeneration.load());
        return;
    }

    // Registers were already read on the main thread (in HwndMsgProc)
    // immediately after GoStep returned.  Do NOT call ReadRegisters here —
    // DLL functions are not thread-safe.
    uint32_t pc = g_registers.PC();
    LOG("[DEBUG] Halted at PC=0x%04X  reason=%s\n", pc, reason.c_str());

    if (g_pServer) {
        g_pServer->SendEvent("stopped", {
            {"reason",            reason},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    }
}

void DapServer::HandleContinue(int seq, const nlohmann::json& /*args*/)
{
    nlohmann::json body = {{"allThreadsContinued", true}};
    SendResponse(seq, "continue", body);

    // Attempt to disable the C8051F380 watchdog timer before releasing the CPU.
    // The WDT has a default timeout of ~95ms after a debug reset, which is shorter
    // than the firmware's hardware initialization sequence.  If the WDT fires before
    // the firmware pets it, the CPU resets and AG_RUNSTOP never fires, causing a
    // 30-second timeout on this side.
    //
    // WDTCN SFR on C8051F380 is at address 0x97 (DATA/SFR space).
    // Disable sequence: write 0xDE then 0xAD to WDTCN.
    // Note: the debug interface writes are not guaranteed to be back-to-back
    // within one clock cycle, so the WDT may not accept the disable.  This is
    // a best-effort attempt; the firmware's own WDT init will take over if it
    // succeeds in running past initialization.
    if (g_agdi.AG_MemAcc) {
        constexpr uint16_t WDTCN_SFR = 0x97;   // C8051F380 watchdog control SFR
        GADR wdtAddr{};
        wdtAddr.Adr    = (static_cast<UL32>(amDATA) << 24) | WDTCN_SFR;
        wdtAddr.nLen   = 1;
        wdtAddr.mSpace = amDATA;

        UC8 wdtDisable1 = 0xDE;
        UC8 wdtDisable2 = 0xAD;
        g_agdi.AG_MemAcc(AG_WRITE, &wdtDisable1, &wdtAddr, 1);
        g_agdi.AG_MemAcc(AG_WRITE, &wdtDisable2, &wdtAddr, 1);
        LOG("[DEBUG] WDT disable sequence written to WDTCN (0x%02X)\n", WDTCN_SFR);
    }

    // Post AG_GOFORBRK to the main thread so the reader thread stays free
    // for receiving DAP messages (e.g. pause).  The main thread's GoStep call
    // blocks inside the DLL's internal message pump, which will dispatch our
    // WM_USER+2 stop-request when HandlePause fires.
    LOG("[DEBUG] Posting GoStep(AG_GOFORBRK) to main thread...\n");
    g_runControl.ResetHaltEvent();
    g_runControl.RequestRun();
    uint32_t gen = ++g_runGeneration;
    std::thread(WaitAndSendStopped, kStopReasonBreakpoint, gen).detach();
}

void DapServer::HandleNext(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "next");

    // Source-level step over:
    // Step instructions in a loop until the source line changes.
    // When the current instruction is a CALL (LCALL/ACALL), set a temporary
    // breakpoint at the return address and run instead of single-stepping
    // through the entire called function.
    // When we detect SP > startSP (inside a called function, e.g. due to
    // hitting a user breakpoint inside the call), use a temp BP at the
    // stack return address to quickly step out.
    uint32_t startPC = g_registers.PC();
    auto startLoc = g_symtab.LookupLine(startPC);

    if (startLoc && g_symtab.IsLoaded()) {
        uint8_t startSP = g_registers.SP();

        for (int i = 0; i < 50000; ++i) {
            uint32_t pc = g_registers.PC();
            uint8_t  sp = g_registers.SP();

            // If we're inside a called function (SP grew), step out quickly
            // by setting a temp BP at the return address on the stack.
            if (sp > startSP) {
                // 8051 LCALL stack layout: [SP]=PCH, [SP-1]=PCL
                // Use 4-byte buffers — DLL may overwrite past requested count.
                UC8 pclBuf[4] = {}, pchBuf[4] = {};
                GADR stkAddr{};
                stkAddr.mSpace = amDATA;
                stkAddr.Adr = (static_cast<UL32>(amDATA) << 24) | sp;
                g_agdi.AG_MemAcc(AG_READ, pchBuf, &stkAddr, 1);
                stkAddr.Adr = (static_cast<UL32>(amDATA) << 24) | (sp - 1);
                g_agdi.AG_MemAcc(AG_READ, pclBuf, &stkAddr, 1);
                uint32_t retAddr = (static_cast<uint32_t>(pchBuf[0]) << 8) | pclBuf[0];

                AG_BP* tempBP = g_bpManager.AddTempBreakpoint(retAddr, amCODE);
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRun();
                g_runControl.WaitForHalt(30000);
                g_bpManager.RemoveTempBreakpoint(tempBP);
                // Registers already read by WM_USER+3 handler.
                continue;
            }

            // Read the opcode at the current PC from CODE space.
            // Use 4-byte buffer — DLL may overwrite past requested count.
            UC8 opBuf[4] = {};
            if (g_agdi.AG_MemAcc) {
                GADR codeAddr{};
                codeAddr.mSpace = amCODE;
                codeAddr.Adr = (static_cast<UL32>(amCODE) << 24) | (pc & 0xFFFF);
                g_agdi.AG_MemAcc(AG_READ, opBuf, &codeAddr, 1);
            }
            UC8 opcode = opBuf[0];

            // Detect CALL instructions:
            //   LCALL addr16 = 0x12 (3 bytes)
            //   ACALL addr11 = xxx1_0001 pattern (2 bytes)
            bool isCall = (opcode == 0x12) || ((opcode & 0x1F) == 0x11);

            if (isCall) {
                // Step over the CALL: set temp BP at the instruction after it.
                uint32_t nextPC = pc + ((opcode == 0x12) ? 3 : 2);
                AG_BP* tempBP = g_bpManager.AddTempBreakpoint(nextPC, amCODE);
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRun();
                g_runControl.WaitForHalt(30000);
                g_bpManager.RemoveTempBreakpoint(tempBP);
                // Registers already read by WM_USER+3 handler.
            } else {
                // Normal single instruction step.
                g_runControl.GoStep(AG_NSTEP, 1, nullptr);
                g_runControl.WaitForHalt(5000);
                g_runControl.ReadRegisters();
            }

            pc = g_registers.PC();

            auto loc = g_symtab.LookupLine(pc);
            if (!loc)
                continue;
            // Same line — keep stepping.
            if (loc->file == startLoc->file && loc->line == startLoc->line)
                continue;
            // Different source line — done.
            break;
        }

        // Already halted and registers read — send stopped event directly.
        uint32_t pc = g_registers.PC();
        LOG("[DEBUG] Source step-over complete at PC=0x%04X\n", pc);
        SendEvent("stopped", {
            {"reason",            kStopReasonStep},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else {
        // No source info — fall back to single instruction step.
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadRegisters();  // safe: GoStep already returned
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    }
}

void DapServer::HandleStepIn(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "stepIn");

    // Source-level step in: step instructions until the source line changes.
    // Unlike step-over, we do NOT check SP — we follow into calls.
    uint32_t startPC = g_registers.PC();
    auto startLoc = g_symtab.LookupLine(startPC);

    if (startLoc && g_symtab.IsLoaded()) {
        for (int i = 0; i < 50000; ++i) {
            g_runControl.GoStep(AG_NSTEP, 1, nullptr);
            g_runControl.WaitForHalt(5000);
            g_runControl.ReadRegisters();

            uint32_t pc = g_registers.PC();
            auto loc = g_symtab.LookupLine(pc);
            if (!loc)
                continue;
            if (loc->file == startLoc->file && loc->line == startLoc->line)
                continue;
            break;
        }

        uint32_t pc = g_registers.PC();
        LOG("[DEBUG] Source step-in complete at PC=0x%04X\n", pc);
        SendEvent("stopped", {
            {"reason",            kStopReasonStep},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else {
        // No source info — single instruction step.
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadRegisters();  // safe: GoStep already returned
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    }
}

void DapServer::HandleStepOut(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "stepOut");

    // Step out: read the return address from the 8051 stack and run until we
    // reach it.  On 8051, LCALL/ACALL push PCL first then PCH:
    //   SP+1 → store PCL, SP+1 → store PCH
    // After LCALL: [SP] = PCH (high byte), [SP-1] = PCL (low byte)
    uint8_t sp = g_registers.SP();

    if (sp >= 2 && g_agdi.AG_MemAcc) {
        // Use 4-byte buffers — DLL may overwrite past requested count.
        UC8 pclBuf[4] = {}, pchBuf[4] = {};
        GADR addr{};
        addr.mSpace = amDATA;

        // Read PCH at [SP] (pushed last = top of stack)
        addr.Adr = (static_cast<UL32>(amDATA) << 24) | sp;
        g_agdi.AG_MemAcc(AG_READ, pchBuf, &addr, 1);

        // Read PCL at [SP-1] (pushed first)
        addr.Adr = (static_cast<UL32>(amDATA) << 24) | (sp - 1);
        g_agdi.AG_MemAcc(AG_READ, pclBuf, &addr, 1);

        uint32_t retAddr = (static_cast<uint32_t>(pchBuf[0]) << 8) | pclBuf[0];
        LOG("[DEBUG] Step out: SP=0x%02X, return address=0x%04X\n", sp, retAddr);

        GADR target{};
        target.Adr    = retAddr & 0xFFFF;
        target.mSpace = amCODE;

        if (g_runControl.GoStep(AG_GOTILADR, 0, &target)) {
            g_runControl.ReadRegisters();  // safe: GoStep already returned
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    } else {
        LOG("[DEBUG] Step out: SP too low (%d) or no AG_MemAcc\n", sp);
        // Fall back to single step.
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadRegisters();  // safe: GoStep already returned
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    }
}

void DapServer::HandlePause(int seq, const nlohmann::json& /*args*/)
{
    // Synchronous pause (java-debug pattern):
    //  1. Send DAP response immediately.
    //  2. Increment generation to suppress the stale continue-waiter.
    //  3. StopDirect() — blocks until AG_STOPRUN returns.  By that time
    //     GOFORBRK has also returned on the main thread, so the DLL is idle.
    //  4. Post WM_USER+4 to main thread for AG_NSTEP(1) register refresh.
    //  5. WaitForHalt — NSTEP triggers AG_CB_INITREGV with real registers.
    //  6. Send StoppedEvent with valid PC.
    SendResponse(seq, "pause");
    LOG("[DEBUG] HandlePause: requesting target stop\n");
    ++g_runGeneration;                       // kill stale continue-waiter
    g_runControl.StopDirect();               // synchronous halt
    LOG("[DEBUG] HandlePause: StopDirect returned — reading registers\n");

    g_runControl.ReadRegisters();            // active register read via RegDsc

    uint32_t pc = g_registers.PC();
    LOG("[DEBUG] HandlePause: halted at PC=0x%04X\n", pc);

    SendEvent("stopped", {
        {"reason",            kStopReasonPause},
        {"threadId",          kThreadId},
        {"allThreadsStopped", true},
    });
}

void DapServer::HandleStackTrace(int seq, const nlohmann::json& /*args*/)
{
    // Single frame from the register cache PC.
    nlohmann::json frame = g_registers.ToStackFrame(1);
    nlohmann::json body = {
        {"stackFrames", {frame}},
        {"totalFrames", 1}
    };
    SendResponse(seq, "stackTrace", body);
}

void DapServer::HandleScopes(int seq, const nlohmann::json& args)
{
    // Scope variablesReference IDs (use 100+ to avoid collision with frame IDs):
    //   99  = Locals (C variables in the current function)
    //   100 = Registers
    //   101 = CODE memory
    //   102 = XDATA memory
    //   103 = DATA memory
    //   104 = IDATA memory
    nlohmann::json scopes = nlohmann::json::array();

    // Only show Locals scope if we have local variable info for the current PC.
    if (g_symtab.IsLoaded()) {
        auto locals = g_symtab.LookupLocals(g_registers.PC());
        if (!locals.empty()) {
            scopes.push_back({{"name", "Locals"}, {"presentationHint", "locals"},
                              {"variablesReference", 99}, {"expensive", false}});
        }
    }

    scopes.push_back({{"name", "Registers"}, {"presentationHint", "registers"}, {"variablesReference", 100}, {"expensive", false}});
    scopes.push_back({{"name", "CODE"},      {"presentationHint", "registers"}, {"variablesReference", 101}, {"expensive", true}});
    scopes.push_back({{"name", "XDATA"},     {"presentationHint", "registers"}, {"variablesReference", 102}, {"expensive", true}});
    scopes.push_back({{"name", "DATA"},      {"presentationHint", "registers"}, {"variablesReference", 103}, {"expensive", true}});
    scopes.push_back({{"name", "IDATA"},     {"presentationHint", "registers"}, {"variablesReference", 104}, {"expensive", true}});

    SendResponse(seq, "scopes", {{"scopes", scopes}});
}

void DapServer::HandleVariables(int seq, const nlohmann::json& args)
{
    int ref = args.value("variablesReference", 0);
    LOG("[DAP]  variables ref=%d\n", ref);

    // Locals scope — read C variables for the current function.
    if (ref == 99 && g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto locals = g_symtab.LookupLocals(g_registers.PC());
        nlohmann::json vars = nlohmann::json::array();
        for (const auto& lv : locals) {
            UC8 rdBuf[4] = {};
            GADR addr{};
            addr.mSpace = lv.mSpace;
            addr.Adr    = (static_cast<UL32>(lv.mSpace) << 24) | (lv.addr & 0xFFFF);
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &addr, 1);
            char valStr[8];
            _snprintf_s(valStr, sizeof(valStr), "0x%02X", rdBuf[0]);
            vars.push_back({
                {"name",               lv.name},
                {"value",              valStr},
                {"variablesReference", 0},
            });
        }
        SendResponse(seq, "variables", {{"variables", vars}});
        return;
    }

    if (ref == 100) {
        // Registers scope — format from the cached RG51.
        auto vars = g_registers.ToVariables();
        LOG("[DAP]  -> variables ref=100 (Registers): %d items\n", (int)vars.size());
        nlohmann::json body = {{"variables", vars}};
        LOG("[DAP]  variables body: %s\n", body.dump().c_str());
        SendResponse(seq, "variables", body);
        return;
    }

    // Memory scopes (101–104): read a page of memory via AG_MemAcc.
    struct { int ref; U16 mSpace; const char* name; UL32 size; } memScopes[] = {
        {101, amCODE,  "CODE",  256},
        {102, amXDATA, "XDATA", 256},
        {103, amDATA,  "DATA",  256},
        {104, amIDATA, "IDATA", 256},
    };

    for (auto& ms : memScopes) {
        if (ref == ms.ref && g_agdi.AG_MemAcc) {
            // Read first 'size' bytes of memory space.
            UC8 buf[256] = {};
            GADR addr{};
            addr.Adr    = (static_cast<UL32>(ms.mSpace) << 24) | 0;
            addr.nLen   = ms.size;
            addr.mSpace = ms.mSpace;
            g_agdi.AG_MemAcc(AG_READ, buf, &addr, ms.size);

            nlohmann::json vars = nlohmann::json::array();
            for (UL32 i = 0; i < ms.size; ++i) {
                char addrStr[12], valStr[8];
                _snprintf_s(addrStr, sizeof(addrStr), "0x%04X", i);
                _snprintf_s(valStr,  sizeof(valStr),  "0x%02X", buf[i]);
                vars.push_back({
                    {"name",               addrStr},
                    {"value",              valStr},
                    {"variablesReference", 0},
                });
            }
            SendResponse(seq, "variables", {{"variables", vars}});
            return;
        }
    }

    // Unknown scope ref.
    SendResponse(seq, "variables", {{"variables", nlohmann::json::array()}});
}

void DapServer::HandleReadMemory(int seq, const nlohmann::json& args)
{
    // DAP readMemory: { memoryReference: "<hex>", offset?: N, count: N }
    // memoryReference encodes (mSpace << 24) | baseAddr.
    if (!args.contains("memoryReference") || !args.contains("count")) {
        SendResponse(seq, "readMemory", nlohmann::json(nullptr), false, "missing parameters");
        return;
    }

    uint32_t memRef = static_cast<uint32_t>(
        std::stoul(args["memoryReference"].get<std::string>(), nullptr, 0));
    int offset = args.value("offset", 0);
    int count  = args["count"].get<int>();
    if (count <= 0 || count > 65536) {
        SendResponse(seq, "readMemory", nlohmann::json(nullptr), false, "invalid count");
        return;
    }

    uint32_t baseAddr = (memRef & 0x00FFFFFF) + static_cast<uint32_t>(offset);
    U16 mSpace = static_cast<U16>((memRef >> 24) & 0xFF);

    std::vector<UC8> buf(static_cast<size_t>(count), 0);
    GADR addr{};
    addr.Adr    = (static_cast<UL32>(mSpace) << 24) | (baseAddr & 0xFFFF);
    addr.nLen   = static_cast<UL32>(count);
    addr.mSpace = mSpace;

    U32 ret = AG_OK;
    if (g_agdi.AG_MemAcc) {
        ret = g_agdi.AG_MemAcc(AG_READ, buf.data(), &addr, static_cast<UL32>(count));
    }

    // Encode as base64 for the DAP response.
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((count + 2) / 3 * 4);
    for (int i = 0; i < count; i += 3) {
        uint32_t triple = static_cast<uint32_t>(buf[i]) << 16;
        if (i + 1 < count) triple |= static_cast<uint32_t>(buf[i + 1]) << 8;
        if (i + 2 < count) triple |= static_cast<uint32_t>(buf[i + 2]);
        encoded += b64[(triple >> 18) & 0x3F];
        encoded += b64[(triple >> 12) & 0x3F];
        encoded += (i + 1 < count) ? b64[(triple >> 6) & 0x3F] : '=';
        encoded += (i + 2 < count) ? b64[triple & 0x3F] : '=';
    }

    SendResponse(seq, "readMemory", {
        {"address",          std::to_string(baseAddr)},
        {"unreadableBytes",  (ret != AG_OK) ? count : 0},
        {"data",             encoded},
    });
}

void DapServer::HandleEvaluate(int seq, const nlohmann::json& args)
{
    // DAP evaluate: watch expressions, debug console, hover.
    // Supports: 8051 register names, PUBLIC symbol names, hex addresses (0xNNNN).
    std::string expr = args.value("expression", "");
    if (expr.empty()) {
        SendResponse(seq, "evaluate", nlohmann::json(nullptr), false, "empty expression");
        return;
    }

    // Normalize to uppercase for register matching.
    std::string upper;
    for (char c : expr) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Try 8051 register names.
    // Read from the register cache (already populated after halt).
    g_runControl.ReadRegisters();

    struct RegInfo { const char* name; };
    // SFR registers readable via AG_MemAcc from DATA space.
    struct SfrInfo { const char* name; uint16_t addr; };
    static const SfrInfo sfrs[] = {
        {"ACC",  0xE0}, {"A",    0xE0}, {"B",    0xF0}, {"SP",   0x81},
        {"PSW",  0xD0}, {"DPL",  0x82}, {"DPH",  0x83}, {"IE",   0xA8},
        {"IP",   0xB8}, {"TCON", 0x88}, {"TMOD", 0x89}, {"P0",   0x80},
        {"P1",   0x90}, {"P2",   0xA0}, {"P3",   0xB0}, {"SCON0",0x98},
        {"SBUF0",0x99}, {"WDTCN",0x97}, {"PCON", 0x87},
    };

    // Check PC first.
    if (upper == "PC") {
        uint32_t pc = g_registers.PC();
        char buf[12];
        _snprintf_s(buf, sizeof(buf), "0x%04X", pc);
        SendResponse(seq, "evaluate", {
            {"result", std::string(buf)},
            {"variablesReference", 0},
        });
        return;
    }

    // Check R0-R7.
    if (upper.size() == 2 && upper[0] == 'R' && upper[1] >= '0' && upper[1] <= '7') {
        int idx = upper[1] - '0';
        // Read Rn via AG_RegAcc.
        GVAL val{};
        if (g_agdi.AG_RegAcc) {
            g_agdi.AG_RegAcc(AG_READ, idx, &val);
        }
        char buf[8];
        _snprintf_s(buf, sizeof(buf), "0x%02X", val.uc);
        SendResponse(seq, "evaluate", {
            {"result", std::string(buf)},
            {"variablesReference", 0},
        });
        return;
    }

    // Check DPTR (16-bit combo of DPH:DPL).
    if (upper == "DPTR") {
        if (g_agdi.AG_MemAcc) {
            UC8 dplBuf[4] = {}, dphBuf[4] = {};
            GADR a{};
            a.mSpace = amDATA;
            a.Adr = (static_cast<UL32>(amDATA) << 24) | 0x82;
            g_agdi.AG_MemAcc(AG_READ, dplBuf, &a, 1);
            a.Adr = (static_cast<UL32>(amDATA) << 24) | 0x83;
            g_agdi.AG_MemAcc(AG_READ, dphBuf, &a, 1);
            uint16_t dptr = (static_cast<uint16_t>(dphBuf[0]) << 8) | dplBuf[0];
            char buf[8];
            _snprintf_s(buf, sizeof(buf), "0x%04X", dptr);
            SendResponse(seq, "evaluate", {
                {"result", std::string(buf)},
                {"variablesReference", 0},
                {"memoryReference", std::string(buf)},
            });
            return;
        }
    }

    // Check SFR names.
    for (const auto& sfr : sfrs) {
        if (upper != sfr.name) continue;
        if (g_agdi.AG_MemAcc) {
            UC8 rdBuf[4] = {};
            GADR a{};
            a.mSpace = amDATA;
            a.Adr = (static_cast<UL32>(amDATA) << 24) | sfr.addr;
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
            char buf[8];
            _snprintf_s(buf, sizeof(buf), "0x%02X", rdBuf[0]);
            SendResponse(seq, "evaluate", {
                {"result", std::string(buf)},
                {"variablesReference", 0},
            });
            return;
        }
    }

    // Try local variable lookup (C variables in the current function).
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto lv = g_symtab.LookupLocalByName(expr, g_registers.PC());
        if (lv) {
            UC8 rdBuf[4] = {};
            GADR a{};
            a.mSpace = lv->mSpace;
            a.Adr    = (static_cast<UL32>(lv->mSpace) << 24) | (lv->addr & 0xFFFF);
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
            char buf[8];
            _snprintf_s(buf, sizeof(buf), "0x%02X", rdBuf[0]);
            SendResponse(seq, "evaluate", {
                {"result", std::string(buf)},
                {"variablesReference", 0},
            });
            return;
        }
    }

    // Try symbol lookup (PUBLIC symbols from m51).
    if (g_symtab.IsLoaded()) {
        // Search for a symbol matching the expression by name.
        auto addr = g_symtab.LookupSymbolByName(upper);
        if (addr) {
            char buf[12];
            _snprintf_s(buf, sizeof(buf), "0x%04X", *addr);
            SendResponse(seq, "evaluate", {
                {"result",            std::string(buf)},
                {"variablesReference", 0},
                {"memoryReference",   std::string(buf)},
            });
            return;
        }
    }

    // Try hex address: 0xNNNN — read one byte from CODE space.
    if (expr.size() > 2 && expr[0] == '0' && (expr[1] == 'x' || expr[1] == 'X')) {
        try {
            uint32_t addr = static_cast<uint32_t>(std::stoul(expr, nullptr, 16));
            if (g_agdi.AG_MemAcc) {
                UC8 rdBuf[4] = {};
                GADR a{};
                a.mSpace = amCODE;
                a.Adr = (static_cast<UL32>(amCODE) << 24) | (addr & 0xFFFF);
                g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
                char buf[8];
                _snprintf_s(buf, sizeof(buf), "0x%02X", rdBuf[0]);
                SendResponse(seq, "evaluate", {
                    {"result", std::string(buf)},
                    {"variablesReference", 0},
                    {"memoryReference", expr},
                });
                return;
            }
        } catch (...) {}
    }

    SendResponse(seq, "evaluate", nlohmann::json(nullptr), false,
                 "Cannot evaluate: " + expr);
}

void DapServer::HandleSource(int seq, const nlohmann::json& args)
{
    // DAP source request: client wants the content of a source file.
    // If the source has a path, read it from disk.
    if (args.contains("source") && args["source"].is_object()) {
        std::string path = args["source"].value("path", "");
        if (!path.empty()) {
            std::ifstream f(path);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                SendResponse(seq, "source", {
                    {"content",  ss.str()},
                    {"mimeType", "text/plain"},
                });
                return;
            }
        }
    }

    // No path or file not found.
    SendResponse(seq, "source", {{"content", "// source not available"}});
}
