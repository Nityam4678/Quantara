$ErrorActionPreference = 'SilentlyContinue'
$checks = @(
    @{ Name = 'API'; Url = 'http://127.0.0.1:3000/api/health' },
    @{ Name = 'AI'; Url = 'http://127.0.0.1:8000/health' },
    @{ Name = 'Frontend'; Url = 'http://127.0.0.1:5173/' }
)
$failed = $false
foreach ($check in $checks) {
    try {
        $response = Invoke-WebRequest -Uri $check.Url -TimeoutSec 5 -UseBasicParsing
        Write-Host "[OK] $($check.Name) $($response.StatusCode)"
    } catch {
        Write-Host "[FAIL] $($check.Name): $($_.Exception.Message)" -ForegroundColor Red
        $failed = $true
    }
}
if (Get-Command redis-cli -ErrorAction SilentlyContinue) {
    try { redis-cli ping | Out-Null; Write-Host '[OK] Redis' } catch { Write-Host '[FAIL] Redis'; $failed = $true }
} else { Write-Warning 'Redis CLI not found; Redis was not checked.' }
if ($failed) { exit 1 }
