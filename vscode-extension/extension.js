// vscode-extension/extension.js
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Registers a DebugAdapterDescriptorFactory that connects VSCode to the
// C8051 DAP server running on localhost:4711.
// Also provides custom debug views: Registers, DATA, XDATA, IDATA, CODE.
'use strict';
const vscode = require('vscode');

// ---------------------------------------------------------------------------
// TreeDataProvider that fetches DAP variables for a given variablesReference.
// ---------------------------------------------------------------------------
class DapVariablesProvider {
    constructor(variablesReference) {
        this._ref = variablesReference;
        this._items = [];
        this._onDidChange = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChange.event;
    }

    refresh() { this._onDidChange.fire(); }

    getTreeItem(element) { return element; }

    async getChildren() {
        const session = vscode.debug.activeDebugSession;
        if (!session) return [];
        try {
            const resp = await session.customRequest('variables', {
                variablesReference: this._ref
            });
            const vars = resp.variables || [];
            return vars.map(v => {
                const item = new vscode.TreeItem(
                    `${v.name}: ${v.value}`,
                    vscode.TreeItemCollapsibleState.None
                );
                item.tooltip = `${v.name} = ${v.value}`;
                return item;
            });
        } catch {
            return [];
        }
    }
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------
exports.activate = function (context) {
    console.log('[SiLabs 8051 Debug] Extension activated!');
    // DAP server connection factory.
    const factory = {
        createDebugAdapterDescriptor(_session, _executable) {
            return new vscode.DebugAdapterServer(4711);
        }
    };
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('silabs8051', factory)
    );

    // Custom debug views — one per scope.
    const views = [
        { id: 'silabs8051.registers', ref: 100 },
        { id: 'silabs8051.data',      ref: 103 },
        { id: 'silabs8051.xdata',     ref: 102 },
        { id: 'silabs8051.idata',     ref: 104 },
        { id: 'silabs8051.code',      ref: 101 },
    ];

    const providers = views.map(v => {
        const provider = new DapVariablesProvider(v.ref);
        context.subscriptions.push(
            vscode.window.createTreeView(v.id, { treeDataProvider: provider })
        );
        return provider;
    });

    // Refresh all views when the debug session stops (breakpoint, step, etc.).
    context.subscriptions.push(
        vscode.debug.onDidReceiveDebugSessionCustomEvent(e => {
            if (e.event === 'stopped') {
                providers.forEach(p => p.refresh());
            }
        })
    );

    // Also refresh on the standard stopped event via onDidChangeActiveDebugSession
    // and by listening to debug adapter tracker messages.
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterTrackerFactory('silabs8051', {
            createDebugAdapterTracker(_session) {
                return {
                    onDidSendMessage(msg) {
                        if (msg.type === 'event' && msg.event === 'stopped') {
                            providers.forEach(p => p.refresh());
                        }
                    }
                };
            }
        })
    );
};

exports.deactivate = function () {};
