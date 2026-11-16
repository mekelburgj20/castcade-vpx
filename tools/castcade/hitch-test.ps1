<#
.SYNOPSIS
  Frame-hitch test: one VPX plays a scripted game with the frame probe on (CASTCADE_FRAMELOG), with the stream off
  or with the player recording, and the long frames are summarised. Desktop window, VR off, loopback-free (no
  socket is opened), no keyboard or mouse input to any window (keys are fired inside VPX via CASTCADE_TEST_INPUT).

.DESCRIPTION
  -Mode off      no CASTCADE_STREAM: the session hooks are compiled in and each is one null test
  -Mode record   CASTCADE_STREAM=play, CASTCADE_STREAM_RECORD=<run>\rec\ and the stream log (no address: no socket)
  -Sound         VPX's own table sounds on (PlaySound=1) at volume 0, so the sound code runs and nothing VPX plays is
                 heard; VPinMAME's ROM sound follows VPinMAME's own settings either way
  Output under C:\code\repos\castcade-vpx-deps\run\hitch\<yyyyMMdd-HHmmss>-<label>\: run.log, play.ini, play\
  (vpinball.log, castcade.log, frames.log), rec\ (record mode), summary.json.
  Closes only what it started (WM_CLOSE, kill as a last resort on its own PID). Does not restore the type-library
  registration or BAMViewSettings.xml: the caller does that once at the end.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Table,
    [Parameter(Mandatory)][string]$InputFile,
    [ValidateSet('off', 'record')][string]$Mode = 'off',
    [string]$Exe = 'C:\code\repos\castcade-vpx-wt-state\build\Release\cctest_GL64.exe',
    [int]$Seconds = 200,
    [int]$FrameLogMs = 25,
    [switch]$Sound,
    [int]$CacheMode = 0,   # VPX's texture preload cache (CacheMode=1 is VPX's default; it reads and rewrites %APPDATA%\VPinballX\Cache\<table>\used_textures.xml)
    [switch]$NoStreamLog,  # no CASTCADE_STREAM_LOG: the session mirrors every line into the VPX log (vpinball.log, EnableLog=1)
    [string]$Label = '',
    [string]$OutDir,
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$WindowTimeout = 150,
    [hashtable]$ExtraEnv = @{},
    [string]$Template = 'C:\code\repos\castcade-vpx-deps\run\smoke.template.ini'
)
$ErrorActionPreference = 'Stop'
if ((Split-Path -Leaf $Exe) -like 'VPinballX*') { Write-Host 'hitch-test.ps1: use a copy of the exe whose name does not start with VPinballX'; exit 1 }
if (-not $OutDir) { $OutDir = Join-Path 'C:\code\repos\castcade-vpx-deps\run\hitch' ((Get-Date -Format 'yyyyMMdd-HHmmss') + $(if ($Label) { "-$Label" } else { "-$Mode" })) }
$Run = $OutDir
$SW = [Diagnostics.Stopwatch]::StartNew()
$LogFile = $null
function Log([string]$m) {
    $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
    Write-Host $line
    if ($script:LogFile) { try { Add-Content -LiteralPath $script:LogFile -Value $line } catch {} }
}

if (-not ('HtWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class HtWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public class Win { public IntPtr H; public string Title; public string Cls; public bool Visible; }
    public static List<Win> ForPid(uint want) {
        var list = new List<Win>();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want) {
                var t = new StringBuilder(512); GetWindowText(h, t, 512);
                var c = new StringBuilder(256); GetClassName(h, c, 256);
                list.Add(new Win { H = h, Title = t.ToString(), Cls = c.ToString(), Visible = IsWindowVisible(h) });
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
'@
}

foreach ($f in @($Exe, $Table, $Template, $InputFile)) { if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Write-Host "missing file: $f"; exit 1 } }
New-Item -ItemType Directory -Force $Run | Out-Null
$LogFile = Join-Path $Run 'run.log'

# ---------------------------------------------------------------- ini (as pair.ps1 / replay-test.ps1)
$Keys = [ordered]@{ StartGameKey = 2; AddCreditKey = 6; PlungerKey = 28; LFlipKey = 42; RFlipKey = 54; ExitGameKey = 16 }
function New-Ini([string]$dest, [int]$posX) {
    $player = [ordered]@{ FullScreen = '0'; Width = "$Width"; Height = "$Height"; Display = '-1'; WindowPosX = "$posX"; WindowPosY = '60'; PlaySound = '0'; PlayMusic = '0'; CacheMode = "$CacheMode" }
    if ($Sound) { $player.PlaySound = '1'; $player.SoundVolume = '0'; $player.MusicVolume = '0' }
    $set = [ordered]@{ 'Player' = $player; 'PlayerVR' = [ordered]@{ AskToTurnOn = '2' }; 'Editor' = [ordered]@{ EnableLog = '1' } }
    foreach ($k in $Keys.Keys) { $set['Player'][$k] = "$($Keys[$k])" }
    $out = New-Object System.Collections.Generic.List[string]; $sec = ''; $done = @{}
    $flush = { param($s) if ($set.Contains($s)) { foreach ($k in $set[$s].Keys) { if (-not $done.ContainsKey("$s/$k")) { $out.Add("$k = $($set[$s][$k])"); $done["$s/$k"] = 1 } } } }
    foreach ($ln in [IO.File]::ReadAllLines($Template)) {
        if ($ln -match '^\s*\[(.+?)\]\s*$') { & $flush $sec; $sec = $Matches[1]; $out.Add($ln); continue }
        if ($set.Contains($sec) -and $ln -match '^\s*([^=;#]+?)\s*=') {
            $hit = $set[$sec].Keys | Where-Object { $_ -ieq $Matches[1] } | Select-Object -First 1
            if ($hit) { if (-not $done.ContainsKey("$sec/$hit")) { $out.Add("$($Matches[1]) = $($set[$sec][$hit])"); $done["$sec/$hit"] = 1 }; continue }
        }
        $out.Add($ln)
    }
    & $flush $sec
    [IO.File]::WriteAllLines($dest, $out, (New-Object System.Text.UTF8Encoding($false)))
}

$P = $null
$fail = $null
try {
    $dir = Join-Path $Run 'play'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $ini = Join-Path $Run 'play.ini'
    New-Ini $ini 20
    Copy-Item -LiteralPath $InputFile -Destination (Join-Path $Run 'input.txt')
    $envs = @{ CASTCADE_TEST_NOPAUSE = '1'; CASTCADE_TEST_INPUT = (Resolve-Path -LiteralPath $InputFile).Path; CASTCADE_FRAMELOG = (Join-Path $dir 'frames.log');
               CASTCADE_FRAMELOG_MS = "$FrameLogMs"; CASTCADE_STREAM_LOG = (Join-Path $dir 'castcade.log') }
    if ($Mode -eq 'record') { $envs['CASTCADE_STREAM'] = 'play'; $envs['CASTCADE_STREAM_RECORD'] = (Join-Path $Run 'rec') + '\' }
    if ($NoStreamLog) { $envs.Remove('CASTCADE_STREAM_LOG') }
    foreach ($k in $ExtraEnv.Keys) { $envs[$k] = "$($ExtraEnv[$k])" }
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $dir
    foreach ($a in @('-Ini', $ini, '/ExtMinimized', '-Play', $Table)) { $psi.ArgumentList.Add($a) }
    foreach ($k in @($psi.Environment.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { [void]$psi.Environment.Remove($k) }
    foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
    Log "run folder $Run; exe $Exe; table $Table; mode $Mode; sound $([bool]$Sound); other VPX processes (left alone): $((Get-Process -Name 'VPinballX*' -ErrorAction SilentlyContinue | ForEach-Object Id) -join ',')"
    $proc = [Diagnostics.Process]::Start($psi)
    $null = $proc.Handle
    $P = @{ Proc = $proc; Pid = [uint32]$proc.Id; Killed = $false }
    Log ("start PID {0}, env {1}" -f $proc.Id, (($envs.Keys | Sort-Object | ForEach-Object { "$_=$($envs[$_])" }) -join ' '))
    $win = $null
    while (-not $win) {
        if ($proc.HasExited) { throw "VPX exited early, code $($proc.ExitCode)" }
        $win = [HtWin]::ForPid($P.Pid) | Where-Object { $_.Visible -and $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
        if (-not $win -and $SW.Elapsed.TotalSeconds -gt $WindowTimeout) { throw "no player window after $WindowTimeout s" }
        Start-Sleep -Milliseconds 100
    }
    Log "player window after $([math]::Round($SW.Elapsed.TotalSeconds, 1)) s; playing $Seconds s"
    $t0 = $SW.Elapsed.TotalSeconds
    while ($SW.Elapsed.TotalSeconds - $t0 -lt $Seconds) {
        if ($proc.HasExited) { throw "VPX exited early (code $($proc.ExitCode))" }
        Start-Sleep -Milliseconds 250
    }
} catch {
    $fail = "$_"; Log "FAILED: $fail"
} finally {
    if ($P -and -not $P.Proc.HasExited) {
        $pl = [HtWin]::ForPid($P.Pid) | Where-Object { $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
        if ($pl) { Log 'WM_CLOSE -> player window'; [void][HtWin]::PostMessage($pl.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        if (-not $P.Proc.WaitForExit(25000)) {
            $frame = [HtWin]::ForPid($P.Pid) | Where-Object { $_.Cls -eq 'VPinball' } | Select-Object -First 1
            if ($frame) { Log 'still running; WM_CLOSE -> its VPinball frame'; [void][HtWin]::PostMessage($frame.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); [void]$P.Proc.WaitForExit(8000) }
        }
        if (-not $P.Proc.HasExited) { Log "last resort: killing my own PID $($P.Pid)"; $P.Killed = $true; Stop-Process -Id $P.Pid -Force; [void]$P.Proc.WaitForExit(10000) }
        Log ("PID {0} exited, code {1}{2}" -f $P.Pid, $P.Proc.ExitCode, $(if ($P.Killed) { ' (killed)' } else { '' }))
    }
}

# ---------------------------------------------------------------- summary of frames.log
$fl = Join-Path $Run 'play\frames.log'
$sum = [ordered]@{ run = $Run; table = (Split-Path -Leaf $Table); mode = $Mode; sound = [bool]$Sound; frames = 0; ge25 = 0; ge50 = 0; ge100 = 0; ge200 = 0; ge500 = 0; longestMs = 0; long = @() }
if (Test-Path -LiteralPath $fl) {
    foreach ($ln in Get-Content -LiteralPath $fl) {
        if ($ln -match 'probe end: frames (\d+), >=25 ms (\d+), >=50 ms (\d+), >=100 ms (\d+), >=200 ms (\d+), >=500 ms (\d+), longest ([\d.]+) ms') {
            $sum.frames = [int]$Matches[1]; $sum.ge25 = [int]$Matches[2]; $sum.ge50 = [int]$Matches[3]; $sum.ge100 = [int]$Matches[4]; $sum.ge200 = [int]$Matches[5]; $sum.ge500 = [int]$Matches[6]; $sum.longestMs = [double]$Matches[7]
        }
        if ($ln -match ' frame (\d+): ([\d.]+) ms, at ([\d.]+)-([\d.]+) s; .*?\| (.*?) other ([-\d.]+)') {
            $frameNo = [int]$Matches[1]; $ms = [double]$Matches[2]; $sections = $Matches[5]; $other = [double]$Matches[6]
            if ($ms -ge 50) {
                $secs = @{ other = $other }
                foreach ($kv in ($sections -split ' (?=[a-z])')) { if ($kv -match '^([a-z0-9]+) ([-\d.]+)$') { $secs[$Matches[1]] = [double]$Matches[2] } }
                $top = ($secs.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1)
                $sum.long += [pscustomobject]@{ frame = $frameNo; ms = $ms; top = $top.Name; topMs = $top.Value }
            }
        }
    }
    Log ("frames {0}; >=25 ms {1}, >=50 ms {2}, >=100 ms {3}, >=200 ms {4}, >=500 ms {5}; longest {6} ms" -f $sum.frames, $sum.ge25, $sum.ge50, $sum.ge100, $sum.ge200, $sum.ge500, $sum.longestMs)
    foreach ($l in $sum.long) { Log ("  frame {0}: {1} ms, mostly {2} ({3} ms)" -f $l.frame, $l.ms, $l.top, $l.topMs) }
} else { Log 'NO frames.log' }
$sum | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Run 'summary.json')
if ($fail) { Log "RUN FAILED: $fail"; exit 1 }
Log 'RUN OK'
exit 0
