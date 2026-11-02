$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Require-Command($name, $hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "$name is required. $hint"
    }
}

Require-Command node 'Install Node.js 20+ and reopen PowerShell.'
Require-Command npm 'Install Node.js 20+ and reopen PowerShell.'
Require-Command python 'Install Python 3.11+ and reopen PowerShell.'
Require-Command cmake 'Install CMake 3.20+ and reopen PowerShell.'

if (-not (Get-Command psql -ErrorAction SilentlyContinue)) {
    Write-Warning 'psql was not found. Complete PostgreSQL setup manually or set POSTGRES_BIN.'
}
if (-not (Get-Command redis-server -ErrorAction SilentlyContinue) -and -not $env:REDIS_SERVER_PATH) {
    Write-Warning 'redis-server was not found. Install native Redis or set REDIS_SERVER_PATH.'
}

if (-not (Test-Path "$root\services\api-gateway\.env")) {
    Copy-Item "$root\services\api-gateway\.env.example" "$root\services\api-gateway\.env"
}
if (-not (Test-Path "$root\services\ai-service\.env")) {
    Copy-Item "$root\services\ai-service\.env.example" "$root\services\ai-service\.env"
}
if (-not (Test-Path "$root\.env")) {
@'
POSTGRES_HOST=127.0.0.1
POSTGRES_PORT=5432
POSTGRES_DB=quantara
POSTGRES_USER=quantara
POSTGRES_PASSWORD=replace-with-a-local-password
DATABASE_URL=
REDIS_URL=redis://127.0.0.1:6379/0
POSTGRES_BIN=
PGDATA=
REDIS_SERVER_PATH=
'@ | Set-Content "$root\.env"
}

Push-Location "$root\services\api-gateway"
npm install
Pop-Location
Push-Location "$root\frontend"
npm install
Pop-Location
if (-not (Test-Path "$root\services\ai-service\.venv\Scripts\python.exe")) {
    python -m venv "$root\services\ai-service\.venv"
}
& "$root\services\ai-service\.venv\Scripts\python.exe" -m pip install -r "$root\services\ai-service\requirements.txt"
& "$root\services\ai-service\.venv\Scripts\python.exe" -m pip install -r "$root\services\worker\requirements.txt"
$build = Join-Path $root '.quantara-build'
cmake -S $root -B $build -DCMAKE_BUILD_TYPE=Release
cmake --build $build --parallel

Write-Host 'Setup complete. Review environment files before starting.'
