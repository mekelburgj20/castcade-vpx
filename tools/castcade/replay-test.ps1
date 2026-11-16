<#
.SYNOPSIS
  Record and replay test for the Castcade state stream (CASTCADE_STREAM_RECORD + launchers\castcade-replay.ps1).
  Desktop windows only, loopback only, no keyboard or mouse input to any window (keys are fired inside VPX via
  CASTCADE_TEST_INPUT). See README.md.

.DESCRIPTION
  -Mode record   A player records the table (CASTCADE_STREAM_RECORD=<OutDir>\rec\), playing -InputFile. With
                 -WithWatcher it also listens on 127.0.0.1:-Port and a live watcher A connects (-DelayMs).
                 Without it the player has no CASTCADE_STREAM_ADDR: its listening sockets are logged 10 s in.
                 Screenshots of the player (and A) every -ShotEvery s, and a final one just before the close;
                 shots.json records when each player shot was taken, against the recording's start.
  -Mode replay   A watcher B connects to 127.0.0.1:-Port, where castcade-replay.ps1 (Windows PowerShell 5.1)
                 plays -Recording with -Connection [-Loop]. With -ShotsFrom <record run> B is captured at the
                 same recording times as the player was (plus the delay); a final shot after the end. -Passes
                 (with -Loop) is how many passes to wait for.
  Both: closes only what it started (WM_CLOSE, kill as a last resort on its own PID). Does not restore the
  registry or BAMViewSettings.xml: the caller does that once at the end (other VPX instances may run meanwhile).
#>
[CmdletBinding()]
param(
    [ValidateSet('record', 'replay')][string]$Mode = 'record',
    [Parameter(Mandatory)][string]$Table,
    # A name that does not start with VPinballX: the Castcade companion takes any VPinballX* process that loads a table
    # for a game start (it logs a launch, reports it, and remembers that exe). Copy build\Release\VPinballX_GL64.exe to it.
    [string]$Exe = 'C:\code\repos\castcade-vpx-wt-state\build\Release\cctest_GL64.exe',
    [string]$WatchExe,      # the watchers' exe when it is another build (default: -Exe)
    [string]$Replay ='C:\code\repos\castcade-vpx-wt-state\tools\castcade\launchers\castcade-replay.ps1',
    [int]$Port = 47921,
    [int]$DelayMs = 100,
    [string]$InputFile,
    [int]$Seconds = 60,
    [int]$ShotEvery = 5,
    [switch]$WithWatcher,
    [string]$Recording,
    [string]$RecordTo,      # record: CASTCADE_STREAM_RECORD as given (default <run>\rec\)
    [string]$Connection = 'perfect',
    [switch]$Loop,
    [int]$Passes = 1,
    [string]$ShotsFrom,
    [int]$AfterEndSeconds = 7,
    [string]$OutDir,
    [int]$Width = 960,
    [int]$Height = 540,
    [int]$WindowTimeout = 150,
    [string]$Template = 'C:\code\repos\castcade-vpx-deps\run\smoke.template.ini',
    # VPX's table sounds on (PlaySound=1) at volume 0 in every instance: the sound code runs and nothing VPX plays is
    # heard (the StopSound audit needs it on the watcher); VPinMAME's ROM sound follows VPinMAME's own settings
    [switch]$Sound,
    # Extra environment for the player (record) and the watchers (A, B), e.g. @{ CASTCADE_STREAM_SMOOTH = '0' }
    [hashtable]$PlayEnv = @{},
    [hashtable]$WatchEnv = @{},
    [string]$FrameLog       # a folder: every instance also runs the frame probe (CASTCADE_FRAMELOG=<folder>\<name>-frames.log)
)
$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path 'C:\code\repos\castcade-vpx-deps\run\rr' ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Mode") }
$Run = $OutDir
$SW = [Diagnostics.Stopwatch]::StartNew()
$LogFile = $null
function Log([string]$m) {
    $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
    Write-Host $line
    if ($script:LogFile) { try { Add-Content -LiteralPath $script:LogFile -Value $line } catch {} }
}
if ($Port -lt 47920 -or $Port -gt 47939) { Write-Host 'replay-test.ps1: use a port from 47920 to 47939'; exit 1 }

Add-Type -AssemblyName System.Drawing
if (-not ('RrWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class RrWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public class Win { public IntPtr H; public string Title; public string Cls; public bool Visible;
        public override string ToString() { return string.Format("0x{0:X} cls='{1}' title='{2}' visible={3}", H.ToInt64(), Cls, Title, Visible); } }
    public static void PerMonitorDpi() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }
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
    public static RECT Bounds(IntPtr h) {
        RECT r; if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r); return r;
    }
    public static double[] Diff(byte[] a, byte[] b) {
        int n = a.Length / 4; double sum = 0; int over = 0;
        for (int p = 0; p < n; p++) {
            int o = p * 4;
            int d = Math.Abs(a[o] - b[o]) + Math.Abs(a[o + 1] - b[o + 1]) + Math.Abs(a[o + 2] - b[o + 2]);
            sum += d / 3.0; if (d / 3.0 > 40) over++;
        }
        return new double[] { sum / n, 100.0 * over / n };
    }
}
'@
}
[RrWin]::PerMonitorDpi()

foreach ($p in @($Exe, $Table, $Template)) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Write-Host "missing file: $p"; exit 1 } }
if ($Mode -eq 'replay' -and -not (Test-Path -LiteralPath $Recording -PathType Leaf)) { Write-Host "missing recording: $Recording"; exit 1 }
$l = $null
try { $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port); $l.Start(); $l.Stop() } catch { Write-Host "port $Port is not free on 127.0.0.1"; exit 1 }
New-Item -ItemType Directory -Force $Run, "$Run\shots" | Out-Null
$LogFile = Join-Path $Run 'run.log'

# ---------------------------------------------------------------- inis (as pair.ps1)
$Keys = [ordered]@{ StartGameKey = 2; AddCreditKey = 6; PlungerKey = 28; LFlipKey = 42; RFlipKey = 54; ExitGameKey = 16 }
function New-Ini([string]$dest, [int]$posX) {
    $set = [ordered]@{
        'Player'   = $(if ($Sound) { [ordered]@{ FullScreen = '0'; Width = "$Width"; Height = "$Height"; Display = '-1'; WindowPosX = "$posX"; WindowPosY = '60'; PlaySound = '1'; PlayMusic = '0'; SoundVolume = '0'; MusicVolume = '0'; CacheMode = '0' } }
                       else { [ordered]@{ FullScreen = '0'; Width = "$Width"; Height = "$Height"; Display = '-1'; WindowPosX = "$posX"; WindowPosY = '60'; PlaySound = '0'; PlayMusic = '0'; CacheMode = '0' } })
        'PlayerVR' = [ordered]@{ AskToTurnOn = '2' }
        'Editor'   = [ordered]@{ EnableLog = '1' }
    }
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

# ---------------------------------------------------------------- instances
$Inst = [ordered]@{}
function Start-Vpx([string]$name, [int]$posX, [hashtable]$envs) {
    $dir = Join-Path $Run $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    $ini = Join-Path $Run "$name.ini"
    New-Ini $ini $posX
    $psi = [Diagnostics.ProcessStartInfo]::new($(if ($name -ne 'play' -and $WatchExe) { $WatchExe } else { $Exe }))
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $dir
    foreach ($a in @('-Ini', $ini, '/ExtMinimized', '-Play', $Table)) { $psi.ArgumentList.Add($a) }
    foreach ($k in @($psi.Environment.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { [void]$psi.Environment.Remove($k) }
    $extra = if ($name -eq 'play') { $PlayEnv } else { $WatchEnv }
    foreach ($k in $extra.Keys) { $envs[$k] = "$($extra[$k])" }
    if ($FrameLog) { New-Item -ItemType Directory -Force $FrameLog | Out-Null; $envs['CASTCADE_FRAMELOG'] = Join-Path $FrameLog "$name-frames.log" }
    foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
    $psi.Environment['CASTCADE_STREAM_LOG'] = Join-Path $dir 'castcade.log'
    $p = [Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $x = @{ Name = $name; Proc = $p; Pid = [uint32]$p.Id; Dir = $dir; Win = $null; WinAt = $null; Launched = $SW.Elapsed.TotalSeconds; Exit = $null; Killed = $false }
    $Inst[$name] = $x
    Log ("start {0}: PID {1}, env {2}" -f $name, $p.Id, (($envs.Keys | Sort-Object | ForEach-Object { "$_=$($envs[$_])" }) -join ' '))
    $x
}
function Poll-Window($x) {
    if ($x.Win) { return }
    if ($x.Proc.HasExited) { throw "$($x.Name) (PID $($x.Pid)) exited early, code $($x.Proc.ExitCode)" }
    foreach ($w in [RrWin]::ForPid($x.Pid)) { if ($w.Visible -and $w.Title -eq 'Visual Pinball Player') { $x.Win = $w.H; $x.WinAt = $SW.Elapsed.TotalSeconds; Log "$($x.Name) player window after $([math]::Round($x.WinAt - $x.Launched, 1)) s" } }
    if (-not $x.Win -and ($SW.Elapsed.TotalSeconds - $x.Launched) -gt $WindowTimeout) { throw "no player window for $($x.Name) after $WindowTimeout s" }
}
function Wait-Window($x) { while (-not $x.Win) { Poll-Window $x; Start-Sleep -Milliseconds 100 } }
function Shot($x, [string]$file) {
    $r = [RrWin]::Bounds($x.Win); $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { Log "capture ${file}: empty window"; return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $hdc = $g.GetHdc(); [void][RrWin]::PrintWindow($x.Win, $hdc, 2); $g.ReleaseHdc($hdc) } finally { $g.Dispose() }
    $bmp.Save((Join-Path $Run "shots\$file"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
function Close-Vpx($x, [int]$wait = 25000) {
    if (-not $x -or $x.Proc.HasExited) { if ($x) { $x.Exit = $x.Proc.ExitCode }; return }
    $pl = [RrWin]::ForPid($x.Pid) | Where-Object { $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
    if ($pl) { Log "WM_CLOSE -> $($x.Name) player window"; [void][RrWin]::PostMessage($pl.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    else { [RrWin]::ForPid($x.Pid) | Where-Object Visible | ForEach-Object { [void][RrWin]::PostMessage($_.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }
    if (-not $x.Proc.WaitForExit($wait)) {
        $frame = [RrWin]::ForPid($x.Pid) | Where-Object { $_.Cls -eq 'VPinball' } | Select-Object -First 1
        if ($frame) { Log "$($x.Name) still running; WM_CLOSE -> its VPinball frame"; [void][RrWin]::PostMessage($frame.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); [void]$x.Proc.WaitForExit(8000) }
    }
    if (-not $x.Proc.HasExited) { Log "last resort: killing my own PID $($x.Pid) ($($x.Name))"; $x.Killed = $true; Stop-Process -Id $x.Pid -Force; [void]$x.Proc.WaitForExit(10000) }
    $x.Exit = $x.Proc.ExitCode
    Log ("{0} PID {1} exited, code {2}{3}" -f $x.Name, $x.Pid, $x.Exit, $(if ($x.Killed) { ' (killed)' } else { '' }))
}
# The header's start time; $null until the first second has been written (a directory listing shows 0 bytes for a
# file that is still open, so the file itself is asked)
function Recording-Start([string]$file) {
    $fs = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite')
    try { if ($fs.Length -lt 64) { return $null }; $b = New-Object byte[] 24; [void]$fs.Read($b, 0, 24) } finally { $fs.Dispose() }
    [DateTime]::FromFileTimeUtc([BitConverter]::ToInt64($b, 16)).ToLocalTime()
}

$fail = $null
$replayProc = $null
try {
    Log "run folder $Run; exe $Exe; table $Table; other VPX processes (left alone): $((Get-Process -Name 'VPinballX*' -ErrorAction SilentlyContinue | ForEach-Object Id) -join ',')"
    if ($Mode -eq 'record') {
        $recDir = Join-Path $Run 'rec'
        $recSpec = "$recDir\"
        if ($RecordTo) { $recSpec = $RecordTo; $recDir = if ($RecordTo -match '\.castrec$') { Split-Path -Parent $RecordTo } else { $RecordTo.TrimEnd('\') } }
        $envs = @{ CASTCADE_STREAM = 'play'; CASTCADE_STREAM_RECORD = $recSpec; CASTCADE_TEST_NOPAUSE = '1' }
        if ($InputFile) { $envs['CASTCADE_TEST_INPUT'] = (Resolve-Path -LiteralPath $InputFile).Path; Copy-Item -LiteralPath $InputFile -Destination (Join-Path $Run 'input.txt') }
        if ($WithWatcher) { $envs['CASTCADE_STREAM_ADDR'] = "listen:127.0.0.1:$Port" }
        $P = Start-Vpx 'play' 20 $envs
        Wait-Window $P
        $A = $null
        if ($WithWatcher) { $A = Start-Vpx 'watchA' (40 + $Width) @{ CASTCADE_STREAM = 'watch'; CASTCADE_STREAM_ADDR = "connect:127.0.0.1:$Port"; CASTCADE_STREAM_DELAY_MS = "$DelayMs" } }
        $t0 = $SW.Elapsed.TotalSeconds
        $recFile = $null; $recStart = $null
        $shots = New-Object System.Collections.Generic.List[object]
        $k = 1; $listenChecked = $false
        while ($true) {
            $el = $SW.Elapsed.TotalSeconds - $t0
            if ($A) { Poll-Window $A }
            if ($P.Proc.HasExited) { throw "player exited early (code $($P.Proc.ExitCode))" }
            if (-not $recFile) {
                $f = Get-ChildItem -LiteralPath $recDir -Filter '*.castrec' -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($f) { $recStart = Recording-Start $f.FullName; if ($recStart) { $recFile = $f.FullName; Log "recording $recFile, started $($recStart.ToString('HH:mm:ss.fff'))" } }
            }
            if (-not $listenChecked -and $el -ge 10) {
                $listenChecked = $true
                $ls = @(Get-NetTCPConnection -OwningProcess $P.Pid -State Listen -ErrorAction SilentlyContinue)
                $all = @(Get-NetTCPConnection -OwningProcess $P.Pid -ErrorAction SilentlyContinue)
                $udp = @(Get-NetUDPEndpoint -OwningProcess $P.Pid -ErrorAction SilentlyContinue)
                Log ("player PID {0} at 10 s: listening TCP sockets {1} [{2}]; all TCP {3} [{4}]; UDP endpoints {5}" -f $P.Pid, $ls.Count, (($ls | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ', '), $all.Count,
                    (($all | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)->$($_.RemoteAddress):$($_.RemotePort) $($_.State)" }) -join ', '), $udp.Count)
                foreach ($m in (Get-Process -Id $P.Pid).Modules) { if ($m.ModuleName -match '(?i)dmd|pinmame|b2s|pup|serum|flex|zedmd|ws2|winhttp|wininet') { Log "  module: $($m.FileName)" } }
            }
            if ($el -ge $k * $ShotEvery -and $el -lt $Seconds) {
                $now = Get-Date
                Shot $P ("t{0:000}-play.png" -f ($k * $ShotEvery))
                if ($recStart) { $shots.Add([pscustomobject]@{ k = $k * $ShotEvery; file = ("t{0:000}-play.png" -f ($k * $ShotEvery)); rec = ($now - $recStart).TotalSeconds }) }
                if ($A -and $A.Win) { Start-Sleep -Milliseconds ($DelayMs + 20); Shot $A ("t{0:000}-watchA.png" -f ($k * $ShotEvery)) }
                $k++
            }
            if ($el -ge $Seconds) { break }
            Start-Sleep -Milliseconds 15
        }
        $now = Get-Date
        Shot $P 'final-play.png'
        if ($recStart) { $shots.Add([pscustomobject]@{ k = 'final'; file = 'final-play.png'; rec = ($now - $recStart).TotalSeconds }) }
        if ($A -and $A.Win) { Start-Sleep -Milliseconds ($DelayMs + 50); Shot $A 'final-watchA.png' }
        $shots | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'shots.json')
        Close-Vpx $P
        if ($A) { Log "keeping watcher A $AfterEndSeconds s for its last stats line"; Start-Sleep -Seconds $AfterEndSeconds; Shot $A 'after-close-watchA.png'; Close-Vpx $A }
        if ($recFile) { Log ("recording: {0} ({1:N0} bytes)" -f $recFile, (Get-Item -LiteralPath $recFile).Length) } else { Log 'NO RECORDING FILE' }
    } else {
        $B = Start-Vpx 'watchB' (40 + $Width) @{ CASTCADE_STREAM = 'watch'; CASTCADE_STREAM_ADDR = "connect:127.0.0.1:$Port"; CASTCADE_STREAM_DELAY_MS = "$DelayMs" }
        $replayLog = Join-Path $Run 'replay.log'
        $args51 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Replay, '-Recording', $Recording, '-Port', "$Port", '-Connection', $Connection, '-WatchPid', "$($B.Pid)", '-LogFile', $replayLog)
        if ($Loop) { $args51 += '-Loop' }
        $replayProc = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList (($args51 | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Run 'replay.out') -RedirectStandardError (Join-Path $Run 'replay.err')
        Log "replay (Windows PowerShell 5.1) PID $($replayProc.Id): $($args51 -join ' ')"
        $plan = @()
        if ($ShotsFrom) { $plan = @(Get-Content -LiteralPath (Join-Path $ShotsFrom 'shots.json') -Raw | ConvertFrom-Json) }
        Wait-Window $B
        $done = @{}; $passEnds = @{}; $passStarts = @{}
        $deadline = $null
        while ($true) {
            if ($B.Proc.HasExited) { throw "watcher B exited early" }
            if ($replayProc.HasExited -and -not $deadline) { throw "the replay ended early, code $($replayProc.ExitCode); see $Run\replay.out" }
            $lines = if (Test-Path -LiteralPath $replayLog) { @(Get-Content -LiteralPath $replayLog) } else { @() }
            foreach ($ln in $lines) {
                if ($ln -match '#pass (\d+) start (\S+)') { $n = [int]$Matches[1]; if (-not $passStarts.ContainsKey($n)) { $passStarts[$n] = [DateTime]::ParseExact($Matches[2], 'yyyy-MM-ddTHH:mm:ss.fff', $null); Log "pass $n started $($Matches[2])" } }
                if ($ln -match '#pass (\d+) end') { $n = [int]$Matches[1]; if (-not $passEnds.ContainsKey($n)) { $passEnds[$n] = Get-Date; Log "pass $n ended: $ln" } }
            }
            foreach ($n in @($passStarts.Keys)) {
                if ($n -gt $Passes) { continue }
                foreach ($s in $plan) {
                    $key = "$n/$($s.k)"
                    if ($done.ContainsKey($key)) { continue }
                    $at = $passStarts[$n].AddSeconds($s.rec + ($DelayMs + 20) / 1000.0)
                    if ((Get-Date) -ge $at) {
                        $done[$key] = 1
                        $late = ((Get-Date) - $at).TotalMilliseconds
                        # 'final': the player's last capture, taken before its close (a ROM table takes about a second to close)
                        $tag = if ($s.k -eq 'final') { 'atfinal' } else { 't{0:000}' -f [int]$s.k }
                        $name = if ($Passes -gt 1) { "p$n-$tag-watchB.png" } else { "$tag-watchB.png" }
                        if ($late -lt 300) { Shot $B $name } else { Log "skipped $name ($([int]$late) ms late)" }
                    }
                }
            }
            if ($passEnds.ContainsKey($Passes) -and -not $deadline) {
                Start-Sleep -Milliseconds 1500
                Shot $B $(if ($Passes -gt 1) { "p$Passes-final-watchB.png" } else { 'final-watchB.png' })
                $deadline = (Get-Date).AddSeconds($AfterEndSeconds)
            }
            if ($deadline -and (Get-Date) -ge $deadline) {
                # after the 5 s "the player closed the table" note has gone
                Shot $B $(if ($Passes -gt 1) { "p$Passes-final-late-watchB.png" } else { 'final-late-watchB.png' })
                break
            }
            Start-Sleep -Milliseconds 10
        }
        Close-Vpx $B
        if (-not $replayProc.WaitForExit(15000)) { Log 'the replay did not end after the watcher closed; stopping it'; Stop-Process -Id $replayProc.Id -Force }
        Log "replay exit code $($replayProc.ExitCode)"
    }
} catch {
    $fail = "$_"; Log "FAILED: $fail ($($_.ScriptStackTrace))"
} finally {
    foreach ($x in $Inst.Values) { if (-not $x.Proc.HasExited) { try { Close-Vpx $x } catch { Log "close error: $_" } } }
    if ($replayProc -and -not $replayProc.HasExited) { if (-not $replayProc.WaitForExit(10000)) { Stop-Process -Id $replayProc.Id -Force; Log 'stopped the replay process' } }
}

# ---------------------------------------------------------------- summary
function StatsTotals([string]$log) {
    $t = [ordered]@{ lines = 0; received = 0; starved = 0; buffering = 0; jumps = 0; failed = 0; unresolved = 0; beforeKeyframe = 0; connected = 0 }
    foreach ($ln in Get-Content -LiteralPath $log) {
        if ($ln -match 'stats [\d.]+s: (\w[\w ]*?), frames \d+, received (\d+) .* starved (\d+), buffering (\d+), jumps (\d+);.* failed (\d+), unresolved (\d+),.* frames before keyframe (\d+)') {
            $t.lines++; if ($Matches[1] -eq 'connected') { $t.connected++ }
            $t.received += [int]$Matches[2]; $t.starved += [int]$Matches[3]; $t.buffering += [int]$Matches[4]; $t.jumps += [int]$Matches[5]
            $t.failed += [int]$Matches[6]; $t.unresolved += [int]$Matches[7]; $t.beforeKeyframe += [int]$Matches[8]
        }
    }
    [pscustomobject]$t
}
Log '================ SUMMARY ================'
foreach ($x in $Inst.Values) {
    $cl = Join-Path $x.Dir 'castcade.log'
    if (-not (Test-Path -LiteralPath $cl)) { Log "$($x.Name): no castcade.log"; continue }
    Log ("{0}: PID {1}, exit {2}{3}" -f $x.Name, $x.Pid, $x.Exit, $(if ($x.Killed) { ' KILLED' } else { '' }))
    if ($x.Name -like 'watch*') { $s = StatsTotals $cl; Log ("  {0} totals: {1}" -f $x.Name, (($s.PSObject.Properties | ForEach-Object { "$($_.Name) $($_.Value)" }) -join ', ')); $s | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run "$($x.Name)-totals.json") }
    Get-Content -LiteralPath $cl | Where-Object { $_ -match 'record|keyframe|watching|connected|the player closed|connection lost|session end|ERROR|WARNING' } | Select-Object -First 30 | ForEach-Object { Log "  $_" }
}
if ($Mode -eq 'replay') { foreach ($f in 'replay.out', 'replay.err') { $p = Join-Path $Run $f; if ((Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).Length) { Log "---- $f"; Get-Content -LiteralPath $p | ForEach-Object { Log "  $_" } } } }
if ($fail) { Log "RUN FAILED: $fail"; exit 1 }
Log 'RUN OK'
exit 0
