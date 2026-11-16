# Castcade: watch a recorded game. Started by "Castcade Watch Recording.bat".
#
# Finds the recordings of the table in castcade-recordings\ beside the exe (made with "Castcade Record.bat"),
# takes the newest one or lets you choose when there are several, asks which connection to simulate, then starts
#   the replay   in this window (castcade-replay.ps1, on 127.0.0.1 only: nothing leaves this PC)
#   a watcher    with your normal settings (so: in the headset, if that is how you play), connected to the replay
# The watcher shows the recorded game from your own viewpoint, as if the player were live. The replay ends when the watcher's VPX exits (close the table, then VPX).
param(
    [string]$Table,
    [string]$Connection, # perfect, good, fair or poor: skips the question
    [int]$Pick = 0,      # 1 = the newest recording, 2 = the one before, ...: skips the choice
    [int]$Port = 0,      # first port to try (default 47801, or CASTCADE_REPLAY_PORT when set)
    [ValidateSet(0, 1)][int]$Smooth = 1 # 1: after a gap in the stream the watcher catches up quickly; 0: it jumps (as before), to compare
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here 'VPinballX_GL64_castcade.exe'
$replay = Join-Path $here 'castcade-replay.ps1'
$dir = Join-Path $here 'castcade-recordings'
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "VPinballX_GL64_castcade.exe is not in $here"; exit 1 }
if (-not (Test-Path -LiteralPath $replay)) { Write-Host "castcade-replay.ps1 is not in $here"; exit 1 }

if (-not $Table) { $Table = (Read-Host 'Path of the table (.vpx) the recording was made with').Trim('"') }
if (-not $Table -or -not (Test-Path -LiteralPath $Table -PathType Leaf)) { Write-Host "Table not found: $Table"; exit 1 }
$Table = (Resolve-Path -LiteralPath $Table).ProviderPath
$tableName = Split-Path -Leaf $Table

try { $all = @(& $replay -List $dir) } catch { Write-Host "The recordings in $dir could not be read: $($_.Exception.Message)"; exit 1 }
if (-not $all.Count) { Write-Host "There are no recordings in $dir yet. Make one first: drag the table onto ""Castcade Record.bat"" and play."; exit 1 }
$mine = @($all | Where-Object { $_.Table -ieq $tableName })
if (-not $mine.Count) {
    Write-Host "There is no recording of $tableName in $dir."
    $others = @($all | Where-Object { $_.Table } | ForEach-Object { $_.Table } | Sort-Object -Unique)
    if ($others.Count) { Write-Host "The recordings there are of: $($others -join ', ')" }
    exit 1
}
$playable = @($mine | Where-Object { $_.Playable })
if (-not $playable.Count) {
    Write-Host "No recording of $tableName can be played:"
    foreach ($r in $mine) { Write-Host "  $($r.Name): $($r.Problem)" }
    exit 1
}

function Describe($r) {
    $s = '{0:yyyy-MM-dd HH:mm}  {1,2}:{2:00}  {3,6:N1} MB' -f $r.Start, [math]::Floor($r.Seconds / 60), [math]::Floor($r.Seconds % 60), ($r.Bytes / 1MB)
    if ($r.Problem) { $s += "  ($($r.Problem))" } elseif (-not $r.Closed) { $s += '  (VPX did not close it normally)' }
    $s
}
Write-Host ''
Write-Host "  Castcade - WATCH A RECORDING of $tableName"
Write-Host ''
$chosen = $playable[0]
if ($Pick -ge 1 -and $Pick -le $playable.Count) { $chosen = $playable[$Pick - 1] }
elseif ($playable.Count -gt 1) {
    Write-Host '  Recordings of this table, newest first:'
    for ($i = 0; $i -lt $playable.Count; $i++) { Write-Host ('    {0,2}  {1}' -f ($i + 1), (Describe $playable[$i])) }
    $answer = Read-Host '  Which one (Enter = 1, the newest)'
    $n = 0
    if ($answer -and [int]::TryParse($answer.Trim(), [ref]$n) -and $n -ge 1 -and $n -le $playable.Count) { $chosen = $playable[$n - 1] }
    elseif ($answer) { Write-Host '  Not one of the numbers: taking the newest.' }
}
Write-Host "  Recording: $(Describe $chosen)"
Write-Host ''

$presets = @(& $replay -Presets)
if (-not ($presets | Where-Object { $_.Name -eq $Connection })) {
    Write-Host '  Connection to simulate (synthetic: made-up numbers, not a measurement of anyone''s line):'
    for ($i = 0; $i -lt $presets.Count; $i++) { Write-Host ('    {0}  {1,-8} {2}' -f ($i + 1), $presets[$i].Name, $presets[$i].Text) }
    $answer = Read-Host '  Which (Enter = 1, perfect)'
    $n = 0
    $Connection = 'perfect'
    if ($answer -and [int]::TryParse($answer.Trim(), [ref]$n) -and $n -ge 1 -and $n -le $presets.Count) { $Connection = $presets[$n - 1].Name }
    elseif ($answer -and ($presets | Where-Object { $_.Name -eq $answer.Trim() })) { $Connection = $answer.Trim() }
    elseif ($answer) { Write-Host '  Not one of the choices: perfect.' }
}

# A free port on 127.0.0.1 (listening on loopback only raises no firewall prompt)
if ($Port -le 0) { $Port = if ($env:CASTCADE_REPLAY_PORT) { [int]$env:CASTCADE_REPLAY_PORT } else { 47801 } }
$free = 0
foreach ($p in $Port..($Port + 19)) {
    $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback, $p)
    try { $l.ExclusiveAddressUse = $true; $l.Start(); $l.Stop(); $free = $p; break } catch { }
}
if (-not $free) { Write-Host "No free port between $Port and $($Port + 19) on 127.0.0.1."; exit 1 }

Write-Host ''
Write-Host "  Starting the WATCHER (your normal settings): $Table"
$psi = New-Object Diagnostics.ProcessStartInfo $exe
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $here
$psi.Arguments = '-Play "' + $Table + '"'
foreach ($k in @($psi.EnvironmentVariables.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { $psi.EnvironmentVariables.Remove($k) }
$psi.EnvironmentVariables['CASTCADE_STREAM'] = 'watch'
$psi.EnvironmentVariables['CASTCADE_STREAM_ADDR'] = "connect:127.0.0.1:$free"
# As "Castcade Watch.bat": 100 ms behind the newest frame, the table's own sounds on (no player on this PC makes them)
$psi.EnvironmentVariables['CASTCADE_STREAM_DELAY_MS'] = '100'
$psi.EnvironmentVariables['CASTCADE_STREAM_SOUND'] = '1'
$psi.EnvironmentVariables['CASTCADE_STREAM_SMOOTH'] = "$Smooth"
$psi.EnvironmentVariables['CASTCADE_STREAM_LOG'] = (Join-Path $here 'castcade-watch-recording.log')
Write-Host $(if ($Smooth) { '  After a gap in the recording the watcher catches up quickly (smoothing on).' } else { '  Smoothing OFF: after a gap in the recording the watcher jumps, as before.' })
try { $watcher = [Diagnostics.Process]::Start($psi) } catch { Write-Host "  The watcher could not be started: $($_.Exception.InnerException.Message)"; exit 1 }
Write-Host "  The watcher connects once the table has loaded. To end the replay, close the table and then VPX."

& $replay -Recording $chosen.Path -Port $free -Connection $Connection -WatchPid $watcher.Id -LogFile (Join-Path $here 'castcade-replay.log')
exit $LASTEXITCODE
