<#
.SYNOPSIS
  In-guest cleanup between two stages of ONE run (not between runs: between runs the VM is reverted to its snapshot).

.DESCRIPTION
  Stops any leftover target or Cortex server and removes the refusal and debugger logs. It does NOT remove the
  Cortex project folder, because task 9 stage B must see what stage A kept. The runner removes projects at the
  start of each run.
#>
$ErrorActionPreference = "SilentlyContinue"
Get-Process -Name bench_target, cortex, cortex_runtime_helper | Stop-Process -Force
Get-ChildItem -Path "C:\bench\cortex" -Recurse -Include "cortex_denied_mutations.jsonl", "cortex_debugger_events.jsonl", "cortex.mcp.*.token" |
    Remove-Item -Force
