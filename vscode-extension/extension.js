// vscode-extension/extension.js
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Registers a DebugAdapterDescriptorFactory that connects VSCode to the
// C8051 DAP server running on localhost:4711.
// Also provides custom debug views: Registers, DATA, XDATA, IDATA, CODE.
// DebugConfigurationProvider synthesises a launch config when none exists,
// including auto-detecting the .uvproj and deriving the HEX output path,
// and optionally invoking UV4.exe to build before launching.
'use strict';
const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const cp     = require('child_process');
const os     = require('os');

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
// Keil UV4 helpers
// ---------------------------------------------------------------------------

// Locate UV4.exe: checks the VS Code setting, then the Windows registry
// (HKLM\SOFTWARE\WOW6432Node\Keil\Products\C51 → Path), then default paths.
// Returns the full path to UV4.exe, or null if not found.
function findKeilUv4() {
    // 1. VS Code setting (user-supplied override).
    const cfg = vscode.workspace.getConfiguration('silabs8051');
    const settingPath = cfg.get('keilPath', '');
    if (settingPath) {
        // User may supply either the Keil root or the full UV4.exe path.
        if (settingPath.toLowerCase().endsWith('uv4.exe') && fs.existsSync(settingPath))
            return settingPath;
        const candidate = path.join(settingPath, 'UV4', 'UV4.exe');
        if (fs.existsSync(candidate)) return candidate;
    }

    // 2. Windows registry — Keil C51 installs the path under:
    //    HKLM\SOFTWARE\WOW6432Node\Keil\Products\C51  →  Path  (e.g. C:\Keil_v5\C51\)
    //    UV4.exe lives one level up from the C51 subfolder.
    try {
        const result = cp.execSync(
            'reg query "HKLM\\SOFTWARE\\WOW6432Node\\Keil\\Products\\C51" /v Path',
            { encoding: 'utf8', timeout: 3000, stdio: ['ignore', 'pipe', 'ignore'] }
        );
        const match = result.match(/Path\s+REG_SZ\s+(.+)/);
        if (match) {
            const c51Dir  = match[1].trim();                 // e.g. C:\Keil_v5\C51\
            const keilRoot = path.dirname(c51Dir.replace(/[/\\]+$/, ''));  // C:\Keil_v5
            const uv4 = path.join(keilRoot, 'UV4', 'UV4.exe');
            if (fs.existsSync(uv4)) return uv4;
        }
    } catch { /* registry unavailable or key absent */ }

    // 3. Well-known default install locations (system-wide and per-user).
    const localAppData = process.env.LOCALAPPDATA || '';
    for (const candidate of [
        'C:\\Keil_v5\\UV4\\UV4.exe',
        'C:\\Keil\\UV4\\UV4.exe',
        'C:\\Keil_v4\\UV4\\UV4.exe',
        path.join(localAppData, 'Keil_v5', 'UV4', 'UV4.exe'),
        path.join(localAppData, 'Keil', 'UV4', 'UV4.exe'),
    ]) {
        if (fs.existsSync(candidate)) return candidate;
    }

    return null;
}

// Search for a .uvproj (or .uvprojx) in the workspace root and one level
// of subdirectories.  Returns the first match or null.
function findUvproj(workspaceRoot) {
    const isUvproj = f => f.endsWith('.uvproj') || f.endsWith('.uvprojx');
    try {
        const rootFiles = fs.readdirSync(workspaceRoot).filter(isUvproj);
        if (rootFiles.length > 0) return path.join(workspaceRoot, rootFiles[0]);

        const dirs = fs.readdirSync(workspaceRoot).filter(f => {
            if (f.startsWith('.')) return false;
            try { return fs.statSync(path.join(workspaceRoot, f)).isDirectory(); }
            catch { return false; }
        });
        for (const dir of dirs) {
            const subFiles = fs.readdirSync(path.join(workspaceRoot, dir)).filter(isUvproj);
            if (subFiles.length > 0) return path.join(workspaceRoot, dir, subFiles[0]);
        }
    } catch { /* permission error or empty workspace */ }
    return null;
}

// Parse a .uvproj XML file and return the expected HEX output path.
// Returns null if the project has HEX output disabled or fields are missing.
function parseUvprojHexPath(uvprojPath) {
    let xml;
    try { xml = fs.readFileSync(uvprojPath, 'utf8'); }
    catch { return null; }

    // Extract the first target's output settings.
    // Fields are inside <TargetOption><TargetCommonOption>.
    const outDir    = (xml.match(/<OutputDirectory>\s*(.*?)\s*<\/OutputDirectory>/)  || [])[1];
    const outName   = (xml.match(/<OutputName>\s*(.*?)\s*<\/OutputName>/)            || [])[1];
    const createHex = (xml.match(/<CreateHex(?:File)?>\s*(\d+)\s*<\/CreateHex(?:File)?>/) || [])[1];

    if (!outDir || !outName) return null;
    if (createHex === '0') return null;  // HEX generation disabled in project settings

    const uvprojDir  = path.dirname(uvprojPath);
    const resolvedDir = path.resolve(uvprojDir, outDir.replace(/\//g, path.sep));
    return path.join(resolvedDir, outName + '.hex');
}

// Invoke UV4.exe -b <uvprojPath> -o <logfile> and stream the log to the
// provided output channel.  Resolves true on success (exit 0 or 1 = warnings),
// false on error (exit 2+) or launch failure.
// Run a single UV4 invocation (-b or -f) and append output to the channel.
// target (optional) is passed as -t <name> to select a specific µVision target.
// Returns true on success (exit 0 or 1), false on failure.
function runUv4(uv4Path, flag, uvprojPath, logFile, outputChannel, target) {
    const args = [flag, uvprojPath, '-o', logFile, '-jO'];
    if (target) args.push('-t', target);
    outputChannel.appendLine(`> "${uv4Path}" ${args.join(' ')}`);

    return new Promise(resolve => {
        let proc;
        try {
            proc = cp.spawn(uv4Path, args, { windowsHide: true });
        } catch (err) {
            outputChannel.appendLine(`Failed to launch UV4.exe: ${err.message}`);
            resolve(false);
            return;
        }

        let stdoutBuf = '';
        let stderrBuf = '';
        proc.stdout && proc.stdout.on('data', d => { stdoutBuf += d.toString(); });
        proc.stderr && proc.stderr.on('data', d => { stderrBuf += d.toString(); });

        proc.on('error', err => {
            outputChannel.appendLine(`Failed to launch UV4.exe: ${err.message}`);
            resolve(false);
        });

        proc.on('close', code => {
            let logContent = null;
            try { logContent = fs.readFileSync(logFile, 'utf8'); } catch { /* not written */ }

            if (logContent) {
                outputChannel.append(logContent);
            } else if (stderrBuf) {
                outputChannel.appendLine(stderrBuf);
            } else {
                outputChannel.appendLine('(no log written)');
            }
            if (stdoutBuf) outputChannel.appendLine(stdoutBuf);

            // Exit codes: 0 = success, 1 = warnings (ok), 2 = errors, 3 = fatal
            if (code === 0 || code === 1) {
                outputChannel.appendLine(
                    `Done (exit ${code}${code === 1 ? ' — warnings' : ''}).`);
                resolve(true);
            } else {
                outputChannel.appendLine(`FAILED (exit ${code}).`);
                resolve(false);
            }
            outputChannel.appendLine('');
        });
    });
}

// Build (and optionally flash) a uvproj, streaming all output to the channel.
// target (optional) selects a specific µVision target via UV4 -t flag.
async function buildWithUv4(uv4Path, uvprojPath, outputChannel, flash = false, target = null) {
    const buildLog = path.join(os.tmpdir(), 'silabs8051_build.txt');
    const flashLog = path.join(os.tmpdir(), 'silabs8051_flash.txt');

    outputChannel.clear();
    outputChannel.show(true);
    outputChannel.appendLine(flash ? '-- Build --' : '-- Build --');
    outputChannel.appendLine('');

    const buildOk = await runUv4(uv4Path, '-b', uvprojPath, buildLog, outputChannel, target);
    if (!buildOk) return false;

    if (flash) {
        outputChannel.appendLine('-- Flash --');
        outputChannel.appendLine('');
        const flashOk = await runUv4(uv4Path, '-f', uvprojPath, flashLog, outputChannel, target);
        if (!flashOk) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Server helpers
// ---------------------------------------------------------------------------

// Test whether TCP port is accepting connections.
function isPortOpen(port) {
    const net = require('net');
    return new Promise(resolve => {
        const sock = new net.Socket();
        sock.setTimeout(200);
        sock.on('connect', () => { sock.destroy(); resolve(true); });
        sock.on('error',   () => { sock.destroy(); resolve(false); });
        sock.on('timeout', () => { sock.destroy(); resolve(false); });
        sock.connect(port, '127.0.0.1');
    });
}

// Poll port until open or timeout. Resolves true if port opens in time.
async function waitForPort(port, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (await isPortOpen(port)) return true;
        await new Promise(r => setTimeout(r, 100));
    }
    return false;
}

// Ensure dap_server.exe is running. Uses bundled exe from bin\ if present;
// falls back to ensure_server.ps1 in the scripts folder (development mode).
async function ensureServer(extensionPath) {
    const exePath = path.join(extensionPath, 'bin', 'dap_server.exe');
    if (!fs.existsSync(exePath)) {
        // Dev mode: use ensure_server.ps1 from the sibling scripts folder.
        const scriptPath = path.join(extensionPath, '..', 'scripts', 'ensure_server.ps1');
        if (fs.existsSync(scriptPath)) {
            cp.spawn('powershell.exe',
                ['-NoProfile', '-NonInteractive', '-File', scriptPath],
                { windowsHide: true, detached: false });
        }
        return;
    }
    if (await isPortOpen(4711)) return;  // already running
    const logFile = path.join(os.tmpdir(), 'dap_server.log');
    const logFd = fs.openSync(logFile, 'a');
    cp.spawn(exePath, [], {
        windowsHide: true,
        detached: true,
        stdio: ['ignore', logFd, logFd]
    }).unref();
    await waitForPort(4711, 10000);
}

// Copy SiC8051F.dll and USBHID.dll from the user's Keil installation into
// the extension's bin\ directory if they are not already present.
// Returns true if both DLLs are present after the call.
async function ensureDlls(extensionPath) {
    const binDir = path.join(extensionPath, 'bin');
    const dll1   = path.join(binDir, 'SiC8051F.dll');
    const dll2   = path.join(binDir, 'USBHID.dll');
    if (fs.existsSync(dll1) && fs.existsSync(dll2)) return true;

    const uv4Path = findKeilUv4();
    if (!uv4Path) {
        vscode.window.showErrorMessage(
            'SiLabs 8051: Keil installation not found. ' +
            'Set "silabs8051.keilPath" in settings so the adapter DLLs can be copied.',
            'Open Settings'
        ).then(choice => {
            if (choice === 'Open Settings')
                vscode.commands.executeCommand(
                    'workbench.action.openSettings', 'silabs8051.keilPath');
        });
        return false;
    }

    // uv4Path = <keilRoot>\UV4\UV4.exe  →  keilRoot = two levels up
    const keilRoot = path.dirname(path.dirname(uv4Path));
    const src1 = path.join(keilRoot, 'C51', 'Bin', 'SiC8051F.dll');
    const src2 = path.join(keilRoot, 'UV4', 'USBHID.dll');
    try {
        if (!fs.existsSync(binDir)) fs.mkdirSync(binDir, { recursive: true });
        if (!fs.existsSync(dll1)) fs.copyFileSync(src1, dll1);
        if (!fs.existsSync(dll2)) fs.copyFileSync(src2, dll2);
        return true;
    } catch (err) {
        vscode.window.showErrorMessage(
            `SiLabs 8051: Failed to copy adapter DLLs from Keil: ${err.message}`);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------
exports.activate = function (context) {
    console.log('[SiLabs 8051 Debug] Extension activated!');

    // Eagerly copy Keil DLLs into bin\ if not already present.
    ensureDlls(context.extensionPath);

    // Command: open a folder picker and save the chosen path to the setting.
    context.subscriptions.push(
        vscode.commands.registerCommand('silabs8051.browseKeilPath', async () => {
            const uris = await vscode.window.showOpenDialog({
                canSelectFiles: false,
                canSelectFolders: true,
                canSelectMany: false,
                title: 'Select Keil Installation Folder (e.g. C:\\Keil_v5)',
            });
            if (uris && uris.length > 0) {
                await vscode.workspace.getConfiguration('silabs8051')
                    .update('keilPath', uris[0].fsPath, vscode.ConfigurationTarget.Global);
                vscode.window.showInformationMessage(
                    `SiLabs 8051: Keil path set to "${uris[0].fsPath}"`);
            }
        })
    );

    // Persistent output channel for build output.
    const buildChannel = vscode.window.createOutputChannel('SiLabs 8051 Build');
    context.subscriptions.push(buildChannel);

    // -----------------------------------------------------------------------
    // DebugConfigurationProvider — called when the user presses F5.
    //
    // Resolution order for 'program':
    //   1. Already set in launch.json → use as-is.
    //   2. 'uvprojFile' in launch.json → parse it for the HEX path.
    //   3. Auto-detect: search workspace for a .uvproj → parse it.
    //   4. Search common output dirs for any .hex file.
    //   5. Show a file picker.
    //
    // If 'buildBeforeDebug' is true (and a .uvproj was found), UV4.exe is
    // invoked first; the launch is aborted if the build fails.
    // -----------------------------------------------------------------------
    const configProvider = {
        async resolveDebugConfiguration(_folder, config, _token) {
            // Empty config means no launch.json or no selection — synthesise defaults.
            if (!config.type && !config.request && !config.name) {
                config.type    = 'silabs8051';
                config.request = 'launch';
                config.name    = 'Debug (SiLabs 8051)';
            }

            // ---- Start DAP server early so it's ready by the time we connect ----
            // Fire-and-forget — runs concurrently with the build.
            ensureServer(context.extensionPath);

            const ws = vscode.workspace.workspaceFolders;
            // _folder is the workspace folder that owns the launch.json — use it as
            // the default for bare ${workspaceFolder}. Fall back to ws[0] only when
            // the config comes from a workspace-level launch section with no owner.
            const wsRoot = (_folder && _folder.uri.fsPath)
                || (ws && ws.length > 0 ? ws[0].uri.fsPath : null);

            // ---- Resolve .uvproj ----
            let uvprojPath = null;
            if (config.uvprojFile) {
                // Resolve ${workspaceFolder} and ${workspaceFolder:Name} variables,
                // which VS Code does not expand automatically for custom fields.
                let uvprojRaw = config.uvprojFile;
                if (ws) {
                    uvprojRaw = uvprojRaw.replace(
                        /\$\{workspaceFolder(?::([^}]*))?\}/g,
                        (_, name) => {
                            if (name) {
                                const found = ws.find(f => f.name === name);
                                return found ? found.uri.fsPath : _;
                            }
                            return wsRoot || _;
                        }
                    );
                }
                // Make absolute relative to workspace root if still relative.
                uvprojPath = path.isAbsolute(uvprojRaw)
                    ? uvprojRaw
                    : wsRoot ? path.join(wsRoot, uvprojRaw) : uvprojRaw;
                if (!fs.existsSync(uvprojPath)) {
                    vscode.window.showErrorMessage(
                        `SiLabs 8051: uvprojFile not found: ${uvprojPath}`);
                    return undefined;
                }
            } else if (!config.program && wsRoot) {
                uvprojPath = findUvproj(wsRoot);
                if (uvprojPath)
                    console.log(`[SiLabs 8051] Auto-detected uvproj: ${uvprojPath}`);
            }

            // ---- Optionally build (and flash) before launch ----
            if (config.buildBeforeDebug && uvprojPath) {
                const uv4 = findKeilUv4();
                if (!uv4) {
                    vscode.window.showErrorMessage(
                        `SiLabs 8051: UV4.exe not found. ` +
                        `Set the 'silabs8051.keilPath' setting to your Keil installation folder.`);
                    return undefined;
                }

                if (config.noDebug) {
                    // -f: build + flash in one step via UV4's built-in flash downloader.
                    // No DAP server needed — return undefined to cancel the debug session
                    // after UV4 finishes.
                    const ok = await buildWithUv4(uv4, uvprojPath, buildChannel, true, config.buildTarget || null);
                    if (!ok) {
                        vscode.window.showErrorMessage(
                            `SiLabs 8051: Flash failed — check the "SiLabs 8051 Build" output panel.`);
                    }
                    return undefined;  // cancel debug session; flash was handled by UV4
                }

                const ok = await buildWithUv4(uv4, uvprojPath, buildChannel, false, config.buildTarget || null);
                if (!ok) {
                    vscode.window.showErrorMessage(
                        `SiLabs 8051: Build failed — check the "SiLabs 8051 Build" output panel.`);
                    return undefined;
                }
            }

            // ---- Resolve program (HEX path) ----
            if (!config.program) {
                // Try parsing the uvproj for the configured output path.
                if (uvprojPath) {
                    const hexFromProj = parseUvprojHexPath(uvprojPath);
                    if (hexFromProj) {
                        config.program = hexFromProj;
                        console.log(`[SiLabs 8051] HEX from uvproj: ${hexFromProj}`);
                    }
                }

                // Fallback: scan common output directories for any .hex.
                if (!config.program && wsRoot) {
                    for (const dir of [
                        path.join(wsRoot, 'output'),
                        path.join(wsRoot, 'Objects'),
                        path.join(wsRoot, 'build'),
                        wsRoot,
                    ]) {
                        try {
                            const files = fs.readdirSync(dir).filter(f => f.endsWith('.hex'));
                            if (files.length > 0) {
                                config.program = path.join(dir, files[0]);
                                break;
                            }
                        } catch { /* dir doesn't exist */ }
                    }
                }

                // Last resort: file picker.
                if (!config.program) {
                    const uris = await vscode.window.showOpenDialog({
                        canSelectFiles: true,
                        canSelectFolders: false,
                        canSelectMany: false,
                        filters: { 'Intel HEX': ['hex'] },
                        title: 'Select firmware HEX file',
                    });
                    if (uris && uris.length > 0) {
                        config.program = uris[0].fsPath;
                    } else {
                        return undefined;  // user cancelled
                    }
                }
            }

            return config;
        }
    };
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('silabs8051', configProvider)
    );

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
