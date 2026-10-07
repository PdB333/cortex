<#
.SYNOPSIS
  Prepare the dedicated Windows VM for benchmark runs (run once, inside the guest, then take the snapshot).

.DESCRIPTION
  Lays out C:\bench, copies the pinned builds, records what was installed and its hashes, and leaves nothing
  running. The hypervisor snapshot taken right after this script is the state every run starts from.

  Usage (elevated PowerShell in the guest):
    .\prepare.ps1 -Cortex D:\share\Cortex -Targets D:\share\bench_build [-Python C:\Python312\python.exe]

  -Cortex   folder of the pinned Cortex build (cortex.exe, cortex_core.dll, runtime\, ...)
  -Targets  folder with v1\bench_target.exe and v2\bench_target.exe (bench/build.sh)
#>
param(
    [Parameter(Mandatory = $true)][string]$Cortex,
    [Parameter(Mandatory = $true)][string]$Targets,
    [string]$Python = "python"
)
$ErrorActionPreference = "Stop"

$root = "C:\bench"
New-Item -ItemType Directory -Force -Path "$root\cortex", "$root\targets", "$root\results", "$root\harness" | Out-Null
Copy-Item -Recurse -Force "$Cortex\*" "$root\cortex"
Copy-Item -Recurse -Force "$Targets\*" "$root\targets"

# The harness itself is copied from the repository checkout this script lives in.
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Copy-Item -Recurse -Force "$repo\bench\*" "$root\harness"

# Nothing from an earlier run may survive in the snapshot.
Get-ChildItem -Path "$root\cortex" -Recurse -Include "cortex_projects", "cortex_denied_mutations.jsonl", `
    "cortex_debugger_events.jsonl", "cortex.mcp.*.token" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# Defender and the indexer must not touch the target or the runtime while a run measures it.
try { Add-MpPreference -ExclusionPath $root } catch { Write-Warning "could not add the Defender exclusion: $_" }

$report = [ordered]@{
    prepared_utc = (Get-Date).ToUniversalTime().ToString("o")
    windows = (Get-CimInstance Win32_OperatingSystem).Caption + " " + (Get-CimInstance Win32_OperatingSystem).Version
    python = (& $Python --version) 2>&1
    hashes = Get-ChildItem -Recurse -File "$root\cortex", "$root\targets" -Include *.exe, *.dll |
        ForEach-Object { [ordered]@{ path = $_.FullName.Substring($root.Length + 1); sha256 = (Get-FileHash $_.FullName).Hash } }
}
$report | ConvertTo-Json -Depth 5 | Set-Content "$root\vm_state.json"
Write-Host "Prepared. Take the hypervisor snapshot now; record its name in the run log."
