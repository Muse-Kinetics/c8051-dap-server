// dap_server/log.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Minimal triple-write logger.
//
// LOG()   — important events; written to stdout AND to the VS Code Debug
//           Console via DAP output events (when a client is connected).
// LOGV()  — verbose detail; written to stderr (log file) only.
//
// Both flush immediately so the console window stays live.

#pragma once
#include <cstdio>
#include <cstdarg>

// Sends formatted text as a DAP "output" event to VS Code's Debug Console.
// No-op when no DAP client is connected.  Defined in dap_server.cpp.
void DapLogSend(const char* fmt, ...);

#define LOG(fmt, ...) \
    do { \
        fprintf(stdout, fmt, ##__VA_ARGS__); \
        fflush(stdout); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        DapLogSend(fmt, ##__VA_ARGS__); \
    } while (0)

#define LOGV(fmt, ...) \
    do { \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
    } while (0)
