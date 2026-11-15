# PC measurement lock shared by parallel measurement jobs (dot-source after shared_root.ps1). Rules (2026-11-14):
#  - <shared root>\out\pc-measure.lock: whoever runs predator.exe for any reason (measurement, test, PGO training,
#    first start with its game code build) or a parallel compile of the game code holds it (content: owner ID and
#    start time), at most ~20 min, then releases it. Building predator.exe itself (ninja) does not need it.
#  - <shared root>\out\pc-measure.queue is a FIFO, one waiting owner per line. Whoever has to wait appends its line;
#    only the owner in the first line (or anyone, while the queue is empty) may take the free lock, and removes its
#    line when it does. Whoever releases the lock must not take it again while a foreign line stands first.
#  - <shared root>\out\pc-measure.priority: while it exists, only the owner named in it may take the lock (a priority
#    window set by hand); everyone else waits without queueing.
#  - After each release at least 60 s pass before the same owner takes the lock again (not during its own priority).
# TakeLock "<what>" waits and takes it, DropLock releases it. $LockOwner identifies this checkout (path of the repo).
$script:LockFile = Join-Path (Get-FcpSharedRoot) "out\pc-measure.lock"
$script:QueueFile = Join-Path (Get-FcpSharedRoot) "out\pc-measure.queue"
$script:PriorityFile = Join-Path (Get-FcpSharedRoot) "out\pc-measure.priority"
$script:LockOwner = "$(Split-Path -Leaf (Resolve-Path "$PSScriptRoot\..").Path) ($((Resolve-Path "$PSScriptRoot\..").Path))"
$script:LockId = Split-Path -Leaf (Resolve-Path "$PSScriptRoot\..").Path
$script:LastRelease = [DateTime]::MinValue

function Read-Small([string]$path) {
  try { return [IO.File]::ReadAllText($path) } catch { return $null }
}

function Read-Queue() {
  $q = Read-Small $script:QueueFile
  if (-not $q) { return @() }
  return @($q -split "`r?`n" | Where-Object { $_.Trim() })
}

function Write-Queue([string[]]$lines) {
  try {
    if ($lines.Count -eq 0) { Remove-Item $script:QueueFile -ErrorAction SilentlyContinue }
    else { [IO.File]::WriteAllText($script:QueueFile, (($lines -join "`n") + "`n")) }
  } catch { }
}

function TakeLock([string]$what) {
  while ($true) {
    $prio = Read-Small $script:PriorityFile
    $mine = $prio -and $prio.Contains($script:LockId)
    if ($prio -and -not $mine) { Start-Sleep -Seconds 5; continue }
    $holder = Read-Small $script:LockFile
    $queue = @(Read-Queue)
    $inQueue = @($queue | Where-Object { $_.Contains($script:LockId) }).Count -gt 0
    $first = ($queue.Count -eq 0) -or $queue[0].Contains($script:LockId)
    $pause = (-not $mine) -and (([DateTime]::Now - $script:LastRelease).TotalSeconds -lt 60)
    if ($holder -or (-not $first -and -not $mine) -or $pause) {
      if (-not $inQueue -and -not $mine -and ($holder -or -not $first)) {
        try { [IO.File]::AppendAllText($script:QueueFile, "$script:LockOwner $(Get-Date -Format s)`n") } catch { }
      }
      Start-Sleep -Seconds 5
      continue
    }
    try {
      $fs = [IO.File]::Open($script:LockFile, 'CreateNew', 'Write')
      $b = [Text.Encoding]::ASCII.GetBytes("$script:LockOwner $(Get-Date -Format s) $what")
      $fs.Write($b, 0, $b.Length); $fs.Close()
    } catch { Start-Sleep -Seconds 5; continue }
    Write-Queue @(@(Read-Queue) | Where-Object { -not $_.Contains($script:LockId) })
    return
  }
}

function DropLock() {
  $holder = Read-Small $script:LockFile
  if ($holder -and $holder.Contains($script:LockOwner)) { Remove-Item $script:LockFile -ErrorAction SilentlyContinue }
  $script:LastRelease = [DateTime]::Now
}
