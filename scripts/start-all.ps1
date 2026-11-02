$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$state = Join-Path $root '.trademind'
$build = Join-Path $root '.trademind-build'
New-Item -ItemType Directory -Force $state | Out-Null

function Import-DotEnv($path) {
    if (-not (Test-Path $path)) { return }
    Get-Content $path | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith('#') -and $line.Contains('=')) {
            $parts = $line.Split('=', 2)
            [Environment]::SetEnvironmentVariable($parts[0].Trim(), $parts[1].Trim().Trim('"'))
        }
    }
}
Import-DotEnv "$root\.env"
Import-DotEnv "$root\services\api-gateway\.env"
Import-DotEnv "$root\services\ai-service\.env"

function Start-Native($name, $file, $workingDirectory, $arguments) {
    $log = Join-Path $state "$name.log"
    $process = Start-Process -FilePath $file -ArgumentList $arguments -WorkingDirectory $workingDirectory `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
    $process.Id | Set-Content (Join-Path $state "$name.pid")
    Write-Host "Started $name (PID $($process.Id))"
}

function Wait-ForUrl($name, $url, $timeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    do {
        try {
            Invoke-WebRequest -Uri $url -TimeoutSec 2 -UseBasicParsing | Out-Null
            Write-Host "$name is healthy"
            return
        } catch { Start-Sleep -Seconds 1 }
    } while ((Get-Date) -lt $deadline)
    throw "$name did not become healthy at $url"
}

if (-not (Test-Path "$build\trademind_engine_adapter.exe")) {
    cmake -S $root -B $build -DCMAKE_BUILD_TYPE=Release
    cmake --build $build --parallel
}

$env:ENGINE_ADAPTER_PATH = "$build\trademind_engine_adapter.exe"
$env:BENCHMARK_EXECUTABLE = "$build\lob_bench.exe"
if (-not (Test-Path $env:ENGINE_ADAPTER_PATH)) {
    throw "C++ adapter was not built at $env:ENGINE_ADAPTER_PATH"
}
$env:PYTHONPATH = "$root\services\ai-service"

if ($env:POSTGRES_BIN -and (Test-Path "$env:POSTGRES_BIN\pg_ctl.exe")) {
    Start-Native 'postgres' "$env:POSTGRES_BIN\pg_ctl.exe" $root "-D `"$env:PGDATA`" -l `"$state\postgres.log`" start"
}
if ($env:REDIS_SERVER_PATH -and (Test-Path $env:REDIS_SERVER_PATH)) {
    Start-Native 'redis' $env:REDIS_SERVER_PATH $root ''
}

Start-Native 'ai-service' "$root\services\ai-service\.venv\Scripts\python.exe" "$root\services\ai-service" '-m uvicorn app.main:app --host 127.0.0.1 --port 8000'
Wait-ForUrl 'AI service' 'http://127.0.0.1:8000/health'
Start-Native 'api-gateway' 'node.exe' "$root\services\api-gateway" 'src/server.js'
Wait-ForUrl 'API gateway' 'http://127.0.0.1:3000/api/health'
Start-Native 'worker' "$root\services\ai-service\.venv\Scripts\python.exe" "$root\services\worker" 'worker.py'
Start-Native 'frontend' 'node.exe' "$root\frontend" 'node_modules\vite\bin\vite.js --host 127.0.0.1'
Wait-ForUrl 'Frontend' 'http://127.0.0.1:5173/'

& "$PSScriptRoot\health-check.ps1"
