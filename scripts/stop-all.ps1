$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$state = Join-Path $root '.quantara'
if (-not (Test-Path $state)) { Write-Host 'No Quantara process state found.'; exit 0 }

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

Get-ChildItem "$state\*.pid" | ForEach-Object {
    $processId = [int](Get-Content $_.FullName)
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($_.BaseName -eq 'postgres' -and $env:POSTGRES_BIN -and $env:PGDATA) {
        & "$env:POSTGRES_BIN\pg_ctl.exe" -D $env:PGDATA stop -m fast | Out-Null
        Write-Host 'Requested PostgreSQL shutdown'
    } elseif ($process) {
        Write-Host "Stopping $($_.BaseName) (PID $processId)"
        Stop-Process -Id $processId -ErrorAction SilentlyContinue
    }
    Remove-Item $_.FullName -Force
}
