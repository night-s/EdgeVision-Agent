param([string]$BoardHost = "192.168.94.126", [int]$Port = 9000, [int]$RtspPort = 8554)
$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '..\.venv-pc\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    throw 'Create the PC virtual environment first; see docs/pc-viewer.md.'
}
& $python (Join-Path $PSScriptRoot 'pc_viewer.py') --host $BoardHost --port $Port --rtsp-port $RtspPort
