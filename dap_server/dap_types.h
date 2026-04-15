// dap_server/dap_types.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Minimal DAP protocol types and constants for the silicon labs 8051 DAP server.
// The full DAP specification is at:
//   https://microsoft.github.io/debug-adapter-protocol/specification
//
// This header provides only what the server needs to construct valid responses.
// JSON serialisation uses nlohmann/json directly; no heavy type mapping here.

#pragma once

#include <string>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// DAP message type strings
// ---------------------------------------------------------------------------

constexpr const char* kDapTypeRequest  = "request";
constexpr const char* kDapTypeResponse = "response";
constexpr const char* kDapTypeEvent    = "event";

// ---------------------------------------------------------------------------
// DAP stop reasons (used in 'stopped' event body)
// ---------------------------------------------------------------------------

constexpr const char* kStopReasonStep       = "step";
constexpr const char* kStopReasonBreakpoint = "breakpoint";
constexpr const char* kStopReasonPause      = "pause";
constexpr const char* kStopReasonEntry      = "entry";
constexpr const char* kStopReasonException  = "exception";

// ---------------------------------------------------------------------------
// Well-known thread ID
// The C8051F380 is a single-core MCU with no RTOS model.
// We always report one thread with this ID.
// ---------------------------------------------------------------------------

constexpr int kThreadId = 1;

// ---------------------------------------------------------------------------
// Fixed DAP port
// ---------------------------------------------------------------------------

constexpr int kDapPort = 4711;

// ---------------------------------------------------------------------------
// Capabilities advertised in the 'initialize' response body.
// Only features the server actually implements are listed as true.
// ---------------------------------------------------------------------------

inline nlohmann::json MakeCapabilities()
{
    return {
        {"supportsConfigurationDoneRequest", true},
        {"supportsSetBreakpointsRequest",    true},
        {"supportsContinueRequest",          true},
        {"supportsNextRequest",              true},
        {"supportsStepInRequest",            true},
        {"supportsPauseRequest",             true},
        {"supportsDisconnectRequest",        true},
        {"supportsReadMemoryRequest",        true},
        {"supportsTerminateRequest",         false},
        {"supportsRestartRequest",           false},
        {"supportsGotoTargetsRequest",       false},
        {"supportsCompletionsRequest",       false},
        {"supportsModulesRequest",           false},
    };
}

// ---------------------------------------------------------------------------
// Helper: build a DAP source reference from a file path string.
// Returns a JSON object suitable for use as a 'source' field.
// ---------------------------------------------------------------------------

inline nlohmann::json MakeSource(const std::string& path)
{
    return {{"name", path}, {"path", path}};
}
