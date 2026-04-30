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
#include "disasm8051.h"
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
// Shadow call stack — maintained by step operations.
//
// The 8051 hardware stack is unreliable for unwinding because Keil C51 uses
// PUSH/POP for register saves and tail-call optimizations remove return
// addresses.  Instead, we track function entry/exit during step operations
// to build an accurate call stack.
// ---------------------------------------------------------------------------
struct ShadowFrame {
    std::string funcName;
    uint32_t    pc;       // PC at the time we stopped in this frame
    std::string file;
    int         line;
};
static std::vector<ShadowFrame> g_shadowStack;

// Update the shadow call stack after any step/halt.  Call this with the
// current PC after registers have been read.
static void ShadowStackUpdate(uint32_t pc)
{
    std::string curFunc = g_symtab.LookupSymbol(pc);
    auto loc = g_symtab.LookupLine(pc);

    ShadowFrame curFrame;
    curFrame.funcName = curFunc;
    curFrame.pc       = pc;
    curFrame.file     = loc ? loc->file : "";
    curFrame.line     = loc ? loc->line : 0;

    if (g_shadowStack.empty()) {
        // First stop — just push.
        g_shadowStack.push_back(curFrame);
        LOG("[SHADOW] init: %s @ 0x%04X\n", curFunc.c_str(), pc);
        return;
    }

    // Check if we're in the same function as the top of stack.
    if (!curFunc.empty() && curFunc == g_shadowStack.back().funcName) {
        // Same function — just update PC/line.
        g_shadowStack.back().pc   = pc;
        g_shadowStack.back().file = curFrame.file;
        g_shadowStack.back().line = curFrame.line;
        return;
    }

    // Function changed.  Determine if this is a step-in, step-out, or
    // a tail-call/sibling-call.

    // Check if we returned to a function already on the stack.
    for (int i = static_cast<int>(g_shadowStack.size()) - 2; i >= 0; --i) {
        if (!curFunc.empty() && curFunc == g_shadowStack[i].funcName) {
            // Returned to frame i — pop everything above it.
            size_t popCount = g_shadowStack.size() - 1 - static_cast<size_t>(i);
            LOG("[SHADOW] return to %s (pop %zu frames)\n", curFunc.c_str(), popCount);
            g_shadowStack.resize(static_cast<size_t>(i) + 1);
            g_shadowStack.back().pc   = pc;
            g_shadowStack.back().file = curFrame.file;
            g_shadowStack.back().line = curFrame.line;
            return;
        }
    }

    // Not on the stack — this is a step-in or tail-call into a new function.
    // Check if the previous top function calls this function (step-in).
    // Or if this is a tail-call (the previous function jumped to a new one).
    g_shadowStack.push_back(curFrame);
    LOG("[SHADOW] push %s @ 0x%04X (depth %zu)\n", curFunc.c_str(), pc,
        g_shadowStack.size());
}

// Reset the shadow stack (e.g. on continue/launch where we lose tracking).
static void ShadowStackReset()
{
    g_shadowStack.clear();
    LOG("[SHADOW] reset\n");
}

// Return the byte length of an 8051 instruction given its opcode.
static inline uint8_t Opcode8051Length(uint8_t opcode)
{
    return k8051InstructionLength[opcode];
}

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
    else if (command == "setVariable")      HandleSetVariable      (seq, args);
    else if (command == "setExpression")    HandleSetExpression    (seq, args);
    else if (command == "writeMemory")      HandleWriteMemory      (seq, args);
    else if (command == "disassemble")             HandleDisassemble             (seq, args);
    else if (command == "setInstructionBreakpoints") HandleSetInstructionBreakpoints(seq, args);
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
        ShadowStackReset();
        ShadowStackUpdate(g_registers.PC());
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
    ShadowStackReset();

    SendResponse(seq, "disconnect");
    m_disconnecting = true;
}

// ---------------------------------------------------------------------------
// Phase 2+ stubs — respond so the client does not hang
// ---------------------------------------------------------------------------

void DapServer::HandleLaunch(int seq, const nlohmann::json& args)
{
    auto sendNonBlockingLaunchFailure = [&](const std::string& reason) {
        SendEvent("output", {
            {"category", "console"},
            {"output",   std::string("[launch failed] ") + reason + "\n"},
        });
        SendResponse(seq, "launch");
        SendEvent("terminated", {
            {"restart", false}
        });
        SendEvent("exited", {
            {"exitCode", 1}
        });
    };

    if (!args.contains("program") || !args["program"].is_string()) {
        sendNonBlockingLaunchFailure("launch requires 'program' (path to HEX file)");
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
        sendNonBlockingLaunchFailure("could not parse HEX file: " + hexPath);
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
        std::string dllErr = g_runControl.LastDllError();
        std::string errMsg = dllErr.empty()
            ? "AGDI registration chain failed — target not connected? Run scripts/reset_silabs.ps1 and retry."
            : (dllErr + " Run scripts/reset_silabs.ps1 and retry.");
        LOG("[ERROR] AGDI session init failed: %s\n", errMsg.c_str());
        sendNonBlockingLaunchFailure(errMsg);
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
        LOG("[DEBUG] Target halted at PC=0x%04X after reset\n", pc);

        // Determine the application entry point.
        // Priority: explicit 'startAddress' launch arg > HEX base address.
        // When the app is linked above 0x0000 (e.g. 0x2400 for app firmware
        // that coexists with a bootloader), the target resets into bootloader
        // code that has no debug symbols.  Run-to the entry point so the
        // first halt is inside the application code.
        uint32_t entryPoint = g_hexLoader.BaseAddress();
        if (args.contains("startAddress")) {
            try {
                const std::string& addrStr = args["startAddress"].get<std::string>();
                entryPoint = static_cast<uint32_t>(std::stoul(addrStr, nullptr, 0));
                LOG("[DEBUG] startAddress override: 0x%04X\n", entryPoint);
            } catch (...) {}
        }

        if (entryPoint > pc) {
            LOG("[DEBUG] Running to app entry 0x%04X (reset vector was 0x%04X)\n", entryPoint, pc);

            // Disable the C8051F34x/F38x watchdog before letting the CPU run.
            // Without this the WDT (~95 ms post-reset) fires before the
            // bootloader can reach 0x2400, the chip resets, AG_GOTILADR
            // returns immediately and PC reads back 0x0000.
            //
            // The 0xDE/0xAD WDTCN sequence requires both writes to land
            // within 4 system clocks of each other — going through AGDI
            // USB transactions that's impossible.  Instead we clear
            // PCA0MD.WDTE (bit 6) which permanently disables the WDT
            // with no timing constraint.  Read-modify-write to preserve
            // the other PCA mode bits (CIDL, WDLCK, CPS[2:0], ECF).
            if (g_agdi.AG_MemAcc) {
                constexpr uint16_t PCA0MD_SFR = 0xD9;
                constexpr uint8_t  WDTE_BIT   = 0x40;
                GADR pcaAddr{};
                pcaAddr.Adr    = (static_cast<UL32>(amDATA) << 24) | PCA0MD_SFR;
                pcaAddr.nLen   = 1;
                pcaAddr.mSpace = amDATA;
                UC8 pcaVal = 0;
                g_agdi.AG_MemAcc(AG_READ, &pcaVal, &pcaAddr, 1);
                UC8 newPca = static_cast<UC8>(pcaVal & ~WDTE_BIT);
                g_agdi.AG_MemAcc(AG_WRITE, &newPca, &pcaAddr, 1);
                LOG("[DEBUG] WDT disabled: PCA0MD 0x%02X -> 0x%02X (cleared WDTE)\n",
                    pcaVal, newPca);
            }

            g_runControl.ResetHaltEvent();
            g_runControl.RequestRunToAddr(entryPoint);
            if (!g_runControl.WaitForHalt(5000)) {
                LOG("[WARN] Timeout waiting for run-to entry 0x%04X\n", entryPoint);
            }
            g_runControl.ReadRegisters();
            pc = g_registers.PC();
        }
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

    int armed = 0;
    if (g_runControl.IsSessionActive()) {
        if (!addresses.empty()) {
            armed = g_bpManager.SetFileBreakpoints(sourcePath,
                                                   addresses.data(),
                                                   static_cast<int>(addresses.size()),
                                                   amCODE);
        } else {
            // Empty breakpoints array — clear all BPs for this file.
            g_bpManager.SetFileBreakpoints(sourcePath, nullptr, 0, amCODE);
            LOG("[BP]   Cleared all breakpoints for %s\n", sourcePath.c_str());
        }
    }

    // Check if a BP's address is in the armed list.
    auto isArmed = [&](uint32_t addr) -> bool {
        for (AG_BP* p = g_bpManager.HeadPtr() ? *g_bpManager.HeadPtr() : nullptr;
             p; p = p->next) {
            if (p->Adr == addr) return true;
        }
        return false;
    };

    // Build the verified breakpoints response.
    for (size_t i = 0; i < addresses.size(); ++i) {
        bool ok = g_runControl.IsSessionActive() && isArmed(addresses[i]);
        nlohmann::json bpEntry = {
            {"verified", ok},
            {"id",       static_cast<int>(i + 1)},
        };
        if (!ok && g_runControl.IsSessionActive()) {
            bpEntry["message"] = "Hardware limit: max " +
                                 std::to_string(kMaxUserBreakpoints) + " breakpoints";
        }
        verified.push_back(bpEntry);
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

    // On breakpoint/continue halt, the shadow stack may be stale.
    // Reset and re-init from the current PC.
    ShadowStackReset();
    ShadowStackUpdate(pc);

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

    // Disable the C8051F34x/F38x watchdog before releasing the CPU.
    // The WDT has a default timeout of ~95 ms after a debug reset, which is
    // shorter than the firmware's hardware-initialization sequence; if it
    // fires before the firmware can pet it the CPU resets and AG_RUNSTOP
    // never fires, causing a 30-second timeout on this side.
    //
    // The 0xDE/0xAD WDTCN sequence requires both writes within 4 system
    // clocks — impossible across two AGDI USB transactions.  Clearing
    // PCA0MD.WDTE (bit 6) disables the WDT in a single SFR write with no
    // timing constraint.  Read-modify-write to preserve other PCA bits.
    if (g_agdi.AG_MemAcc) {
        constexpr uint16_t PCA0MD_SFR = 0xD9;
        constexpr uint8_t  WDTE_BIT   = 0x40;
        GADR pcaAddr{};
        pcaAddr.Adr    = (static_cast<UL32>(amDATA) << 24) | PCA0MD_SFR;
        pcaAddr.nLen   = 1;
        pcaAddr.mSpace = amDATA;
        UC8 pcaVal = 0;
        g_agdi.AG_MemAcc(AG_READ, &pcaVal, &pcaAddr, 1);
        if (pcaVal & WDTE_BIT) {
            UC8 newPca = static_cast<UC8>(pcaVal & ~WDTE_BIT);
            g_agdi.AG_MemAcc(AG_WRITE, &newPca, &pcaAddr, 1);
            LOG("[DEBUG] WDT disabled before continue: PCA0MD 0x%02X -> 0x%02X\n",
                pcaVal, newPca);
        }
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

void DapServer::HandleNext(int seq, const nlohmann::json& args)
{
    SendResponse(seq, "next");
    int64_t t0 = StepTimerUs();

    // Instruction granularity: one opcode, no source-line loop.
    if (args.value("granularity", "statement") == "instruction") {
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
        return;
    }

    // Source-level step over via AG_GOTILADR — mirrors µVision's F10.
    //
    // AG_GOTILADR tells the DLL to set an internal temp breakpoint at the
    // target address and run the target at full 48 MHz speed.  The DLL
    // manages its own temp BP, so we don't need to allocate from our pool.
    // This saves USB round trips for BP set/clear and doesn't consume one
    // of the 4 hardware breakpoint slots from our side.
    //
    // For function-end lines (where the next source line is in a different
    // function), we scan for RET and GOTILADR to it, then NSTEP(1) to
    // actually execute the RET and land in the caller.
    uint32_t startPC = g_registers.PC();
    auto startLoc = g_symtab.LookupLine(startPC);
    auto nextAddr = g_symtab.NextLineAddr(startPC);

    if (startLoc && g_symtab.IsLoaded() && nextAddr) {
        std::string curFunc  = g_symtab.LookupSymbol(startPC);
        std::string nextFunc = g_symtab.LookupSymbol(*nextAddr);

        if (!curFunc.empty() && curFunc != nextFunc) {
            // At function end — scan for RET/RETI from current PC to function end.
            uint32_t funcEnd = *nextAddr;
            uint32_t scanLen = funcEnd - startPC;
            if (scanLen > 1024) scanLen = 1024;

            std::vector<UC8> codeBuf(scanLen, 0);
            GADR codeAddr{};
            codeAddr.mSpace = amCODE;
            codeAddr.Adr = (static_cast<UL32>(amCODE) << 24) | startPC;
            g_agdi.AG_MemAcc(AG_READ, codeBuf.data(), &codeAddr, scanLen);

            uint32_t retOff = 0;
            bool foundRet = false;
            while (retOff < scanLen) {
                uint8_t op = codeBuf[retOff];
                if (op == 0x22 || op == 0x32) { foundRet = true; break; }
                uint8_t len = k8051InstructionLength[op];
                if (len == 0) len = 1;
                retOff += len;
            }

            if (foundRet) {
                uint32_t targetAddr = startPC + retOff;
                LOG("[STEP] Next at function end (%s), RET at 0x%04X\n",
                    curFunc.c_str(), targetAddr);
                // GOTILADR to the RET, then NSTEP(1) to execute it.
                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRunToAddr(targetAddr);
                bool halted = g_runControl.WaitForHalt(5000);
                if (!halted) {
                    LOG("[STEP] Step-over timeout — forcing halt\n");
                    g_runControl.RequestStop();
                    g_runControl.WaitForHalt(5000);
                } else {
                    g_runControl.GoStep(AG_NSTEP, 1, nullptr);
                    g_runControl.ReadPcSpCached();
                }
            } else {
                LOG("[STEP] Next crosses function boundary (%s -> %s), no RET found\n",
                    curFunc.c_str(), nextFunc.c_str());
                // Fall back to GOTILADR to the next line address.
                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRunToAddr(*nextAddr);
                bool halted = g_runControl.WaitForHalt(5000);
                if (!halted) {
                    g_runControl.RequestStop();
                    g_runControl.WaitForHalt(5000);
                }
            }
        } else {
            // Normal step-over within same function.
            // NSTEP(1) loop with CALL detection.  Handles conditional
            // branches correctly — unlike GOTILADR which sets a BP at
            // the next line address that branches can skip past.
            int safety = 50;
            while (safety-- > 0) {
                uint32_t pc = g_registers.PC();
                // Read opcode at current PC to check for CALL.
                UC8 opBuf[1];
                GADR readAddr{};
                readAddr.mSpace = amCODE;
                readAddr.Adr = (static_cast<UL32>(amCODE) << 24) | pc;
                g_agdi.AG_MemAcc(AG_READ, opBuf, &readAddr, 1);
                uint8_t op = opBuf[0];

                bool isCall = (op == 0x12) || ((op & 0x1F) == 0x11);
                if (isCall) {
                    // Step OVER the call: GOTILADR to the return address
                    // (instruction after the CALL).  Safe because PC is
                    // AT the CALL — the function will return here.
                    uint8_t callLen = k8051InstructionLength[op];
                    uint32_t returnAddr = pc + callLen;
                    ++g_runGeneration;
                    g_runControl.ResetHaltEvent();
                    g_runControl.RequestRunToAddr(returnAddr);
                    bool halted = g_runControl.WaitForHalt(10000);
                    if (!halted) {
                        g_runControl.RequestStop();
                        g_runControl.WaitForHalt(5000);
                        break;
                    }
                    g_runControl.ReadPcCached();
                } else {
                    g_runControl.GoStep(AG_NSTEP, 1, nullptr);
                    g_runControl.ReadPcCached();
                }

                uint32_t newPC = g_registers.PC();
                auto loc = g_symtab.LookupLine(newPC);
                if (!loc || loc->file != startLoc->file || loc->line != startLoc->line) {
                    break;
                }
            }
        }

        uint32_t pc = g_registers.PC();
        int64_t elapsed = StepTimerUs() - t0;
        ShadowStackUpdate(pc);
        LOG("[STEP] Step-over 0x%04X -> 0x%04X  (%lld us)\n", startPC, pc, elapsed);
        SendEvent("stopped", {
            {"reason",            kStopReasonStep},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else if (startLoc && g_symtab.IsLoaded()) {
        // Have source info but no next line — last line in symtab.
        // Fall back to a small instruction-level step.
        g_runControl.GoStep(AG_NSTEP, 1, nullptr);
        g_runControl.ReadPcSpCached();
        uint32_t pc = g_registers.PC();
        ShadowStackUpdate(pc);
        LOG("[STEP] Step-over (last line fallback) 0x%04X -> 0x%04X\n", startPC, pc);
        SendEvent("stopped", {
            {"reason",            kStopReasonStep},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else {
        // No source info — fall back to single instruction step.
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    }
}

void DapServer::HandleStepIn(int seq, const nlohmann::json& args)
{
    SendResponse(seq, "stepIn");
    int64_t t0 = StepTimerUs();

    // Instruction granularity: one opcode, no source-line loop.
    if (args.value("granularity", "statement") == "instruction") {
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
        return;
    }

    // Source-level step in via code scan + AG_GOTILADR.
    //
    // 1. Read code bytes between PC and the next source line.
    // 2. Scan for LCALL/ACALL instructions.
    // 3. If NO CALL found: identical to step-over — AG_GOTILADR to next line.
    // 4. If CALL found: step to the CALL instruction with NSTEP(N), then
    //    one more NSTEP to enter the function, then check if we're on a
    //    new source line.
    uint32_t startPC = g_registers.PC();
    auto startLoc = g_symtab.LookupLine(startPC);
    auto nextAddr = g_symtab.NextLineAddr(startPC);

    if (startLoc && g_symtab.IsLoaded() && nextAddr) {
        uint32_t dist = *nextAddr - startPC;
        if (dist > 64) dist = 64;  // sanity cap

        // Read code bytes from PC to next line boundary.
        UC8 codeBuf[68] = {};
        GADR codeAddr{};
        codeAddr.mSpace = amCODE;
        codeAddr.Adr = (static_cast<UL32>(amCODE) << 24) | startPC;
        g_agdi.AG_MemAcc(AG_READ, codeBuf, &codeAddr, dist);

        // Scan for CALL instructions and count instructions before the CALL.
        int callOffset = -1;       // byte offset of CALL within codeBuf
        int instrsBefore = 0;      // number of instructions before the CALL
        uint32_t off = 0;
        while (off < dist) {
            uint8_t op = codeBuf[off];
            bool isCall = (op == 0x12) || ((op & 0x1F) == 0x11);
            if (isCall) {
                callOffset = static_cast<int>(off);
                break;
            }
            uint8_t len = k8051InstructionLength[op];
            if (len == 0) len = 1;  // safety
            off += len;
            ++instrsBefore;
        }

        if (callOffset < 0) {
            // No CALL found — step instruction-by-instruction until the
            // source line changes.  NSTEP(1) follows conditional branches
            // correctly, unlike AG_GOTILADR which sets a BP at a single
            // address that branches (JNB, JB, CJNE, DJNZ, …) can skip.
            int safety = 20;
            while (safety-- > 0) {
                g_runControl.GoStep(AG_NSTEP, 1, nullptr);
                g_runControl.ReadPcCached();
                uint32_t pc2 = g_registers.PC();
                auto loc2 = g_symtab.LookupLine(pc2);
                if (!loc2 || loc2->file != startLoc->file || loc2->line != startLoc->line) {
                    break;
                }
            }
        } else {
            // CALL found — step to it, then into it.
            if (instrsBefore > 0) {
                g_runControl.GoStep(AG_NSTEP, instrsBefore, nullptr);
            }
            // One more step enters the called function.
            g_runControl.GoStep(AG_NSTEP, 1, nullptr);
            g_runControl.ReadPcCached();

            // We're now inside the called function.  If the first instruction
            // doesn't map to a source line (e.g. function prologue), advance
            // to the first real source line — but ONLY within the same function.
            uint32_t pc = g_registers.PC();
            std::string enteredFunc = g_symtab.LookupSymbol(pc);
            auto loc = g_symtab.LookupLine(pc);
            LOG("[STEP] Step-in: entered 0x%04X func=%s\n", pc, enteredFunc.c_str());

            if (!loc || (loc->file == startLoc->file && loc->line == startLoc->line)) {
                auto postCallNext = g_symtab.NextLineAddr(pc);
                // Only prologue-skip if the next line is still in the same function.
                std::string nextLineFunc = postCallNext ? g_symtab.LookupSymbol(*postCallNext) : "";
                if (postCallNext && !enteredFunc.empty() && enteredFunc == nextLineFunc) {
                    LOG("[STEP] Step-in: prologue skip to 0x%04X (same func %s)\n",
                        *postCallNext, enteredFunc.c_str());
                    ++g_runGeneration;
                    g_runControl.ResetHaltEvent();
                    g_runControl.RequestRunToAddr(*postCallNext);
                    bool halted = g_runControl.WaitForHalt(5000);
                    if (!halted) {
                        g_runControl.RequestStop();
                        g_runControl.WaitForHalt(5000);
                    }
                } else {
                    LOG("[STEP] Step-in: no prologue skip (nextLine 0x%04X in %s, not %s)\n",
                        postCallNext ? *postCallNext : 0, nextLineFunc.c_str(), enteredFunc.c_str());
                }
            }
        }

        uint32_t pc = g_registers.PC();
        int64_t elapsed = StepTimerUs() - t0;
        ShadowStackUpdate(pc);
        LOG("[STEP] Step-in 0x%04X -> 0x%04X  (%lld us)\n", startPC, pc, elapsed);
        SendEvent("stopped", {
            {"reason",            kStopReasonStep},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else {
        // No source info or no next line — single instruction step.
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
    }
}

void DapServer::HandleStepOut(int seq, const nlohmann::json& args)
{
    SendResponse(seq, "stepOut");
    int64_t t0 = StepTimerUs();

    // Instruction granularity: one opcode.
    if (args.value("granularity", "statement") == "instruction") {
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
            uint32_t gen = ++g_runGeneration;
            std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
        }
        return;
    }

    // Source-level step out: scan the current function for RET (0x22) and
    // RETI (0x32) instructions, set temp breakpoints on all of them, then
    // GOFORBRK.  After the RET BP fires, single-step once to execute the
    // RET and land in the caller.
    //
    // For single-RET functions (common case), we can use AG_GOTILADR
    // instead, which avoids consuming a hardware breakpoint slot.
    //
    // We cannot read the return address from the 8051 internal stack via
    // AG_MemAcc (the DLL returns zeros for DATA/IDATA), so we scan for
    // RET instructions instead.
    uint32_t startPC = g_registers.PC();

    if (g_symtab.IsLoaded()) {
        // Find function boundaries from symbol table.
        std::string funcName = g_symtab.LookupSymbol(startPC);
        uint32_t funcStart = startPC;
        if (!funcName.empty()) {
            auto addr = g_symtab.LookupSymbolByName(funcName);
            if (addr) funcStart = *addr;
        }

        // Find the next function's start address to determine our function's end.
        uint32_t funcEnd = funcStart + 4096;  // safety cap
        {
            auto nextFuncAddr = g_symtab.NextSymbolAddr(funcStart);
            if (nextFuncAddr) funcEnd = *nextFuncAddr;
        }

        // Scan from current PC (not function start) to function end.
        uint32_t scanFrom = startPC;
        uint32_t codeLen = funcEnd - scanFrom;
        if (codeLen > 8192) codeLen = 8192;  // generous cap

        LOG("[STEP] Step-out: func=%s funcStart=0x%04X funcEnd=0x%04X scanFrom=0x%04X len=%u\n",
            funcName.c_str(), funcStart, funcEnd, scanFrom, codeLen);

        // Read code bytes from PC to function end.
        std::vector<UC8> codeBuf(codeLen, 0);
        GADR codeAddr{};
        codeAddr.mSpace = amCODE;
        codeAddr.Adr = (static_cast<UL32>(amCODE) << 24) | scanFrom;
        g_agdi.AG_MemAcc(AG_READ, codeBuf.data(), &codeAddr, codeLen);

        // Scan for all RET/RETI and exit jumps using instruction length table.
        std::vector<uint32_t> retAddrs;   // RET/RETI addresses (need NSTEP after)
        std::vector<uint32_t> exitAddrs;  // LJMP/AJMP targets outside function (no NSTEP needed)
        uint32_t off = 0;
        while (off < codeLen) {
            uint8_t op = codeBuf[off];
            if (op == 0x22 || op == 0x32) {
                retAddrs.push_back(scanFrom + off);
            } else if (op == 0x02 && off + 2 < codeLen) {
                // LJMP addr16: target = (byte1 << 8) | byte2
                uint32_t target = (static_cast<uint32_t>(codeBuf[off + 1]) << 8) | codeBuf[off + 2];
                if (target < funcStart || target >= funcEnd) {
                    exitAddrs.push_back(target);
                    LOG("[STEP] Step-out: LJMP at 0x%04X -> 0x%04X (exit)\n", scanFrom + off, target);
                }
            } else if ((op & 0x1F) == 0x01 && off + 1 < codeLen) {
                // AJMP: target = ((op >> 5) << 8 | byte1) within same 2K page
                uint32_t instrAddr = scanFrom + off;
                uint32_t page = (instrAddr + 2) & 0xF800;
                uint32_t target = page | (static_cast<uint32_t>(op >> 5) << 8) | codeBuf[off + 1];
                if (target < funcStart || target >= funcEnd) {
                    exitAddrs.push_back(target);
                    LOG("[STEP] Step-out: AJMP at 0x%04X -> 0x%04X (exit)\n", scanFrom + off, target);
                }
            }
            uint8_t len = k8051InstructionLength[op];
            if (len == 0) len = 1;
            off += len;
        }

        for (uint32_t ra : retAddrs) {
            LOG("[STEP] Step-out: RET at 0x%04X\n", ra);
        }

        if (!retAddrs.empty()) {
            bool halted = false;

            if (retAddrs.size() == 1) {
                // Single RET — use AG_GOTILADR (no hardware BP consumed).
                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRunToAddr(retAddrs[0]);
                halted = g_runControl.WaitForHalt(10000);
            } else {
                // Multiple RETs — set temp BPs on all and GOFORBRK.
                std::vector<AG_BP*> tempBPs;
                for (uint32_t ra : retAddrs) {
                    AG_BP* bp = g_bpManager.AddTempBreakpoint(ra, amCODE);
                    if (bp) tempBPs.push_back(bp);
                }

                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRun();
                halted = g_runControl.WaitForHalt(10000);

                for (auto* bp : tempBPs)
                    g_bpManager.RemoveTempBreakpoint(bp);
            }

            if (!halted) {
                LOG("[STEP] Step-out timeout — forcing halt\n");
                g_runControl.RequestStop();
                g_runControl.WaitForHalt(5000);
            } else {
                // On the RET — single-step to execute it and land in caller.
                g_runControl.GoStep(AG_NSTEP, 1, nullptr);
                g_runControl.ReadPcSpCached();
            }

            uint32_t pc = g_registers.PC();
            int64_t elapsed = StepTimerUs() - t0;
            ShadowStackUpdate(pc);
            LOG("[STEP] Step-out 0x%04X -> 0x%04X  (%lld us)\n", startPC, pc, elapsed);
            SendEvent("stopped", {
                {"reason",            kStopReasonStep},
                {"threadId",          kThreadId},
                {"allThreadsStopped", true},
            });
        } else if (!exitAddrs.empty()) {
            // No RET, but found LJMP/AJMP exits (tail-call optimization).
            // Run to the jump target — no NSTEP needed because the jump
            // itself transfers control out of the function.
            LOG("[STEP] Step-out: no RET, using %zu exit jump target(s)\n", exitAddrs.size());
            bool halted = false;

            if (exitAddrs.size() == 1) {
                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRunToAddr(exitAddrs[0]);
                halted = g_runControl.WaitForHalt(10000);
            } else {
                std::vector<AG_BP*> tempBPs;
                for (uint32_t ea : exitAddrs) {
                    AG_BP* bp = g_bpManager.AddTempBreakpoint(ea, amCODE);
                    if (bp) tempBPs.push_back(bp);
                }
                ++g_runGeneration;
                g_runControl.ResetHaltEvent();
                g_runControl.RequestRun();
                halted = g_runControl.WaitForHalt(10000);
                for (auto* bp : tempBPs)
                    g_bpManager.RemoveTempBreakpoint(bp);
            }

            if (!halted) {
                LOG("[STEP] Step-out timeout — forcing halt\n");
                g_runControl.RequestStop();
                g_runControl.WaitForHalt(5000);
            }

            // We're at the tail-call target.  The original function exited
            // via LJMP.  The user can step-out again to return to the caller.
            uint32_t pc = g_registers.PC();
            int64_t elapsed = StepTimerUs() - t0;
            ShadowStackUpdate(pc);
            LOG("[STEP] Step-out (tail-call) 0x%04X -> 0x%04X  (%lld us)\n", startPC, pc, elapsed);
            SendEvent("stopped", {
                {"reason",            kStopReasonStep},
                {"threadId",          kThreadId},
                {"allThreadsStopped", true},
            });
        } else {
            LOG("[STEP] Step-out: no RET found in function (first 32 bytes from 0x%04X):\n", scanFrom);
            char hexline[128] = {};
            int pos = 0;
            for (uint32_t i = 0; i < codeLen && i < 32; ++i) {
                pos += snprintf(hexline + pos, sizeof(hexline) - pos, "%02X ", codeBuf[i]);
            }
            LOG("[STEP]   %s\n", hexline);

            // Last resort: single step.
            if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
                g_runControl.ReadPcSpCached();
                uint32_t gen = ++g_runGeneration;
                std::thread(WaitAndSendStopped, kStopReasonStep, gen).detach();
            }
        }
    } else {
        LOG("[DEBUG] Step out: no symbol table, falling back to single step\n");
        if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
            g_runControl.ReadPcSpCached();
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
    ShadowStackReset();
    ShadowStackUpdate(pc);
    LOG("[DEBUG] HandlePause: halted at PC=0x%04X\n", pc);

    SendEvent("stopped", {
        {"reason",            kStopReasonPause},
        {"threadId",          kThreadId},
        {"allThreadsStopped", true},
    });
}

void DapServer::HandleStackTrace(int seq, const nlohmann::json& args)
{
    int startFrame = args.value("startFrame", 0);
    int levels     = args.value("levels", 20);
    if (startFrame < 0) startFrame = 0;
    if (levels <= 0) levels = 20;

    auto BaseName = [](const std::string& path) -> std::string {
        size_t slash = path.find_last_of("\\/");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    };

    auto MakeFrame = [&](int frameId, const std::string& funcName,
                         uint32_t pc, const std::string& file, int line) -> nlohmann::json {
        char pcBuf[12];
        _snprintf_s(pcBuf, sizeof(pcBuf), "0x%04X", pc & 0xFFFF);
        std::string frameName = funcName.empty() ? std::string(pcBuf) : funcName;

        if (!file.empty() && line > 0) {
            return {
                {"id",     frameId},
                {"name",   frameName},
                {"source", {{"name", BaseName(file)}, {"path", file}, {"presentationHint", "normal"}}},
                {"line",   line},
                {"column", 1},
                {"instructionPointerReference", std::string(pcBuf)},
            };
        }

        return {
            {"id",     frameId},
            {"name",   frameName},
            {"line",   0},
            {"column", 0},
            {"instructionPointerReference", std::string(pcBuf)},
        };
    };

    // Build frames from the shadow call stack (maintained by step operations).
    // The shadow stack is ordered bottom-to-top: [0]=deepest, [last]=current.
    // DAP expects top-of-stack first.
    nlohmann::json frames = nlohmann::json::array();
    int totalFrames = static_cast<int>(g_shadowStack.size());

    if (totalFrames == 0) {
        // Fallback: no shadow stack — just show current PC.
        uint32_t pc = g_registers.PC();
        std::string sym = g_symtab.LookupSymbol(pc);
        auto loc = g_symtab.LookupLine(pc);
        frames.push_back(MakeFrame(1, sym, pc,
                                   loc ? loc->file : "", loc ? loc->line : 0));
        totalFrames = 1;
    } else {
        if (startFrame > totalFrames) startFrame = totalFrames;
        int endFrame = ((startFrame + levels) < totalFrames) ? (startFrame + levels) : totalFrames;
        for (int i = startFrame; i < endFrame; ++i) {
            // Shadow stack is bottom-to-top; DAP wants top-first.
            int shadowIdx = totalFrames - 1 - i;
            const auto& sf = g_shadowStack[static_cast<size_t>(shadowIdx)];
            frames.push_back(MakeFrame(i + 1, sf.funcName, sf.pc, sf.file, sf.line));
        }
    }

    nlohmann::json body = {
        {"stackFrames", frames},
        {"totalFrames", totalFrames}
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

    // Show Locals scope if we have local variable info for the current PC.
    // expensive=true tells VS Code to collapse by default and only fetch on expand.
    if (g_symtab.IsLoaded()) {
        auto locals = g_symtab.LookupLocals(g_registers.PC());
        if (!locals.empty()) {
            scopes.push_back({{"name", "Locals"}, {"presentationHint", "locals"},
                              {"variablesReference", 99}, {"expensive", true}});
        }
    }

    // Registers scope removed — VS Code fetches it even when collapsed,
    // adding unnecessary overhead on every step.
    // scopes.push_back({{"name", "Registers"}, {"presentationHint", "registers"}, {"variablesReference", 100}, {"expensive", true}});

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
            // Read up to 4 bytes — size inferred from m51 address gaps.
            // C51 stores multi-byte values big-endian.
            uint8_t sz = lv.size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            UC8 rdBuf[4] = {};
            GADR addr{};
            addr.mSpace = lv.mSpace;
            addr.Adr    = (static_cast<UL32>(lv.mSpace) << 24) | (lv.addr & 0xFFFF);
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &addr, sz);
            // Build big-endian hex string with appropriate width.
            char valStr[12];
            if (sz == 4)
                _snprintf_s(valStr, sizeof(valStr), "0x%02X%02X%02X%02X",
                             rdBuf[0], rdBuf[1], rdBuf[2], rdBuf[3]);
            else if (sz == 2)
                _snprintf_s(valStr, sizeof(valStr), "0x%02X%02X",
                             rdBuf[0], rdBuf[1]);
            else
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

    // Register names and locals use the cached values (already populated
    // after halt / step completion).  No need to re-read from hardware.

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
            uint8_t sz = lv->size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            UC8 rdBuf[4] = {};
            GADR a{};
            a.mSpace = lv->mSpace;
            a.Adr    = (static_cast<UL32>(lv->mSpace) << 24) | (lv->addr & 0xFFFF);
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, sz);
            char buf[12];
            if (sz == 4)
                _snprintf_s(buf, sizeof(buf), "0x%02X%02X%02X%02X",
                             rdBuf[0], rdBuf[1], rdBuf[2], rdBuf[3]);
            else if (sz == 2)
                _snprintf_s(buf, sizeof(buf), "0x%02X%02X",
                             rdBuf[0], rdBuf[1]);
            else
                _snprintf_s(buf, sizeof(buf), "0x%02X", rdBuf[0]);
            SendResponse(seq, "evaluate", {
                {"result", std::string(buf)},
                {"variablesReference", 0},
            });
            return;
        }
    }

    // Try `bit` variable lookup (C51 bit-addressable PUBLIC symbols).
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto bv = g_symtab.LookupBitByName(expr);
        if (bv) {
            UC8 rdBuf[1] = {};
            GADR a{};
            a.mSpace = amDATA;
            a.Adr    = (static_cast<UL32>(amDATA) << 24) | bv->byteAddr;
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
            uint8_t bitVal = (rdBuf[0] >> bv->bitIndex) & 0x1;
            SendResponse(seq, "evaluate", {
                {"result", std::string(bitVal ? "true" : "false")},
                {"variablesReference", 0},
            });
            return;
        }
    }

    // Try global variable lookup (PUBLIC data symbols from m51).
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto gv = g_symtab.LookupGlobalByName(expr);
        if (gv) {
            uint8_t sz = gv->size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            UC8 rdBuf[4] = {};
            GADR a{};
            a.mSpace = gv->mSpace;
            a.Adr    = (static_cast<UL32>(gv->mSpace) << 24) | (gv->addr & 0xFFFF);
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, sz);
            char buf[12];
            if (sz == 4)
                _snprintf_s(buf, sizeof(buf), "0x%02X%02X%02X%02X",
                             rdBuf[0], rdBuf[1], rdBuf[2], rdBuf[3]);
            else if (sz == 2)
                _snprintf_s(buf, sizeof(buf), "0x%02X%02X",
                             rdBuf[0], rdBuf[1]);
            else
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

// ---------------------------------------------------------------------------
// Parse a hex value string ("0xFF", "0x1234", "255", "42") into bytes.
// Returns the number of bytes written (1–4), or 0 on failure.
// Output is big-endian (MSB first) matching C51 memory layout.
// ---------------------------------------------------------------------------
static int ParseHexValue(const std::string& input, UC8 outBuf[4], int maxBytes)
{
    if (input.empty()) return 0;
    uint32_t val = 0;
    try {
        if (input.size() > 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
            val = static_cast<uint32_t>(std::stoul(input, nullptr, 16));
        else
            val = static_cast<uint32_t>(std::stoul(input, nullptr, 0));
    } catch (...) { return 0; }

    // Determine minimum byte width needed.
    int needed = 1;
    if (val > 0xFF)     needed = 2;
    if (val > 0xFFFF)   needed = 3;
    if (val > 0xFFFFFF) needed = 4;
    int sz = (needed > maxBytes) ? maxBytes : needed;

    // Store big-endian (C51 convention).
    for (int i = 0; i < sz; ++i)
        outBuf[i] = static_cast<UC8>((val >> (8 * (sz - 1 - i))) & 0xFF);
    return sz;
}

// ---------------------------------------------------------------------------
// Write a value to a memory-mapped variable/SFR via AG_MemAcc.
// Returns true on success.
// ---------------------------------------------------------------------------
static bool WriteMemoryValue(uint16_t mSpace, uint32_t addr, const UC8* buf, uint8_t sz)
{
    if (!g_agdi.AG_MemAcc || sz == 0) return false;
    GADR a{};
    a.mSpace = mSpace;
    a.Adr    = (static_cast<UL32>(mSpace) << 24) | (addr & 0xFFFF);
    g_agdi.AG_MemAcc(AG_WRITE, const_cast<UC8*>(buf), &a, sz);
    return true;
}

// ---------------------------------------------------------------------------
// Read back a value after writing, formatted as hex string.
// ---------------------------------------------------------------------------
static std::string ReadBackHex(uint16_t mSpace, uint32_t addr, uint8_t sz)
{
    UC8 rdBuf[4] = {};
    GADR a{};
    a.mSpace = mSpace;
    a.Adr    = (static_cast<UL32>(mSpace) << 24) | (addr & 0xFFFF);
    g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, sz);
    char buf[12];
    if (sz == 4)
        _snprintf_s(buf, sizeof(buf), "0x%02X%02X%02X%02X", rdBuf[0], rdBuf[1], rdBuf[2], rdBuf[3]);
    else if (sz == 2)
        _snprintf_s(buf, sizeof(buf), "0x%02X%02X", rdBuf[0], rdBuf[1]);
    else
        _snprintf_s(buf, sizeof(buf), "0x%02X", rdBuf[0]);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// setVariable — edit a variable value in the Variables panel.
// DAP args: { variablesReference, name, value }
// ---------------------------------------------------------------------------
void DapServer::HandleSetVariable(int seq, const nlohmann::json& args)
{
    int ref = args.value("variablesReference", 0);
    std::string name  = args.value("name",  "");
    std::string value = args.value("value", "");
    LOG("[DAP]  setVariable ref=%d name=%s value=%s\n", ref, name.c_str(), value.c_str());

    if (name.empty() || value.empty()) {
        SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "missing name or value");
        return;
    }

    // SFR address table (same as HandleEvaluate).
    struct SfrInfo { const char* name; uint16_t addr; };
    static const SfrInfo sfrs[] = {
        {"ACC",  0xE0}, {"A",    0xE0}, {"B",    0xF0}, {"SP",   0x81},
        {"PSW",  0xD0}, {"DPL",  0x82}, {"DPH",  0x83}, {"IE",   0xA8},
        {"IP",   0xB8}, {"TCON", 0x88}, {"TMOD", 0x89}, {"P0",   0x80},
        {"P1",   0x90}, {"P2",   0xA0}, {"P3",   0xB0}, {"SCON0",0x98},
        {"SBUF0",0x99}, {"WDTCN",0x97}, {"PCON", 0x87},
    };

    std::string upper;
    for (char c : name) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // --- Registers scope (ref=100) ---
    if (ref == 100) {
        UC8 wrBuf[4] = {};

        // PC
        if (upper == "PC") {
            if (!g_agdi.AG_RegAcc) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "AGDI not loaded");
                return;
            }
            int n = ParseHexValue(value, wrBuf, 2);
            if (n == 0) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid value");
                return;
            }
            GVAL gv{};
            gv.u32 = (static_cast<uint32_t>(wrBuf[0]) << 8) | wrBuf[1];
            if (n == 1) gv.u32 = wrBuf[0];
            g_agdi.AG_RegAcc(AG_WRITE, 0x500, &gv);
            // Read back.
            GVAL rb{};
            g_agdi.AG_RegAcc(AG_READ, 0x500, &rb);
            char buf[12];
            _snprintf_s(buf, sizeof(buf), "0x%04X", rb.u32 & 0xFFFF);
            SendResponse(seq, "setVariable", {{"value", std::string(buf)}});
            return;
        }

        // R0-R7
        if (upper.size() == 2 && upper[0] == 'R' && upper[1] >= '0' && upper[1] <= '7') {
            if (!g_agdi.AG_RegAcc) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "AGDI not loaded");
                return;
            }
            int idx = upper[1] - '0';
            int n = ParseHexValue(value, wrBuf, 1);
            if (n == 0) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid value");
                return;
            }
            GVAL gv{};
            gv.uc = wrBuf[0];
            g_agdi.AG_RegAcc(AG_WRITE, idx, &gv);
            // Read back.
            GVAL rb{};
            g_agdi.AG_RegAcc(AG_READ, idx, &rb);
            char buf[8];
            _snprintf_s(buf, sizeof(buf), "0x%02X", rb.uc);
            SendResponse(seq, "setVariable", {{"value", std::string(buf)}});
            return;
        }

        // SFRs (SP, ACC, B, PSW, DPL, DPH, etc.) — written via AG_MemAcc to DATA space.
        for (const auto& sfr : sfrs) {
            if (upper != sfr.name) continue;
            int n = ParseHexValue(value, wrBuf, 1);
            if (n == 0) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid value");
                return;
            }
            WriteMemoryValue(amDATA, sfr.addr, wrBuf, 1);
            std::string rb = ReadBackHex(amDATA, sfr.addr, 1);
            SendResponse(seq, "setVariable", {{"value", rb}});
            return;
        }

        SendResponse(seq, "setVariable", nlohmann::json(nullptr), false,
                     "unknown register: " + name);
        return;
    }

    // --- Locals scope (ref=99) ---
    if (ref == 99 && g_symtab.IsLoaded()) {
        auto lv = g_symtab.LookupLocalByName(name, g_registers.PC());
        if (lv) {
            uint8_t sz = lv->size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            UC8 wrBuf[4] = {};
            int n = ParseHexValue(value, wrBuf, sz);
            if (n == 0) {
                SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid value");
                return;
            }
            // Pad to target size (MSB=0 for smaller input).
            UC8 padded[4] = {};
            int offset = sz - n;
            for (int i = 0; i < n; ++i) padded[offset + i] = wrBuf[i];
            WriteMemoryValue(lv->mSpace, lv->addr, padded, sz);
            std::string rb = ReadBackHex(lv->mSpace, lv->addr, sz);
            SendResponse(seq, "setVariable", {{"value", rb}});
            return;
        }
    }

    // --- Memory scopes (ref=101..104) ---
    struct { int ref; U16 mSpace; } memScopes[] = {
        {101, amCODE}, {102, amXDATA}, {103, amDATA}, {104, amIDATA},
    };
    for (auto& ms : memScopes) {
        if (ref != ms.ref) continue;
        // name is the hex address string "0xNNNN".
        uint32_t addr = 0;
        try { addr = static_cast<uint32_t>(std::stoul(name, nullptr, 16)); }
        catch (...) {
            SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid address");
            return;
        }
        UC8 wrBuf[4] = {};
        int n = ParseHexValue(value, wrBuf, 1);
        if (n == 0) {
            SendResponse(seq, "setVariable", nlohmann::json(nullptr), false, "invalid value");
            return;
        }
        WriteMemoryValue(ms.mSpace, addr, wrBuf, 1);
        std::string rb = ReadBackHex(ms.mSpace, addr, 1);
        SendResponse(seq, "setVariable", {{"value", rb}});
        return;
    }

    SendResponse(seq, "setVariable", nlohmann::json(nullptr), false,
                 "cannot set variable: " + name);
}

// ---------------------------------------------------------------------------
// setExpression — edit a watch expression value.
// DAP args: { expression, value, frameId? }
// Resolves the expression the same way HandleEvaluate does, then writes.
// ---------------------------------------------------------------------------
void DapServer::HandleSetExpression(int seq, const nlohmann::json& args)
{
    std::string expr  = args.value("expression", "");
    std::string value = args.value("value", "");
    LOG("[DAP]  setExpression expr=%s value=%s\n", expr.c_str(), value.c_str());

    if (expr.empty() || value.empty()) {
        SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "missing expression or value");
        return;
    }

    std::string upper;
    for (char c : expr) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    struct SfrInfo { const char* name; uint16_t addr; };
    static const SfrInfo sfrs[] = {
        {"ACC",  0xE0}, {"A",    0xE0}, {"B",    0xF0}, {"SP",   0x81},
        {"PSW",  0xD0}, {"DPL",  0x82}, {"DPH",  0x83}, {"IE",   0xA8},
        {"IP",   0xB8}, {"TCON", 0x88}, {"TMOD", 0x89}, {"P0",   0x80},
        {"P1",   0x90}, {"P2",   0xA0}, {"P3",   0xB0}, {"SCON0",0x98},
        {"SBUF0",0x99}, {"WDTCN",0x97}, {"PCON", 0x87},
    };

    UC8 wrBuf[4] = {};

    // PC
    if (upper == "PC" && g_agdi.AG_RegAcc) {
        int n = ParseHexValue(value, wrBuf, 2);
        if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
        GVAL gv{};
        gv.u32 = (n == 1) ? wrBuf[0] : ((static_cast<uint32_t>(wrBuf[0]) << 8) | wrBuf[1]);
        g_agdi.AG_RegAcc(AG_WRITE, 0x500, &gv);
        GVAL rb{};
        g_agdi.AG_RegAcc(AG_READ, 0x500, &rb);
        char buf[12]; _snprintf_s(buf, sizeof(buf), "0x%04X", rb.u32 & 0xFFFF);
        SendResponse(seq, "setExpression", {{"value", std::string(buf)}});
        return;
    }

    // R0-R7
    if (upper.size() == 2 && upper[0] == 'R' && upper[1] >= '0' && upper[1] <= '7' && g_agdi.AG_RegAcc) {
        int idx = upper[1] - '0';
        int n = ParseHexValue(value, wrBuf, 1);
        if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
        GVAL gv{}; gv.uc = wrBuf[0];
        g_agdi.AG_RegAcc(AG_WRITE, idx, &gv);
        GVAL rb{}; g_agdi.AG_RegAcc(AG_READ, idx, &rb);
        char buf[8]; _snprintf_s(buf, sizeof(buf), "0x%02X", rb.uc);
        SendResponse(seq, "setExpression", {{"value", std::string(buf)}});
        return;
    }

    // DPTR (16-bit: DPH:DPL)
    if (upper == "DPTR" && g_agdi.AG_MemAcc) {
        int n = ParseHexValue(value, wrBuf, 2);
        if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
        uint16_t dptr = (n == 1) ? wrBuf[0] : ((static_cast<uint16_t>(wrBuf[0]) << 8) | wrBuf[1]);
        UC8 dpl = static_cast<UC8>(dptr & 0xFF);
        UC8 dph = static_cast<UC8>((dptr >> 8) & 0xFF);
        WriteMemoryValue(amDATA, 0x82, &dpl, 1);
        WriteMemoryValue(amDATA, 0x83, &dph, 1);
        std::string rb = ReadBackHex(amDATA, 0x82, 1);
        std::string rbh = ReadBackHex(amDATA, 0x83, 1);
        // Reconstruct DPTR display.
        UC8 rdl[4]={}, rdh[4]={};
        GADR a{}; a.mSpace = amDATA;
        a.Adr = (static_cast<UL32>(amDATA) << 24) | 0x82; g_agdi.AG_MemAcc(AG_READ, rdl, &a, 1);
        a.Adr = (static_cast<UL32>(amDATA) << 24) | 0x83; g_agdi.AG_MemAcc(AG_READ, rdh, &a, 1);
        char buf[8]; _snprintf_s(buf, sizeof(buf), "0x%04X", (static_cast<uint16_t>(rdh[0]) << 8) | rdl[0]);
        SendResponse(seq, "setExpression", {{"value", std::string(buf)}});
        return;
    }

    // SFRs
    for (const auto& sfr : sfrs) {
        if (upper != sfr.name) continue;
        int n = ParseHexValue(value, wrBuf, 1);
        if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
        WriteMemoryValue(amDATA, sfr.addr, wrBuf, 1);
        std::string rb = ReadBackHex(amDATA, sfr.addr, 1);
        SendResponse(seq, "setExpression", {{"value", rb}});
        return;
    }

    // Local variable
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto lv = g_symtab.LookupLocalByName(expr, g_registers.PC());
        if (lv) {
            uint8_t sz = lv->size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            int n = ParseHexValue(value, wrBuf, sz);
            if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
            UC8 padded[4] = {};
            int offset = sz - n;
            for (int i = 0; i < n; ++i) padded[offset + i] = wrBuf[i];
            WriteMemoryValue(lv->mSpace, lv->addr, padded, sz);
            std::string rb = ReadBackHex(lv->mSpace, lv->addr, sz);
            SendResponse(seq, "setExpression", {{"value", rb}});
            return;
        }
    }

    // Bit variable (C51 `bit` PUBLIC symbols). Accepts 0/1/false/true (any case).
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto bv = g_symtab.LookupBitByName(expr);
        if (bv) {
            int bitVal = -1;
            std::string vlow;
            for (char c : value) vlow += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if      (vlow == "0" || vlow == "false") bitVal = 0;
            else if (vlow == "1" || vlow == "true")  bitVal = 1;
            else {
                // Allow hex/decimal numeric forms too (non-zero -> 1).
                UC8 tmp[4] = {};
                int n = ParseHexValue(value, tmp, 1);
                if (n > 0) bitVal = (tmp[0] != 0) ? 1 : 0;
            }
            if (bitVal < 0) {
                SendResponse(seq, "setExpression", nlohmann::json(nullptr), false,
                             "invalid bit value (use 0/1/true/false)");
                return;
            }

            // Read-modify-write the containing byte in DATA space.
            GADR a{};
            a.mSpace = amDATA;
            a.Adr    = (static_cast<UL32>(amDATA) << 24) | bv->byteAddr;
            UC8 rdBuf[4] = {};
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
            UC8 mask = static_cast<UC8>(1u << bv->bitIndex);
            UC8 newByte = bitVal ? (rdBuf[0] | mask)
                                 : (rdBuf[0] & static_cast<UC8>(~mask));
            UC8 wrByte = newByte;
            g_agdi.AG_MemAcc(AG_WRITE, &wrByte, &a, 1);

            // Read back to confirm.
            std::memset(rdBuf, 0, sizeof(rdBuf));
            g_agdi.AG_MemAcc(AG_READ, rdBuf, &a, 1);
            uint8_t finalBit = (rdBuf[0] >> bv->bitIndex) & 0x1;
            SendResponse(seq, "setExpression", {
                {"value", std::string(finalBit ? "true" : "false")},
            });
            return;
        }
    }

    // Global variable
    if (g_symtab.IsLoaded() && g_agdi.AG_MemAcc) {
        auto gv = g_symtab.LookupGlobalByName(expr);
        if (gv) {
            uint8_t sz = gv->size;
            if (sz < 1) sz = 1;
            if (sz > 4) sz = 4;
            int n = ParseHexValue(value, wrBuf, sz);
            if (n == 0) { SendResponse(seq, "setExpression", nlohmann::json(nullptr), false, "invalid value"); return; }
            UC8 padded[4] = {};
            int offset = sz - n;
            for (int i = 0; i < n; ++i) padded[offset + i] = wrBuf[i];
            WriteMemoryValue(gv->mSpace, gv->addr, padded, sz);
            std::string rb = ReadBackHex(gv->mSpace, gv->addr, sz);
            SendResponse(seq, "setExpression", {{"value", rb}});
            return;
        }
    }

    SendResponse(seq, "setExpression", nlohmann::json(nullptr), false,
                 "cannot set: " + expr);
}

// ---------------------------------------------------------------------------
// writeMemory \u2014 write bytes to target memory (hex Memory Inspector editing).
// DAP args: { memoryReference, offset?, data (base64) }
// ---------------------------------------------------------------------------
void DapServer::HandleWriteMemory(int seq, const nlohmann::json& args)
{
    if (!args.contains("memoryReference") || !args.contains("data")) {
        SendResponse(seq, "writeMemory", nlohmann::json(nullptr), false, "missing parameters");
        return;
    }

    uint32_t memRef = static_cast<uint32_t>(
        std::stoul(args["memoryReference"].get<std::string>(), nullptr, 0));
    int offset = args.value("offset", 0);
    std::string b64data = args["data"].get<std::string>();

    // Decode base64.
    auto b64val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<UC8> decoded;
    decoded.reserve(b64data.size() * 3 / 4);
    for (size_t i = 0; i + 3 < b64data.size(); i += 4) {
        int a = b64val(b64data[i]), b = b64val(b64data[i+1]);
        int c = b64val(b64data[i+2]), d = b64val(b64data[i+3]);
        if (a < 0 || b < 0) break;
        decoded.push_back(static_cast<UC8>((a << 2) | (b >> 4)));
        if (c >= 0) decoded.push_back(static_cast<UC8>(((b & 0xF) << 4) | (c >> 2)));
        if (d >= 0) decoded.push_back(static_cast<UC8>(((c & 0x3) << 6) | d));
    }

    if (decoded.empty()) {
        SendResponse(seq, "writeMemory", nlohmann::json(nullptr), false, "empty data");
        return;
    }

    uint32_t baseAddr = (memRef & 0x00FFFFFF) + static_cast<uint32_t>(offset);
    U16 mSpace = static_cast<U16>((memRef >> 24) & 0xFF);

    LOG("[DAP]  writeMemory mSpace=0x%04X addr=0x%04X count=%zu\n",
        mSpace, baseAddr & 0xFFFF, decoded.size());

    if (!g_agdi.AG_MemAcc) {
        SendResponse(seq, "writeMemory", nlohmann::json(nullptr), false, "AGDI not loaded");
        return;
    }

    GADR addr{};
    addr.Adr    = (static_cast<UL32>(mSpace) << 24) | (baseAddr & 0xFFFF);
    addr.nLen   = static_cast<UL32>(decoded.size());
    addr.mSpace = mSpace;
    g_agdi.AG_MemAcc(AG_WRITE, decoded.data(), &addr, static_cast<UL32>(decoded.size()));

    SendResponse(seq, "writeMemory", {
        {"bytesWritten", static_cast<int>(decoded.size())},
    });
}

// ---------------------------------------------------------------------------
// Disassemble
// ---------------------------------------------------------------------------

void DapServer::HandleDisassemble(int seq, const nlohmann::json& args)
{
    // DAP disassemble request:
    //   memoryReference: string  — hex address of the instruction (from stackFrame
    //                              instructionPointerReference, e.g. "0x1234")
    //   offset?:            int  — byte offset added to memoryReference
    //   instructionOffset?: int  — instruction count offset (may be negative for
    //                              context above current PC)
    //   instructionCount:   int  — number of instructions to return

    if (!args.contains("memoryReference") || !args.contains("instructionCount")) {
        SendResponse(seq, "disassemble", nlohmann::json(nullptr), false, "missing parameters");
        return;
    }

    uint16_t baseAddr = static_cast<uint16_t>(
        std::stoul(args["memoryReference"].get<std::string>(), nullptr, 0));
    int byteOffset  = args.value("offset", 0);
    int instrOffset = args.value("instructionOffset", 0);
    int instrCount  = args["instructionCount"].get<int>();

    if (instrCount <= 0 || instrCount > 1024) instrCount = 64;

    baseAddr = static_cast<uint16_t>(static_cast<int>(baseAddr) + byteOffset);

    // For negative instructionOffset, scan backwards by at most
    // |instrOffset|*3 bytes (3 = maximum 8051 instruction length).
    int instrBefore = (instrOffset < 0) ? -instrOffset : 0;
    int backBytes   = instrBefore * 3;

    int startAddr = static_cast<int>(baseAddr) - backBytes;
    if (startAddr < 0) startAddr = 0;

    // Total bytes to read: backward region + enough forward bytes for all requested instructions.
    int readCount = (static_cast<int>(baseAddr) - startAddr) + instrCount * 3 + 3;
    if (startAddr + readCount > 0x10000)
        readCount = 0x10000 - startAddr;
    if (readCount <= 0) {
        SendResponse(seq, "disassemble", {{"instructions", nlohmann::json::array()}});
        return;
    }

    // Read CODE memory.
    std::vector<uint8_t> mem(static_cast<size_t>(readCount), 0xFF);
    if (g_agdi.AG_MemAcc) {
        GADR addr{};
        addr.Adr    = (static_cast<UL32>(amCODE) << 24) | static_cast<uint16_t>(startAddr);
        addr.nLen   = static_cast<UL32>(readCount);
        addr.mSpace = amCODE;
        g_agdi.AG_MemAcc(AG_READ, mem.data(), &addr, static_cast<UL32>(readCount));
    }

    // Disassemble instructions sequentially from startAddr.
    struct Instr {
        uint16_t    codeAddr;
        uint8_t     len;
        int         byteOff;  // index into mem[]
        std::string text;
    };
    std::vector<Instr> instrs;
    instrs.reserve(static_cast<size_t>(instrBefore + instrCount + 4));

    int byteOff = 0;
    uint16_t pc = static_cast<uint16_t>(startAddr);

    while (byteOff < readCount) {
        // Pass up to 3 bytes to the disassembler; pad with 0xFF if near end.
        uint8_t tmp[3] = {0xFF, 0xFF, 0xFF};
        int avail = readCount - byteOff;
        if (avail > 3) avail = 3;
        std::memcpy(tmp, mem.data() + byteOff, static_cast<size_t>(avail));

        uint8_t len = 0;
        std::string text = Disasm8051(tmp, pc, len);

        instrs.push_back({pc, len, byteOff, std::move(text)});
        pc       = static_cast<uint16_t>(pc + len);
        byteOff += static_cast<int>(len);

        // Stop once we've collected enough instructions past baseAddr.
        if (static_cast<int>(pc) > static_cast<int>(baseAddr) + instrCount * 3 + 3 &&
            static_cast<int>(instrs.size()) >= instrBefore + instrCount + 1) {
            break;
        }
    }

    // Find the instruction starting at exactly baseAddr.
    int baseIdx = 0;
    for (int i = 0; i < static_cast<int>(instrs.size()); ++i) {
        if (instrs[i].codeAddr == baseAddr) {
            baseIdx = i;
            break;
        }
    }

    // Apply instructionOffset (negative = go up, positive = go down).
    int startIdx = baseIdx + instrOffset;
    if (startIdx < 0) startIdx = 0;

    // Build the DAP response array.
    nlohmann::json instructions = nlohmann::json::array();

    for (int i = startIdx;
         i < static_cast<int>(instrs.size()) && static_cast<int>(instructions.size()) < instrCount;
         ++i)
    {
        const auto& ins = instrs[i];
        char addrBuf[12];
        snprintf(addrBuf, sizeof(addrBuf), "0x%04X", ins.codeAddr);

        // Hex byte string for instructionBytes ("12 34 56").
        char bytesBuf[12] = "";
        for (int b = 0; b < static_cast<int>(ins.len) &&
             ins.byteOff + b < readCount; ++b)
        {
            char tmp[5];
            snprintf(tmp, sizeof(tmp), b == 0 ? "%02X" : " %02X",
                     mem[static_cast<size_t>(ins.byteOff + b)]);
            strncat(bytesBuf, tmp, sizeof(bytesBuf) - std::strlen(bytesBuf) - 1);
        }

        nlohmann::json entry = {
            {"address",          std::string(addrBuf)},
            {"instruction",      ins.text},
            {"instructionBytes", std::string(bytesBuf)},
        };

        // Attach source location when available.
        auto loc = g_symtab.LookupLine(ins.codeAddr);
        if (loc) {
            entry["location"] = {{"name", loc->file}, {"path", loc->file}};
            entry["line"]     = loc->line;
        }

        instructions.push_back(std::move(entry));
    }

    // Pad with placeholder entries if we ran out of address space.
    while (static_cast<int>(instructions.size()) < instrCount) {
        uint16_t nextAddr = 0;
        if (!instructions.empty()) {
            nextAddr = static_cast<uint16_t>(
                std::stoul(instructions.back()["address"].get<std::string>(), nullptr, 16) + 1u);
        }
        char addrBuf[12];
        snprintf(addrBuf, sizeof(addrBuf), "0x%04X", nextAddr);
        instructions.push_back({
            {"address",          std::string(addrBuf)},
            {"instruction",      "??"},
            {"instructionBytes", ""},
        });
        if (nextAddr == 0xFFFF) break;
    }

    LOG("[DAP]  disassemble base=0x%04X instrOffset=%d count=%d -> %zu instrs\n",
        baseAddr, instrOffset, instrCount, instructions.size());

    SendResponse(seq, "disassemble", {{"instructions", instructions}});
}

// ---------------------------------------------------------------------------
// SetInstructionBreakpoints
// ---------------------------------------------------------------------------

void DapServer::HandleSetInstructionBreakpoints(int seq, const nlohmann::json& args)
{
    // DAP setInstructionBreakpoints request:
    //   breakpoints: [ { instructionReference: string, offset?: number }, ... ]
    // instructionReference is a hex address string (same format as
    // stackFrame.instructionPointerReference, e.g. "0x1234").
    // Instruction BPs share the kMaxUserBreakpoints hardware pool with file BPs;
    // file BPs are armed first, so instruction BPs may be unverified if the
    // pool is already full.

    nlohmann::json bpResults = nlohmann::json::array();

    if (!args.contains("breakpoints") || !args["breakpoints"].is_array()) {
        // Empty / missing array — clear all instruction BPs.
        if (g_runControl.IsSessionActive())
            g_bpManager.SetInstructionBreakpoints(nullptr, 0, amCODE);
        SendResponse(seq, "setInstructionBreakpoints", {{"breakpoints", bpResults}});
        return;
    }

    const auto& bpList = args["breakpoints"];
    std::vector<uint32_t> addresses;
    addresses.reserve(bpList.size());

    for (const auto& bp : bpList) {
        std::string ref = bp.value("instructionReference", "0");
        int bpOffset    = bp.value("offset", 0);
        uint16_t addr   = static_cast<uint16_t>(
            std::stoul(ref, nullptr, 0) + static_cast<unsigned>(bpOffset));
        addresses.push_back(static_cast<uint32_t>(addr));
    }

    int count = static_cast<int>(addresses.size());
    int armed = 0;

    if (g_runControl.IsSessionActive()) {
        armed = g_bpManager.SetInstructionBreakpoints(
            addresses.empty() ? nullptr : addresses.data(), count, amCODE);
    }

    // Verify which BPs are actually in the DLL linked list.
    auto isArmed = [&](uint32_t addr) -> bool {
        for (AG_BP* p = g_bpManager.HeadPtr() ? *g_bpManager.HeadPtr() : nullptr;
             p; p = p->next) {
            if (p->Adr == addr) return true;
        }
        return false;
    };

    for (int i = 0; i < count; ++i) {
        bool ok = g_runControl.IsSessionActive() && isArmed(addresses[i]);
        nlohmann::json entry = {{"verified", ok}, {"id", i + 1}};
        if (!ok && g_runControl.IsSessionActive()) {
            entry["message"] = "Hardware limit: max " +
                               std::to_string(kMaxUserBreakpoints) + " breakpoints total";
        }
        bpResults.push_back(std::move(entry));
    }

    LOG("[DAP]  setInstructionBreakpoints: %d requested, %d armed\n", count, armed);
    SendResponse(seq, "setInstructionBreakpoints", {{"breakpoints", bpResults}});
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
