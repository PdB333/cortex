<#
.SYNOPSIS
  Replays the MCP write-authority regression test on a real Windows machine.

.DESCRIPTION
  Runs tests/security_no_writes_regression.py against an unpacked Cortex bundle
  for the x64 and x86 test targets, WITHOUT any --skip-tool. If a tool such as
  screenshot or debug_registers freezes the target on Windows, the test fails
  and names the call: that is a Cortex bug to fix, not something to skip.

  Usage (from the repository root, on the VM):
    powershell -ExecutionPolicy Bypass -File tests\run_security_regression_vm.ps1 -Bundle C:\Cortex

  -Bundle  folder that holds cortex.exe, cortex_core.dll, runtime\ and e2e\
  -Out     folder for the logs (default: .\security-regression-logs)

  Exit code 0 only if both architectures pass. The commit that was tested and
  both logs are written to -Out so the run can be cited.
#>
param(
    [Parameter(Mandatory = $true)][string]$Bundle,
    [string]$Out = ".\security-regression-logs"
)

$ErrorActionPreference = "Stop"
$bundlePath = (Resolve-Path $Bundle).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$commit = (git rev-parse HEAD 2>$null)
if (-not $commit) { $commit = "unknown" }
"commit: $commit" | Tee-Object -FilePath "$Out\summary.txt"
"bundle: $bundlePath" | Tee-Object -FilePath "$Out\summary.txt" -Append
Get-FileHash "$bundlePath\cortex.exe", "$bundlePath\cortex_core.dll" -Algorithm SHA256 |
    ForEach-Object { "{0}  {1}" -f $_.Hash, (Split-Path $_.Path -Leaf) } |
    Tee-Object -FilePath "$Out\summary.txt" -Append

$failed = $false
foreach ($arch in @("x64", "x86")) {
    $target = "$bundlePath\e2e\cortex_test_target_$arch.exe"
    if (-not (Test-Path $target)) { throw "missing $target" }
    $log = "$Out\regression-$arch.log"
    "== $arch ==" | Tee-Object -FilePath "$Out\summary.txt" -Append
    # No --skip-tool: every tool listed by tools/list is called.
    python tests\security_no_writes_regression.py --cortex "$bundlePath\cortex.exe" --target $target 2>&1 |
        Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) {
        $failed = $true
        "$arch FAILED (see $log)" | Tee-Object -FilePath "$Out\summary.txt" -Append
    } else {
        "$arch passed" | Tee-Object -FilePath "$Out\summary.txt" -Append
    }
}

if ($failed) { exit 1 }
"both architectures passed without --skip-tool" | Tee-Object -FilePath "$Out\summary.txt" -Append
