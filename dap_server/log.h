// dap_server/log.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Minimal dual-write logger.
//
// LOG()   — important events; written to stdout (console window) only.
// LOGV()  — verbose detail; written to stderr (log file) only.
//
// Both flush immediately so the console window stays live.

#pragma once
#include <cstdio>

#define LOG(fmt, ...) \
    do { \
        fprintf(stdout, fmt, ##__VA_ARGS__); \
        fflush(stdout); \
    } while (0)

#define LOGV(fmt, ...) \
    do { \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
    } while (0)
