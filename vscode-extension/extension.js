// vscode-extension/extension.js
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Registers a DebugAdapterDescriptorFactory that connects VSCode to the
// C8051 DAP server running on localhost:4711.
'use strict';
const vscode = require('vscode');

exports.activate = function (context) {
    const factory = {
        createDebugAdapterDescriptor(_session, _executable) {
            return new vscode.DebugAdapterServer(4711);
        }
    };
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('silabs8051', factory)
    );
};

exports.deactivate = function () {};
