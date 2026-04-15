// dap_server/dap_server.cpp
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
#include "opcodes8051.h"
#include "log.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// Global DapServer pointer — set during RunSession, used by run-control
// helpers to send asynchronous stopped events from background threads.
DapServer* g_pServer = nullptr;

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

    nlohmann::json msg;
    while (!m_disconnecting && RecvMessage(msg)) {
        Dispatch(msg);
    }

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
    else if (command == "setBreakpoints")    HandleSetBreakpoints   (seq, args);
    else if (command == "threads")           HandleThreads          (seq, args);
    else if (command == "continue")          HandleContinue         (seq, args);
    else if (command == "next")              HandleNext             (seq, args);
    else if (command == "stepIn")            HandleStepIn           (seq, args);
    else if (command == "pause")             HandlePause            (seq, args);
    else if (command == "stackTrace")        HandleStackTrace       (seq, args);
    else if (command == "scopes")            HandleScopes           (seq, args);
    else if (command == "variables")         HandleVariables        (seq, args);
    else if (command == "readMemory")        HandleReadMemory       (seq, args);
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
    // Breakpoints have been delivered; no action needed until Phase 6.
    SendResponse(seq, "configurationDone");
}

// ---------------------------------------------------------------------------
// Phase 1 — disconnect
// ---------------------------------------------------------------------------

void DapServer::HandleDisconnect(int seq, const nlohmann::json& /*args*/)
{
    // Clean up the AGDI session before disconnecting.
    if (g_runControl.IsSessionActive()) {
        g_runControl.UninitAgdiSession();
    }
    g_hexLoader.Unload();

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

        uint32_t pc = g_registers.PC();
        LOG("[DEBUG] Target halted at PC=0x%04X\n", pc);

        SendEvent("stopped", {
            {"reason",            "entry"},
            {"threadId",          kThreadId},
            {"allThreadsStopped", true},
        });
    } else {
        g_hexLoader.Unload();
        LOG("[LAUNCH] Flash complete — session terminated\n");
        SendEvent("terminated", nlohmann::json(nullptr));
    }
}

void DapServer::HandleSetBreakpoints(int seq, const nlohmann::json& args)
{
    // Extract breakpoint addresses from the request.
    // DAP sends: { source: { path: ... }, breakpoints: [ { line: N }, ... ] }
    // For a bare-metal 8051 debug adapter with no source mapping, we accept
    // an extension field 'address' (hex string) on each breakpoint.
    // If only 'line' is provided, we treat it as a CODE address (for now).
    std::vector<uint32_t> addresses;
    nlohmann::json verified = nlohmann::json::array();

    if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
        for (auto& bp : args["breakpoints"]) {
            uint32_t addr = 0;
            if (bp.contains("address") && bp["address"].is_string()) {
                addr = static_cast<uint32_t>(std::stoul(bp["address"].get<std::string>(), nullptr, 0));
            } else if (bp.contains("line") && bp["line"].is_number_integer()) {
                addr = static_cast<uint32_t>(bp["line"].get<int>());
            }
            addresses.push_back(addr);
        }
    }

    int count = 0;
    if (!addresses.empty() && g_runControl.IsSessionActive()) {
        count = g_bpManager.SetBreakpoints(addresses.data(),
                                           static_cast<int>(addresses.size()),
                                           amCODE);
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

static void WaitAndSendStopped(const std::string& reason)
{
    if (!g_runControl.WaitForHalt(30000)) {
        LOG("[DEBUG] Timeout waiting for halt (%s)\n", reason.c_str());
    }
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

    // Issue AG_GOFORBRK: run until a breakpoint fires.
    if (g_runControl.GoStep(AG_GOFORBRK, 0, nullptr)) {
        std::thread(WaitAndSendStopped, kStopReasonBreakpoint).detach();
    }
}

void DapServer::HandleNext(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "next");

    // Step-over: compute the address of the next sequential instruction.
    // Read the opcode byte at PC to determine instruction length.
    uint32_t pc = g_registers.PC();
    UC8 opcByte = 0;
    GADR addr{};
    addr.Adr    = (static_cast<UL32>(amCODE) << 24) | (pc & 0xFFFF);
    addr.nLen   = 1;
    addr.mSpace = amCODE;
    if (g_agdi.AG_MemAcc) {
        g_agdi.AG_MemAcc(AG_READ, &opcByte, &addr, 1);
    }
    uint32_t nextPC = pc + k8051InstructionLength[opcByte];

    GADR target{};
    target.Adr    = nextPC & 0xFFFF;
    target.mSpace = amCODE;

    if (g_runControl.GoStep(AG_GOTILADR, 0, &target)) {
        std::thread(WaitAndSendStopped, kStopReasonStep).detach();
    }
}

void DapServer::HandleStepIn(int seq, const nlohmann::json& /*args*/)
{
    SendResponse(seq, "stepIn");

    // Single-step: execute one instruction.
    if (g_runControl.GoStep(AG_NSTEP, 1, nullptr)) {
        std::thread(WaitAndSendStopped, kStopReasonStep).detach();
    }
}

void DapServer::HandlePause(int seq, const nlohmann::json& /*args*/)
{
    // Force-stop the running target.
    g_runControl.GoStep(AG_STOPRUN, 0, nullptr);
    SendResponse(seq, "pause");
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
    // Scope variablesReference IDs:
    //   1 = Registers
    //   2 = CODE memory
    //   3 = XDATA memory
    //   4 = DATA memory
    //   5 = IDATA memory
    nlohmann::json scopes = {
        {{"name", "Registers"}, {"variablesReference", 1}, {"expensive", false}},
        {{"name", "CODE"},      {"variablesReference", 2}, {"expensive", true}},
        {{"name", "XDATA"},     {"variablesReference", 3}, {"expensive", true}},
        {{"name", "DATA"},      {"variablesReference", 4}, {"expensive", true}},
        {{"name", "IDATA"},     {"variablesReference", 5}, {"expensive", true}},
    };
    SendResponse(seq, "scopes", {{"scopes", scopes}});
}

void DapServer::HandleVariables(int seq, const nlohmann::json& args)
{
    int ref = args.value("variablesReference", 0);

    if (ref == 1) {
        // Registers scope — format from the cached RG51.
        SendResponse(seq, "variables", {{"variables", g_registers.ToVariables()}});
        return;
    }

    // Memory scopes (2–5): read a page of memory via AG_MemAcc.
    // Map scope ref to memory space.
    struct { int ref; U16 mSpace; const char* name; UL32 size; } memScopes[] = {
        {2, amCODE,  "CODE",  256},
        {3, amXDATA, "XDATA", 256},
        {4, amDATA,  "DATA",  256},
        {5, amIDATA, "IDATA", 256},
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
