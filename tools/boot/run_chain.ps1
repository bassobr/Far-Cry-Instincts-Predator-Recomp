# Starts predator.exe like run.ps1 and follows the relaunch chain (child processes of its own process,
# recursively, by ParentProcessId). Ends only processes of this chain, by PID.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot\run_chain.ps1 [-Seconds 40]
#       [-HardSeconds 240] [-Env "A=1;B=2"] [-Action none|close|killparent|killchild] [-ActionGen 1]
#       [-ActionDelayMs 0]
# -Action takes effect as soon as the process of generation -ActionGen appears (hand-over in progress):
#   close      WM_CLOSE to the window of the old process (CloseMainWindow)
#   killparent hard-end the old process (Stop-Process -Id)
#   killchild  hard-end the new process (Stop-Process -Id)
# At the end: list of all processes of the chain with exit code, and a check that none is still running.
# FCP_GAME_ISO = the shared disc image (tools\shared_root.ps1; an FCP_GAME_ISO already set wins).
param(
  [string]$Config = "relwithdebinfo",
  [int]$Seconds = 40,
  [int]$HardSeconds = 240,
  [string[]]$Env = @(),
  [string]$Action = "none",
  [int]$ActionGen = 1,
  [int]$ActionDelayMs = 0
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
if (-not (Test-Path $exe)) { throw "missing: $exe" }

$vars = @{
  "FCP_GAME_ISO" = (Get-FcpGameIso)
  "FCP_NO_DIALOGS" = "1"
  "FCP_MUTE" = "1"
  "FCP_EXIT_AFTER" = "$Seconds"
  "REX_UI_WINDOW_WIDTH" = "640"
  "REX_UI_WINDOW_HEIGHT" = "360"
}
foreach ($kv in ($Env -join ";").Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }

$start = Get-Date
$p0 = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$chain = @{}                    # PID -> @{Gen; Proc; Parent; Seen}
$null = $p0.Handle
$chain[$p0.Id] = @{ Gen = 0; Proc = $p0; Parent = 0; Seen = $start }
Write-Output "chain: gen 0 PID $($p0.Id) started $($start.ToString('HH:mm:ss'))"
$actionDone = $false
while ($true) {
  Start-Sleep -Milliseconds 200
  $ids = @($chain.Keys)
  $filter = ($ids | ForEach-Object { "ParentProcessId=$_" }) -join " OR "
  foreach ($c in @(Get-CimInstance Win32_Process -Filter "($filter) AND Name='predator.exe'" | Where-Object { $_.ExecutablePath -eq $exe })) {
    if (-not $chain.ContainsKey([int]$c.ProcessId)) {
      $parent = [int]$c.ParentProcessId
      $gen = $chain[$parent].Gen + 1
      try { $proc = Get-Process -Id $c.ProcessId; $null = $proc.Handle } catch { $proc = $null }
      $chain[[int]$c.ProcessId] = @{ Gen = $gen; Proc = $proc; Parent = $parent; Seen = Get-Date }
      Write-Output ("chain: gen {0} PID {1} (parent {2}) after {3:N1} s" -f $gen, $c.ProcessId, $parent, ((Get-Date) - $start).TotalSeconds)
      if (-not $actionDone -and $Action -ne "none" -and $gen -eq $ActionGen) {
        $actionDone = $true
        if ($ActionDelayMs -gt 0) { Start-Sleep -Milliseconds $ActionDelayMs }
        $old = $chain[$parent].Proc
        switch ($Action) {
          "close" { $r = $old.CloseMainWindow(); Write-Output "chain: WM_CLOSE to PID $($old.Id) (window: $r)" }
          "killparent" { Stop-Process -Id $old.Id -Force; Write-Output "chain: PID $($old.Id) hard-ended" }
          "killchild" { Stop-Process -Id $c.ProcessId -Force; Write-Output "chain: PID $($c.ProcessId) hard-ended" }
        }
      }
    }
  }
  $alive = @($chain.Values | Where-Object { $_.Proc -and -not $_.Proc.HasExited })
  if ($alive.Count -eq 0) { break }
  if (((Get-Date) - $start).TotalSeconds -gt $HardSeconds) {
    foreach ($e in $alive) { $e.Proc.Refresh(); Write-Output "chain: PID $($e.Proc.Id) still running after $HardSeconds s (window: '$($e.Proc.MainWindowTitle)'), ending it"; Stop-Process -Id $e.Proc.Id -Force }
    Start-Sleep -Seconds 2
  }
}
$dur = ((Get-Date) - $start).TotalSeconds
foreach ($id in ($chain.Keys | Sort-Object { $chain[$_].Gen })) {
  $e = $chain[$id]
  $code = if ($e.Proc) { $e.Proc.ExitCode } else { "?" }
  Write-Output ("chain: gen {0} PID {1} exit code {2}" -f $e.Gen, $id, $code)
}
# Check only our own process objects (after the end a PID may already belong to a foreign process).
$left = @($chain.Keys | Where-Object { $chain[$_].Proc -and -not $chain[$_].Proc.HasExited })
Write-Output ("chain: done after {0:N1} s, {1} processes, still running: {2}" -f $dur, $chain.Count, $left.Count)
