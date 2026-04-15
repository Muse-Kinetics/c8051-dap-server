$root = "$PSScriptRoot\.."
$exe = "$root\build\dap_server\bin\Debug\dap_server.exe"
$log = "$root\dap_server.log"

if (-not (Test-Path $exe)) {
    Write-Error "dap_server.exe not found at $exe — run cmake --build first."
    exit 1
}

Write-Host "Starting DAP server in new window (verbose log -> $log)"
Start-Process $exe -RedirectStandardError $log
