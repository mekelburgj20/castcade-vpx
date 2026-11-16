<#
.SYNOPSIS
  Runs two VPX instances side by side (play + watch), optionally plays a game on the play one with synthetic keys,
  captures paired screenshots, closes both cleanly, and compares the pairs. See README.md.

.EXAMPLE
  pwsh -NoProfile -File pair.ps1 -Table "C:\...\Nudge Test and Calibration.vpx" -Exe C:\...\VPinballX_GL64.exe -Seconds 20
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Table,
    [string]$WatchTable,
    [string]$Exe = 'C:\code\repos\castcade-vpx\build\Release\VPinballX_GL64.exe',
    [string]$WatchExe,
    [int]$Seconds = 60,
    [ValidateSet('none', 'game')][string]$Drive = 'none',
    [int]$WatchDelaySeconds = 0,
    [switch]$WatchFirst,
    [int]$Port = 47800,
    [int]$DelayMs = 50,
    [int]$ShotEvery = 5,
    [string]$OutDir,
    [int]$Width = 960,
    [int]$Height = 540,
    [switch]$KeepOpen,
    [int]$Seed = 1,
    [int]$WindowTimeout = 120,
    [string]$Template = 'C:\code\repos\castcade-vpx-deps\run\smoke.template.ini'
)
$ErrorActionPreference = 'Stop'
if (-not $WatchTable) { $WatchTable = $Table }
if (-not $WatchExe) { $WatchExe = $Exe }
$PairRoot = 'C:\code\repos\castcade-vpx-deps\run\pair'
if (-not $OutDir) { $OutDir = Join-Path $PairRoot (Get-Date -Format 'yyyyMMdd-HHmmss') }
$Run = $OutDir
$SW = [Diagnostics.Stopwatch]::StartNew()
$LogFile = $null
function Log([string]$m) {
    $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
    Write-Host $line
    if ($script:LogFile) { try { Add-Content -LiteralPath $script:LogFile -Value $line } catch {} }
}

# ---------------------------------------------------------------- helper types (fail before launching anything)
try {
    if ([IntPtr]::Size -ne 8) { throw 'needs a 64-bit PowerShell' }
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class PairWin {
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
    public static uint PidOf(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
    public static RECT Bounds(IntPtr h) {
        RECT r; if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r); return r;
    }
    // mean abs diff (0..255 over RGB) and % of pixels whose per-pixel mean abs RGB diff exceeds 40; inputs are BGRA bytes
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
    [PairWin]::PerMonitorDpi()
    $null = [PairWin]::ForPid([uint32]$PID)
} catch { Write-Host "helper type failed to compile or load, nothing started: $_"; exit 1 }

# ---------------------------------------------------------------- preconditions
function Fail([string]$m) { Write-Host "pair.ps1: $m"; exit 1 }
foreach ($p in @($Exe, $WatchExe, $Table, $WatchTable, $Template)) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Fail "missing file: $p" } }
$Exe = (Resolve-Path -LiteralPath $Exe).Path; $WatchExe = (Resolve-Path -LiteralPath $WatchExe).Path
$Table = (Resolve-Path -LiteralPath $Table).Path; $WatchTable = (Resolve-Path -LiteralPath $WatchTable).Path
if ($Width -lt 200 -or $Height -lt 150) { Fail 'window size too small' }
foreach ($ip in @('127.0.0.1')) {   # loopback only: binding 0.0.0.0 would raise a Windows Firewall prompt for pwsh
    $l = $null
    try { $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse($ip), $Port); $l.Start(); $l.Stop() }
    catch { Fail "port $Port is not free on $ip ($($_.Exception.Message))" }
}
# processes an earlier run of this script started must be gone
$siblings = Split-Path -Parent $Run
if (Test-Path -LiteralPath $siblings) {
    Get-ChildItem -LiteralPath $siblings -Filter 'pids.json' -Recurse -Depth 1 -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            foreach ($e in @(Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json)) {
                $pr = Get-Process -Id $e.pid -ErrorAction SilentlyContinue
                if ($pr -and $pr.StartTime.ToString('o') -eq $e.start) { Fail "an earlier run still has a live process: PID $($e.pid) ($($_.FullName))" }
            }
        } catch [System.Management.Automation.RuntimeException] { throw } catch {}
    }
}
New-Item -ItemType Directory -Force $Run, "$Run\play", "$Run\watch", "$Run\shots", "$Run\regbackup" | Out-Null
$LogFile = Join-Path $Run 'pair.log'
$RegDir = Join-Path $Run 'regbackup'

# ---------------------------------------------------------------- state backup (as smoke.ps1)
$RegKeys = [ordered]@{
    'typelib' = 'HKCU\Software\Classes\TypeLib\{384DF69D-3592-4041-848D-9A2D5CD081A0}'
    'editor'  = 'HKCU\Software\Visual Pinball\VP10\Editor'
}
function Export-Key([string]$key, [string]$file) {
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    & reg.exe export $key $file /y *> $null
    return ($LASTEXITCODE -eq 0)
}
function Save-Reg { $s = @{}; foreach ($n in $RegKeys.Keys) { $s[$n] = Export-Key $RegKeys[$n] (Join-Path $RegDir "$n-before.reg") }; $s }
function Restore-Reg($existed) {
    $allOk = $true
    foreach ($n in $RegKeys.Keys) {
        $key = $RegKeys[$n]; $b = Join-Path $RegDir "$n-before.reg"; $a = Join-Path $RegDir "$n-after.reg"
        $existsNow = Export-Key $key $a
        if ($existed[$n]) {
            if ($existsNow -and ((Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash)) { Log "registry ${n}: unchanged"; continue }
            $diff = if ($existsNow) { Compare-Object (Get-Content -LiteralPath $b) (Get-Content -LiteralPath $a) | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" } } else { @('key deleted') }
            Log "registry ${n}: changed by the run, restoring:"; $diff | Select-Object -First 12 | ForEach-Object { Log "    $_" }
            if ($existsNow) { & reg.exe delete $key /f *> $null }
            & reg.exe import $b *> $null
            $again = Export-Key $key (Join-Path $RegDir "$n-restored.reg")
            $same = $again -and ((Get-FileHash -LiteralPath (Join-Path $RegDir "$n-restored.reg")).Hash -eq (Get-FileHash -LiteralPath $b).Hash)
            Log ("registry ${n}: restored, verified identical = {0}" -f $same); if (-not $same) { $allOk = $false }
        } elseif ($existsNow) {
            & reg.exe delete $key /f *> $null
            Log ("registry ${n}: created by the run, deleted (exit {0})" -f $LASTEXITCODE); if ($LASTEXITCODE -ne 0) { $allOk = $false }
        } else { Log "registry ${n}: absent before and after" }
    }
    $allOk
}
$regBefore = Save-Reg
$BamXml = Join-Path $env:APPDATA 'VPinballX\BAMViewSettings.xml'
$BamBak = Join-Path $RegDir 'BAMViewSettings.xml.before'
$bamExisted = Test-Path -LiteralPath $BamXml
if ($bamExisted) { Copy-Item -LiteralPath $BamXml -Destination $BamBak -Force; $bamTime = (Get-Item -LiteralPath $BamXml).LastWriteTimeUtc }
function Restore-Bam {
    if ($bamExisted -and (Test-Path -LiteralPath $BamXml)) {
        if ((Get-FileHash -LiteralPath $BamXml).Hash -ne (Get-FileHash -LiteralPath $BamBak).Hash -or (Get-Item -LiteralPath $BamXml).LastWriteTimeUtc -ne $bamTime) {
            Copy-Item -LiteralPath $BamBak -Destination $BamXml -Force; (Get-Item -LiteralPath $BamXml).LastWriteTimeUtc = $bamTime
            Log 'BAMViewSettings.xml: rewritten by the run, original bytes and timestamp restored'
        } else { Log 'BAMViewSettings.xml: unchanged' }
        return ((Get-FileHash -LiteralPath $BamXml).Hash -eq (Get-FileHash -LiteralPath $BamBak).Hash)
    } elseif ($bamExisted) { Copy-Item -LiteralPath $BamBak -Destination $BamXml -Force; Log 'BAMViewSettings.xml: was deleted, restored'; return $true }
    elseif (Test-Path -LiteralPath $BamXml) { Remove-Item -LiteralPath $BamXml -Force; Log 'BAMViewSettings.xml: created by the run, removed'; return $true }
    return $true
}

# ---------------------------------------------------------------- inis
$Keys = [ordered]@{ StartGameKey = 2; AddCreditKey = 6; PlungerKey = 28; LFlipKey = 42; RFlipKey = 54; ExitGameKey = 16 }
function New-Ini([string]$dest, [int]$posX) {
    $set = [ordered]@{
        'Player'   = [ordered]@{ FullScreen = '0'; Width = "$Width"; Height = "$Height"; Display = '-1'; WindowPosX = "$posX"; WindowPosY = '60';
                                 PlaySound = '0'; PlayMusic = '0'; CacheMode = '0' }
        'PlayerVR' = [ordered]@{ AskToTurnOn = '2' }
        'Editor'   = [ordered]@{ EnableLog = '1' }
    }
    foreach ($k in $Keys.Keys) { $set['Player'][$k] = "$($Keys[$k])" }
    $out = New-Object System.Collections.Generic.List[string]; $sec = ''; $done = @{}
    $flush = { param($s) if ($set.Contains($s)) { foreach ($k in $set[$s].Keys) { if (-not $done.ContainsKey("$s/$k")) { $out.Add("$k = $($set[$s][$k])"); $done["$s/$k"] = 1 } } } }
    foreach ($l in [IO.File]::ReadAllLines($Template)) {
        if ($l -match '^\s*\[(.+?)\]\s*$') { & $flush $sec; $sec = $Matches[1]; $out.Add($l); continue }
        if ($set.Contains($sec) -and $l -match '^\s*([^=;#]+?)\s*=') {
            $hit = $set[$sec].Keys | Where-Object { $_ -ieq $Matches[1] } | Select-Object -First 1
            if ($hit) { if (-not $done.ContainsKey("$sec/$hit")) { $out.Add("$($Matches[1]) = $($set[$sec][$hit])"); $done["$sec/$hit"] = 1 }; continue }
        }
        $out.Add($l)
    }
    & $flush $sec
    [IO.File]::WriteAllLines($dest, $out, (New-Object System.Text.UTF8Encoding($false)))
}
New-Ini (Join-Path $Run 'play.ini') 20
New-Ini (Join-Path $Run 'watch.ini') (20 + $Width + 20)

# ---------------------------------------------------------------- instances
$I = @{
    play  = @{ Name = 'play'; Exe = $Exe; Table = $Table; Ini = (Join-Path $Run 'play.ini'); Dir = (Join-Path $Run 'play'); Proc = $null; Pid = 0; Win = $null; Launched = $null; Exit = $null; Killed = $false; Seen = @{}; Notes = @() }
    watch = @{ Name = 'watch'; Exe = $WatchExe; Table = $WatchTable; Ini = (Join-Path $Run 'watch.ini'); Dir = (Join-Path $Run 'watch'); Proc = $null; Pid = 0; Win = $null; Launched = $null; Exit = $null; Killed = $false; Seen = @{}; Notes = @() }
}
$pidRecords = New-Object System.Collections.Generic.List[object]
function Start-Instance($inst) {
    $psi = [Diagnostics.ProcessStartInfo]::new($inst.Exe)
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $inst.Dir
    foreach ($a in @('-Ini', $inst.Ini, '/ExtMinimized', '-Play', $inst.Table)) { $psi.ArgumentList.Add($a) }
    foreach ($k in @($psi.Environment.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { [void]$psi.Environment.Remove($k) }
    $log = Join-Path $inst.Dir 'castcade.log'
    if ($inst.Name -eq 'play') {
        $psi.Environment['CASTCADE_STREAM'] = 'play'
        $psi.Environment['CASTCADE_STREAM_ADDR'] = "listen:127.0.0.1:$Port"
        if ($Drive -eq 'game') { $psi.Environment['CASTCADE_TEST_INPUT'] = $InputFile }
    } else {
        $psi.Environment['CASTCADE_STREAM'] = 'watch'
        $psi.Environment['CASTCADE_STREAM_ADDR'] = "connect:127.0.0.1:$Port"
        $psi.Environment['CASTCADE_STREAM_DELAY_MS'] = "$DelayMs"
    }
    $psi.Environment['CASTCADE_TEST_NOPAUSE'] = '1'
    $psi.Environment['CASTCADE_STREAM_LOG'] = $log
    Log ("start {0}: `"{1}`" {2}  [cwd {3}]" -f $inst.Name, $inst.Exe, (($psi.ArgumentList | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }) -join ' '), $inst.Dir)
    $p = [Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $inst.Proc = $p; $inst.Pid = [uint32]$p.Id; $inst.Launched = $SW.Elapsed.TotalSeconds
    $pidRecords.Add([pscustomobject]@{ name = $inst.Name; pid = $p.Id; start = $p.StartTime.ToString('o') })
    ($pidRecords.ToArray() | ConvertTo-Json -AsArray) | Set-Content -LiteralPath (Join-Path $Run 'pids.json')
    Log "$($inst.Name) PID $($p.Id)"
}
function Poll-Window($inst) {
    if ($inst.Win -or -not $inst.Proc) { return }
    foreach ($w in [PairWin]::ForPid($inst.Pid)) {
        $sig = "$($w.Cls)|$($w.Title)|$($w.Visible)"
        if ($w.Visible -and -not $inst.Seen.ContainsKey($sig)) { $inst.Seen[$sig] = 1; Log ("{0} t+{1:N1}s window: {2}" -f $inst.Name, ($SW.Elapsed.TotalSeconds - $inst.Launched), $w) }
        if ($w.Visible -and $w.Title -eq 'Visual Pinball Player') { $inst.Win = $w.H; $inst.WinAt = $SW.Elapsed.TotalSeconds }
    }
}
function Save-Capture([IntPtr]$h, [string]$path) {
    $r = [PairWin]::Bounds($h); $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $ht, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $hdc = $g.GetHdc(); [void][PairWin]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc) } finally { $g.Dispose() }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); $true
}

# ---------------------------------------------------------------- input (every key event is gated on foreground)
# ---------------------------------------------------------------- test input script (fired inside VPX via CASTCADE_TEST_INPUT)
# Lines: "<ms> <down|up> <key>", ms from the play instance's first rendered frame. The harness sends no input to any window.
$InputFile = Join-Path $Run 'play\input.txt'
$InputCount = 0
if ($Drive -eq 'game') {
    $ev = New-Object System.Collections.Generic.List[object]
    $end = ($Seconds + 10) * 1000
    $tap = { param([int]$t, [string]$key, [int]$hold)
        if ($t + $hold -le $end) { $ev.Add([pscustomobject]@{ t = $t; o = 1; line = "$t down $key" }); $ev.Add([pscustomobject]@{ t = $t + $hold; o = 0; line = "$($t + $hold) up $key" }) } }
    $rng = [Random]::new($Seed)
    & $tap 8000 'credit' 150; & $tap 8600 'credit' 150; & $tap 9200 'start' 150
    & $tap 12350 'plunger' 1200
    $t = 14000; $alt = 0; $nextPlunge = 24350; $nextStart = 54200
    while ($t -lt $end) {
        $hold = 120 + $rng.Next(0, 181)
        if ($rng.Next(0, 5) -eq 0) { & $tap $t 'lflip' $hold; & $tap $t 'rflip' $hold }
        else { & $tap $t $(if ($alt % 2 -eq 0) { 'lflip' } else { 'rflip' }) $hold; $alt++ }
        $t += 700 + $rng.Next(0, 901)
        while ($nextPlunge -le $t) { & $tap $nextPlunge 'plunger' 1200; $nextPlunge += 12000 }
        while ($nextStart -le $t) { & $tap $nextStart 'start' 150; $nextStart += 45000 }
    }
    $sorted = @($ev | Sort-Object t, o)   # at the same instant: up before down
    $lines = @("# generated by pair.ps1 (seed $Seed): <ms from first rendered frame> <down|up> <key>") + @($sorted | ForEach-Object { $_.line })
    [IO.File]::WriteAllLines($InputFile, $lines, (New-Object System.Text.UTF8Encoding($false)))
    $InputCount = $sorted.Count
    Log "wrote $InputCount events to $InputFile (seed $Seed, $end ms)"
}

# ---------------------------------------------------------------- run
$fail = $null; $tBoth = $null; $cleanOk = $true; $regOk = $true; $bamOk = $true
$shotEv = New-Object System.Collections.Generic.List[object]
$pairs = New-Object System.Collections.Generic.List[object]
try {
    Log ("run folder {0}; play exe {1}; watch exe {2}" -f $Run, $Exe, $WatchExe)
    Log ("other VPX processes (left alone): {0}" -f ((Get-Process -Name 'VPinballX*' -ErrorAction SilentlyContinue | ForEach-Object Id) -join ','))
    if ($WatchFirst) { Start-Instance $I.watch; $t0w = $SW.Elapsed.TotalSeconds }
    else { Start-Instance $I.play }
    # unified run loop (also handles a watcher that is still launching/loading while the play instance is already driven)
    $tPlay = if ($I.play.Win) { $I.play.WinAt } else { $null }
    $di = 0; $si = 0; $nextShot = $null
    $ended = $false
    while (-not $ended) {
        $now = $SW.Elapsed.TotalSeconds
        # keep launching / polling
        if ($WatchFirst) { if (-not $I.play.Proc -and ($now - $t0w) -ge 6) { Start-Instance $I.play } }
        elseif (-not $I.watch.Proc -and $I.play.Win -and ($now - $I.play.WinAt) -ge $WatchDelaySeconds) { Start-Instance $I.watch }
        foreach ($n in 'play', 'watch') {
            $x = $I[$n]; if (-not $x.Proc) { continue }
            Poll-Window $x
            if ($x.Proc.HasExited) { throw "$n instance (PID $($x.Pid)) exited unexpectedly (exit code $($x.Proc.ExitCode)); see $($x.Dir)\vpinball.log" }
            if (-not $x.Win -and ($now - $x.Launched) -gt $WindowTimeout) { throw "no player window for $n instance after $WindowTimeout s" }
        }
        if ($I.play.Win -and -not $tPlay) { $tPlay = $I.play.WinAt }
        if ($I.play.Win -and $I.watch.Win -and -not $tBoth) {
            $tBoth = $now; Log "both player windows exist; running $Seconds s"
            foreach ($n in 'play', 'watch') { $r = [PairWin]::Bounds($I[$n].Win); Log ("{0} window rect {1},{2}-{3},{4}" -f $n, $r.L, $r.T, $r.R, $r.B) }
            $k = 1
            while ($k * $ShotEvery -le $Seconds) {
                $shotEv.Add([pscustomobject]@{ t = $k * $ShotEvery; who = 'play'; k = $k * $ShotEvery })
                $shotEv.Add([pscustomobject]@{ t = $k * $ShotEvery + ($DelayMs + 20) / 1000.0; who = 'watch'; k = $k * $ShotEvery })
                $k++
            }
            $shotEv = [System.Collections.Generic.List[object]]@($shotEv | Sort-Object t)
        }
        # screenshots (PrintWindow: no focus change)
        if ($tBoth) {
            while ($si -lt $shotEv.Count -and ($now - $tBoth) -ge $shotEv[$si].t) {
                $e = $shotEv[$si]; $si++
                $f = Join-Path $Run ("shots\t{0:000}-{1}.png" -f $e.k, $e.who)
                try { if (-not (Save-Capture $I[$e.who].Win $f)) { Log "capture ${f}: empty window" } } catch { Log "capture ${f} failed: $_" }
            }
            if ($si -ge $shotEv.Count -and ($now - $tBoth) -ge $Seconds) { $ended = $true }
        }
        Start-Sleep -Milliseconds 15
    }
} catch {
    $fail = "$_"; Log ("at: " + $_.ScriptStackTrace + " :: " + $_.Exception.GetType().FullName)
    Log "FAILED: $fail"
} finally {
    # ------------------------------------------------------------ cleanup (always)
    try {
        if ($KeepOpen -and -not $fail) {
            Log ("-KeepOpen: leaving instances running. play PID {0}, watch PID {1}" -f $I.play.Pid, $I.watch.Pid)
        } else {
            foreach ($n in 'play', 'watch') {
                $x = $I[$n]; if (-not $x.Proc) { continue }
                if (-not $x.Proc.HasExited) {
                    $wins = [PairWin]::ForPid($x.Pid)
                    $pl = $wins | Where-Object { $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
                    if ($pl) { Log "WM_CLOSE -> $n player window"; [void][PairWin]::PostMessage($pl.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
                    else { Log "$n has no player window; closing its other windows" ; $wins | Where-Object Visible | ForEach-Object { Log "  $_"; [void][PairWin]::PostMessage($_.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }
                }
            }
            foreach ($n in 'play', 'watch') {
                $x = $I[$n]; if (-not $x.Proc) { continue }
                if (-not $x.Proc.WaitForExit(20000)) {
                    $frame = [PairWin]::ForPid($x.Pid) | Where-Object { $_.Cls -eq 'VPinball' } | Select-Object -First 1
                    if ($frame) { Log "$n still running after 20 s; WM_CLOSE -> its VPinball frame"; [void][PairWin]::PostMessage($frame.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); [void]$x.Proc.WaitForExit(5000) }
                }
                if (-not $x.Proc.HasExited) {
                    $cleanOk = $false; $x.Killed = $true
                    Log ("$n PID {0} did not exit within 20 s; visible windows: {1}" -f $x.Pid, (([PairWin]::ForPid($x.Pid) | Where-Object Visible | ForEach-Object { $_.ToString() }) -join ' ; '))
                    Log "last resort: killing my own PID $($x.Pid)"
                    try { Stop-Process -Id $x.Pid -Force; [void]$x.Proc.WaitForExit(10000) } catch { Log "kill failed: $_" }
                }
                $x.Exit = $x.Proc.ExitCode
                Log ("{0} PID {1} exited, code {2}{3}" -f $n, $x.Pid, $x.Exit, $(if ($x.Killed) { ' (killed by me)' } else { '' }))
            }
        }
    } catch { $cleanOk = $false; Log "cleanup error: $_" }
    try { $regOk = Restore-Reg $regBefore } catch { $regOk = $false; Log "registry restore error: $_" }
    try { $bamOk = Restore-Bam } catch { $bamOk = $false; Log "BAM restore error: $_" }
}

# ---------------------------------------------------------------- compare and summarise
function Get-Pixels([string]$path) {
    $src = [System.Drawing.Image]::FromFile($path)
    try {
        $bmp = New-Object System.Drawing.Bitmap 192, 108, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
        $g.DrawImage($src, 0, 0, 192, 108); $g.Dispose()
        $bd = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, 192, 108), 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $bytes = New-Object byte[] ($bd.Stride * 108); [Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bytes, 0, $bytes.Length)
        $bmp.UnlockBits($bd); $bmp.Dispose(); , $bytes
    } finally { $src.Dispose() }
}
$rows = @()
try {
    foreach ($pf in Get-ChildItem -LiteralPath "$Run\shots" -Filter 't*-play.png' | Sort-Object Name) {
        $wf = Join-Path $Run ($pf.Name -replace '-play\.png$', '-watch.png').Insert(0, 'shots\')
        if (-not (Test-Path -LiteralPath $wf)) { continue }
        $d = [PairWin]::Diff((Get-Pixels $pf.FullName), (Get-Pixels $wf))
        $rows += [pscustomobject]@{ time = [int]($pf.Name.Substring(1, 3)); mean_abs_diff = [math]::Round($d[0], 2); pct_over_40 = [math]::Round($d[1], 2); play = $pf.FullName; watch = $wf }
    }
    $rows | Select-Object time, mean_abs_diff, pct_over_40 | Export-Csv -LiteralPath (Join-Path $Run 'compare.csv') -NoTypeInformation
    if ($rows.Count -gt 0) {
        $sel = if ($rows.Count -le 8) { @(0..($rows.Count - 1)) } else { @(0..7 | ForEach-Object { [int][math]::Round($_ * ($rows.Count - 1) / 7.0) } | Select-Object -Unique) }
        $imgs = @(); $rowH = @()
        foreach ($ix in $sel) {
            $a = [System.Drawing.Image]::FromFile($rows[$ix].play); $b = [System.Drawing.Image]::FromFile($rows[$ix].watch)
            $h = [int][math]::Round(640.0 * $a.Height / $a.Width); $imgs += , @($a, $b); $rowH += $h
        }
        $sheet = New-Object System.Drawing.Bitmap 1280, ($rowH | Measure-Object -Sum).Sum, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::Black); $g.InterpolationMode = 'HighQualityBicubic'
        $font = New-Object System.Drawing.Font 'Consolas', 14, ([System.Drawing.FontStyle]::Bold)
        $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(190, 0, 0, 0)); $fg = [System.Drawing.Brushes]::Yellow
        $y = 0
        for ($r = 0; $r -lt $sel.Count; $r++) {
            $g.DrawImage($imgs[$r][0], 0, $y, 640, $rowH[$r]); $g.DrawImage($imgs[$r][1], 640, $y, 640, $rowH[$r])
            $row = $rows[$sel[$r]]
            $txt = ("t={0}s  mean|diff|={1}  >40: {2}%   [left PLAY | right WATCH]" -f $row.time, $row.mean_abs_diff, $row.pct_over_40)
            $g.FillRectangle($bg, 0, $y, 640, 24); $g.DrawString($txt, $font, $fg, 4, $y + 1)
            $imgs[$r][0].Dispose(); $imgs[$r][1].Dispose(); $y += $rowH[$r]
        }
        $g.Dispose(); $sheet.Save((Join-Path $Run 'sheet.png'), [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
    }
} catch { Log "compare/sheet failed: $_" }

Write-Host ''
Log '================ SUMMARY ================'
Log "run folder: $Run"
foreach ($n in 'play', 'watch') {
    $x = $I[$n]
    Log ("{0}: PID {1}, window {2}, exit code {3}{4}" -f $n, $x.Pid, $(if ($x.Win) { 'yes' } else { 'NO' }), $(if ($null -ne $x.Exit) { $x.Exit } else { 'n/a' }), $(if ($x.Killed) { ' (KILLED)' } else { '' }))
    $others = @($x.Seen.Keys | Where-Object { $_ -notmatch '^VPinball\|' -and $_ -notmatch '\|Visual Pinball Player\|' })
    if ($others) { Log "  other visible windows seen for ${n}: $($others -join ' ; ')" }
}
if ($Drive -eq 'game') {
    $played = 'castcade.log (play) not written'
    $pl = Join-Path $I.play.Dir 'castcade.log'
    if (Test-Path -LiteralPath $pl) { $played = "{0} 'test input' lines in play\castcade.log" -f @(Get-Content -LiteralPath $pl | Where-Object { $_ -match 'test input' }).Count }
    Log "input: $InputCount events written to $InputFile; $played"
} else { Log 'input: none (-Drive none)' }
if ($rows.Count) { Log 'compare (play vs watch, 192x108):'; $rows | Select-Object time, mean_abs_diff, pct_over_40 | Format-Table -AutoSize | Out-String -Width 120 | ForEach-Object { $_.TrimEnd() } | Write-Host }
else { Log 'compare: no complete screenshot pairs' }
foreach ($n in 'play', 'watch') {
    $cl = Join-Path $I[$n].Dir 'castcade.log'
    if (Test-Path -LiteralPath $cl) {
        $lines = @(Get-Content -LiteralPath $cl)
        Log "castcade.log ($n), last 5 of $($lines.Count) lines:"; $lines | Select-Object -Last 5 | ForEach-Object { Log "    $_" }
        $bad = @($lines | Where-Object { $_ -match '(?i)error|mismatch|refus' })
        if ($bad) { Log "  error/mismatch/refus lines ($n):"; $bad | Select-Object -First 20 | ForEach-Object { Log "    $_" } }
    } else { Log "castcade.log ($n): not written" }
}
if (Test-Path -LiteralPath (Join-Path $Run 'sheet.png')) { Log "sheet: $(Join-Path $Run 'sheet.png')" }
$bothStarted = $I.play.Win -and $I.watch.Win
if ($KeepOpen -and -not $fail) {
    $ok = $bothStarted -and $regOk -and $bamOk
    Log ("KEEP OPEN: play PID {0}, watch PID {1} (close them yourself)" -f $I.play.Pid, $I.watch.Pid)
} else {
    $ok = $bothStarted -and ($null -ne $I.play.Exit) -and ($null -ne $I.watch.Exit) -and -not $I.play.Killed -and -not $I.watch.Killed -and $cleanOk -and $regOk -and $bamOk
}
if ($fail) { $ok = $false; Log "failure: $fail" }
Log ("cleanup: instances clean={0}, registry restored={1}, BAMViewSettings restored={2}" -f $cleanOk, $regOk, $bamOk)
if ($ok) { Log 'PAIR OK'; exit 0 } else { Log 'PAIR FAILED'; exit 1 }
