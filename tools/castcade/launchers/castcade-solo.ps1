# Castcade state stream: solo test on one PC. Started by "Castcade Solo Test.bat".
#
# Starts two instances of VPinballX_GL64_castcade.exe on the same table:
#   player   a desktop window with VR off (its own copy of your ini), listening on this PC only
#   watcher  your normal settings (so: in the headset if that is how you play), connected to the player
# Click the player's window so it has the keyboard, then play: the watcher shows the same game from your viewpoint.
param(
    [string]$Table,
    [int]$Port = 47800
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here 'VPinballX_GL64_castcade.exe'
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "VPinballX_GL64_castcade.exe is not in $here"; exit 1 }

if (-not $Table) { $Table = (Read-Host 'Path of the table (.vpx)').Trim('"') }
if (-not (Test-Path -LiteralPath $Table)) { Write-Host "Table not found: $Table"; exit 1 }
$Table = (Resolve-Path -LiteralPath $Table).ProviderPath

# The ini VPX itself would use: the one beside the exe when there is one, else the one in %APPDATA%
$ini = Join-Path $here 'VPinballX.ini'
if (-not (Test-Path -LiteralPath $ini)) { $ini = Join-Path $env:APPDATA 'VPinballX\VPinballX.ini' }
if (-not (Test-Path -LiteralPath $ini)) { Write-Host 'No VPinballX.ini found beside the exe or in %APPDATA%\VPinballX. Run VPX once first.'; exit 1 }

# The player's ini: a copy with VR off and a window instead of full screen. Yours is not touched.
$work = Join-Path $env:TEMP 'castcade-solo'
New-Item -ItemType Directory -Force $work | Out-Null
$playerIni = Join-Path $work 'player.ini'
$set = @{
    'PlayerVR' = [ordered]@{ 'AskToTurnOn' = '2' }
    'Player'   = [ordered]@{ 'FullScreen' = '0'; 'Width' = '1280'; 'Height' = '720'; 'Display' = '-1'; 'WindowPosX' = '60'; 'WindowPosY' = '60'; 'MaxFramerate' = '60' }
}
$out = New-Object System.Collections.Generic.List[string]
$done = @{}
$section = ''
$flush = {
    param($s)
    if ($s -and $set.ContainsKey($s)) {
        foreach ($k in $set[$s].Keys) { if (-not $done.ContainsKey("$s/$k")) { $out.Add("$k = $($set[$s][$k])"); $done["$s/$k"] = $true } }
    }
}
foreach ($line in [IO.File]::ReadAllLines($ini)) {
    if ($line -match '^\s*\[(.+)\]\s*$') {
        & $flush $section
        $section = $Matches[1]
        $out.Add($line)
        continue
    }
    if ($set.ContainsKey($section) -and $line -match '^\s*([^=;\s]+)\s*=') {
        $key = $Matches[1]
        if ($set[$section].Contains($key)) {
            $out.Add("$key = $($set[$section][$key])")
            $done["$section/$key"] = $true
            continue
        }
    }
    $out.Add($line)
}
& $flush $section
foreach ($s in $set.Keys) {
    $missing = @($set[$s].Keys | Where-Object { -not $done.ContainsKey("$s/$_") })
    if ($missing.Count) { $out.Add("[$s]"); foreach ($k in $missing) { $out.Add("$k = $($set[$s][$k])") } }
}
[IO.File]::WriteAllLines($playerIni, $out)

function Start-Vpx([hashtable]$environment, [string[]]$arguments) {
    $psi = New-Object Diagnostics.ProcessStartInfo $exe
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $here
    $psi.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    foreach ($k in @($psi.EnvironmentVariables.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { $psi.EnvironmentVariables.Remove($k) }
    foreach ($k in $environment.Keys) { $psi.EnvironmentVariables[$k] = $environment[$k] }
    [Diagnostics.Process]::Start($psi)
}

Write-Host ''
Write-Host "Starting the PLAYER (desktop window, VR off): $Table"
# Listening on 127.0.0.1 only: no firewall prompt, nobody else can connect. NOPAUSE keeps the game running while
# the window is not focused; the keyboard still only reaches it while it IS focused.
$player = Start-Vpx @{
    CASTCADE_STREAM       = 'play'
    CASTCADE_STREAM_ADDR  = "listen:127.0.0.1:$Port"
    CASTCADE_TEST_NOPAUSE = '1'
    CASTCADE_STREAM_LOG   = (Join-Path $here 'castcade-play.log')
} @('-Ini', $playerIni, '-Play', $Table)

Write-Host 'Waiting 12 seconds for it to load...'
Start-Sleep -Seconds 12

Write-Host 'Starting the WATCHER (your normal settings).'
# Sound off on the watcher: the player instance on this same PC already makes every sound.
$watcher = Start-Vpx @{
    CASTCADE_STREAM          = 'watch'
    CASTCADE_STREAM_ADDR     = "connect:127.0.0.1:$Port"
    CASTCADE_STREAM_DELAY_MS = '50'
    CASTCADE_STREAM_SOUND    = '0'
    CASTCADE_STREAM_LOG      = (Join-Path $here 'castcade-watch.log')
} @('-Play', $Table)

Write-Host ''
Write-Host 'Both are starting. Now:'
Write-Host '  1. Click the PLAYER window (the 1280x720 desktop window) so it has the keyboard.'
Write-Host '  2. Put the headset on: the watcher shows the same table from your own viewpoint.'
Write-Host '  3. Play with the keyboard. Close each VPX the usual way when done.'
Write-Host ''
Write-Host "Logs: $here\castcade-play.log and castcade-watch.log (player PID $($player.Id), watcher PID $($watcher.Id))"
