$proc = Get-Process -Name dap_server -ErrorAction SilentlyContinue
if ($proc) {
    $proc | Stop-Process
    Write-Host "DAP server stopped (PID $($proc.Id))"
} else {
    Write-Host "DAP server is not running"
}
