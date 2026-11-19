<#
.SYNOPSIS
  Turn hand-off test for CASTCADE_STREAM=match: VPX instances in the match role on one PC, with this script playing
  the companion's part on their control connections. See README.md.

.EXAMPLE
  pwsh -NoProfile -File match-test.ps1 -Table "C:\...\Attack from Mars.vpx" -Rom afm_113b -Swaps 50 -Check
  pwsh -NoProfile -File match-test.ps1 -Table "C:\...\table.vpx" -Scenario nolistener
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Table,
    # swap: A and B take turns watching each other, -Swaps times.   nolistener: A is told to watch a port where nothing listens.
    # controllost: B watches A and its control connection is cut.   mismatch: B (-OtherTable) is told to watch A.
    # reaim: C watches A, is re-aimed at B, then called back.        plainwatch: a plain watch build W watches A while A and B swap.
    # contract: what the companion's control client counts on (order of the first lines, the tokens of hello, every
    #   watch answered within 5 s, play while playing, play and watch in one go, the same watch twice, an unknown command,
    #   the watched player closing its table).                      streamlost: B watches A and A is killed (this script's own PID).
    [ValidateSet('swap', 'visit', 'nolistener', 'controllost', 'mismatch', 'reaim', 'plainwatch', 'contract', 'streamlost', 'stalled')][string]$Scenario = 'swap',
    [string]$OtherTable,
    [string]$Exe = 'C:\code\repos\castcade-vpx-wt-handoff\build\Release\VPinballX_GL64.exe',
    # The instances run from a copy of the exe under this name, beside it: the installed Castcade companion takes any
    # running VPinballX*.exe that loads a table for a game of its own
    [string]$RunAs = 'ccmatch_GL64.exe',
    [int]$Swaps = 50,
    [double]$WatchSeconds = 2.5,
    [double]$PlaySeconds = 2.5,
    [int]$WarmupSeconds = 28,
    [ValidateSet('none', 'game')][string]$Drive = 'game',
    [int]$Seed = 1,
    [int]$BasePort = 48100,          # control ports BasePort+1.., stream ports BasePort+11..; all on 127.0.0.1
    [int]$DelayMs = 50,
    # VPinMAME rom names whose nvram\<rom>.nv and cfg\<rom>.cfg are saved before the run and put back after it
    [string[]]$Rom = @(),
    [switch]$Check,                  # CASTCADE_TEST_MATCHCHECK=1: every property compared after each watch phase
    [switch]$StaticRefresh,          # CASTCADE_STREAM_STATICREFRESH=1
    [switch]$NoFrameLog,
    [hashtable]$ExtraEnv = @{},
    [double]$MaxPictureDiff = 1.5,   # mean abs difference (0..255, 192x108) allowed between the picture before a watch and after it
    [double]$MaxSwitchFrameMs = 100,
    [string]$Label,
    [string]$OutDir,
    [int]$Width = 960,
    [int]$Height = 540,
    [int]$WindowTimeout = 180,
    [string]$Template = 'C:\code\repos\castcade-vpx-deps\run\smoke.template.ini'
)
$ErrorActionPreference = 'Stop'
$Root = 'C:\code\repos\castcade-vpx-deps\run\match'
if (-not $Label) { $Label = $Scenario }
if (-not $OutDir) { $OutDir = Join-Path $Root ("{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Label) }
$Run = $OutDir
$SW = [Diagnostics.Stopwatch]::StartNew()
$LogFile = $null
function Log([string]$m) {
    $line = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
    Write-Host $line
    if ($script:LogFile) { try { Add-Content -LiteralPath $script:LogFile -Value $line } catch {} }
}
function Fail([string]$m) { Write-Host "match-test.ps1: $m"; exit 1 }

# ---------------------------------------------------------------- helper types (fail before launching anything)
try {
    if ([IntPtr]::Size -ne 8) { throw 'needs a 64-bit PowerShell' }
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class MatchWin {
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
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr process);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr process);
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
    // mean abs diff (0..255 over RGB) and % of pixels whose per-pixel mean abs RGB diff exceeds 40; inputs are BGRA bytes
    public static double[] Diff(byte[] a, byte[] b) {
        int n = Math.Min(a.Length, b.Length) / 4; double sum = 0; int over = 0;
        for (int p = 0; p < n; p++) {
            int o = p * 4;
            int d = Math.Abs(a[o] - b[o]) + Math.Abs(a[o + 1] - b[o + 1]) + Math.Abs(a[o + 2] - b[o + 2]);
            sum += d / 3.0; if (d / 3.0 > 40) over++;
        }
        return new double[] { sum / n, 100.0 * over / n };
    }
}
'@
    [MatchWin]::PerMonitorDpi()
    $null = [MatchWin]::ForPid([uint32]$PID)
} catch { Write-Host "helper type failed to compile or load, nothing started: $_"; exit 1 }

# ---------------------------------------------------------------- preconditions
if (-not $OtherTable) { $OtherTable = $Table }
foreach ($p in @($Exe, $Table, $OtherTable, $Template)) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Fail "missing file: $p" } }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$Table = (Resolve-Path -LiteralPath $Table).Path; $OtherTable = (Resolve-Path -LiteralPath $OtherTable).Path
if ($BasePort -lt 48100 -or $BasePort + 20 -gt 48199) { Fail 'ports must stay within 48100-48199' }
if ($RunAs -match '^VPinballX') { Fail "-RunAs must not start with VPinballX (the installed Castcade would take the instances for games)" }
$RunExe = Join-Path (Split-Path -Parent $Exe) $RunAs
if (-not (Test-Path -LiteralPath $RunExe) -or (Get-FileHash -LiteralPath $RunExe).Hash -ne (Get-FileHash -LiteralPath $Exe).Hash) {
    Copy-Item -LiteralPath $Exe -Destination $RunExe -Force
}

# instances of this scenario: name, role, table
$Names = @(switch ($Scenario) {
    'swap' { 'a', 'b' } 'visit' { 'a', 'b' } 'stalled' { 'a', 'b' } 'nolistener' { 'a' } 'controllost' { 'a', 'b' } 'mismatch' { 'a', 'b' } 'reaim' { 'a', 'b', 'c' } 'plainwatch' { 'a', 'b', 'w' }
    'contract' { 'a', 'b' } 'streamlost' { 'a', 'b' }
})
$I = [ordered]@{}
$ix = 0
foreach ($n in $Names) {
    $ix++
    $I[$n] = @{ Name = $n; Role = $(if ($n -eq 'w') { 'watch' } else { 'match' }); Table = $(if ($Scenario -eq 'mismatch' -and $n -eq 'b') { $OtherTable } else { $Table });
        Ctl = $BasePort + $ix; Stream = $BasePort + 10 + $ix; PosX = 20 + (($ix - 1) % 2) * ($Width + 20); PosY = 60 + [int][math]::Floor(($ix - 1) / 2) * ($Height + 50);
        Dir = (Join-Path $Run $n); Ini = (Join-Path $Run "$n.ini");
        Proc = $null; Pid = 0; Win = $null; WinAt = $null; Launched = $null; Exit = $null; Killed = $false; Seen = @{};
        Listener = $null; Client = $null; Stream0 = $null; Buf = ''; Lines = (New-Object System.Collections.Generic.List[object]); Hello = $null; State = $null }
}
$DeadPort = $BasePort + 19   # nothing ever listens here
foreach ($n in $Names) {
    foreach ($lp in @($I[$n].Ctl, $I[$n].Stream) + $(if ($Scenario -eq 'nolistener') { $DeadPort } else { @() })) {
        $l = $null
        try { $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $lp); $l.Start(); $l.Stop() }
        catch { Fail "port $lp is not free on 127.0.0.1 ($($_.Exception.Message))" }
    }
}
if (Test-Path -LiteralPath $Root) {
    Get-ChildItem -LiteralPath $Root -Filter 'pids.json' -Recurse -Depth 1 -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            foreach ($e in @(Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json)) {
                $pr = Get-Process -Id $e.pid -ErrorAction SilentlyContinue
                # (a PID of an old run can belong to another process by now, even one whose start time cannot be read)
                if ($pr -and $pr.StartTime -and $pr.StartTime.ToString('o') -eq $e.start) { Fail "an earlier run still has a live process: PID $($e.pid) ($($_.FullName))" }
            }
        } catch [System.Management.Automation.RuntimeException] { throw } catch {}
    }
}
New-Item -ItemType Directory -Force (@($Run, "$Run\shots", "$Run\regbackup", "$Run\vpm") + @($Names | ForEach-Object { $I[$_].Dir })) | Out-Null
$LogFile = Join-Path $Run 'match.log'
$RegDir = Join-Path $Run 'regbackup'

# ---------------------------------------------------------------- state backup (as pair.ps1), plus VPinMAME's files of -Rom
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
            Log "registry ${n}: changed by the run, restoring"
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
# VPinMAME writes nvram\<rom>.nv and cfg\<rom>.cfg when a ROM table closes: saved here, put back after the run
$VpmFiles = @()
if ($Rom.Count) {
    $g = Get-ItemProperty 'HKCU:\Software\Freeware\Visual PinMame\globals' -ErrorAction SilentlyContinue
    if (-not $g -or -not $g.nvram_directory) { Fail 'VPinMAME''s nvram folder is not in the registry; cannot save the files of -Rom' }
    foreach ($r in $Rom) {
        foreach ($f in @((Join-Path $g.nvram_directory "$r.nv"), (Join-Path $g.cfg_directory "$r.cfg"))) {
            $e = @{ Path = $f; Existed = (Test-Path -LiteralPath $f); Bak = (Join-Path "$Run\vpm" ((Split-Path -Leaf (Split-Path -Parent $f)) + '-' + (Split-Path -Leaf $f))) }
            if ($e.Existed) { Copy-Item -LiteralPath $f -Destination $e.Bak -Force; $e.Time = (Get-Item -LiteralPath $f).LastWriteTimeUtc; $e.Hash = (Get-FileHash -LiteralPath $f).Hash }
            $VpmFiles += , $e
        }
    }
}
function Restore-Vpm {
    $ok = $true
    foreach ($e in $VpmFiles) {
        try {
            if ($e.Existed) {
                if (-not (Test-Path -LiteralPath $e.Path) -or (Get-FileHash -LiteralPath $e.Path).Hash -ne $e.Hash -or (Get-Item -LiteralPath $e.Path).LastWriteTimeUtc -ne $e.Time) {
                    Copy-Item -LiteralPath $e.Bak -Destination $e.Path -Force; (Get-Item -LiteralPath $e.Path).LastWriteTimeUtc = $e.Time
                    Log "VPinMAME $($e.Path): rewritten by the run, original bytes and timestamp restored"
                } else { Log "VPinMAME $($e.Path): unchanged" }
                if ((Get-FileHash -LiteralPath $e.Path).Hash -ne $e.Hash) { $ok = $false }
            } elseif (Test-Path -LiteralPath $e.Path) { Remove-Item -LiteralPath $e.Path -Force; Log "VPinMAME $($e.Path): created by the run, removed" }
        } catch { $ok = $false; Log "VPinMAME $($e.Path): restore failed: $_" }
    }
    $ok
}

# ---------------------------------------------------------------- inis
$Keys = [ordered]@{ StartGameKey = 2; AddCreditKey = 6; PlungerKey = 28; LFlipKey = 42; RFlipKey = 54; ExitGameKey = 16 }
function New-Ini([string]$dest, [int]$posX, [int]$posY) {
    $set = [ordered]@{
        'Player'   = [ordered]@{ FullScreen = '0'; Width = "$Width"; Height = "$Height"; Display = '-1'; WindowPosX = "$posX"; WindowPosY = "$posY";
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
foreach ($n in $Names) { New-Ini $I[$n].Ini $I[$n].PosX $I[$n].PosY }

# ---------------------------------------------------------------- test input (fired inside VPX via CASTCADE_TEST_INPUT)
# The schedule of pair.ps1 (with more presses of start and of the plunger at the beginning: a ROM that is still
# booting at 9 s ignores the first), one seed per instance, long enough for the whole run. Keys that fall into a watch
# phase never reach the script (that is the match role's rule), so each game is played in the gaps.
$TotalSeconds = $WarmupSeconds + [int]([math]::Ceiling($Swaps * ($WatchSeconds + $PlaySeconds + 3.5))) + 60
function Write-Input([string]$file, [int]$seed) {
    $ev = New-Object System.Collections.Generic.List[object]
    $end = $TotalSeconds * 1000
    $tap = { param([int]$t, [string]$key, [int]$hold)
        if ($t + $hold -le $end) { $ev.Add([pscustomobject]@{ t = $t; o = 1; line = "$t down $key" }); $ev.Add([pscustomobject]@{ t = $t + $hold; o = 0; line = "$($t + $hold) up $key" }) } }
    $rng = [Random]::new($seed)
    & $tap 8000 'credit' 150; & $tap 8600 'credit' 150; & $tap 9200 'start' 150
    & $tap 12350 'plunger' 1200
    & $tap 15200 'start' 150; & $tap 18350 'plunger' 1200; & $tap 21200 'start' 150
    $t = 14000; $alt = 0; $nextPlunge = 24350; $nextStart = 54200
    while ($t -lt $end) {
        $hold = 120 + $rng.Next(0, 181)
        if ($rng.Next(0, 5) -eq 0) { & $tap $t 'lflip' $hold; & $tap $t 'rflip' $hold }
        else { & $tap $t $(if ($alt % 2 -eq 0) { 'lflip' } else { 'rflip' }) $hold; $alt++ }
        $t += 700 + $rng.Next(0, 901)
        while ($nextPlunge -le $t) { & $tap $nextPlunge 'plunger' 1200; $nextPlunge += 12000 }
        while ($nextStart -le $t) { & $tap $nextStart 'start' 150; $nextStart += 45000 }
    }
    $sorted = @($ev | Sort-Object t, o)
    [IO.File]::WriteAllLines($file, @("# generated by match-test.ps1 (seed $seed): <ms from first rendered frame> <down|up> <key>") + @($sorted | ForEach-Object { $_.line }), (New-Object System.Text.UTF8Encoding($false)))
    $sorted.Count
}

# ---------------------------------------------------------------- instances and their control connections
$pidRecords = New-Object System.Collections.Generic.List[object]
function Start-Instance($inst) {
    $psi = [Diagnostics.ProcessStartInfo]::new($RunExe)
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $inst.Dir
    foreach ($a in @('-Ini', $inst.Ini, '/ExtMinimized', '-Play', $inst.Table)) { $psi.ArgumentList.Add($a) }
    foreach ($k in @($psi.Environment.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { [void]$psi.Environment.Remove($k) }
    if ($inst.Role -eq 'match') {
        $psi.Environment['CASTCADE_STREAM'] = 'match'
        $psi.Environment['CASTCADE_STREAM_ADDR'] = "listen:127.0.0.1:$($inst.Stream)"
        $psi.Environment['CASTCADE_STREAM_CONTROL'] = "connect:127.0.0.1:$($inst.Ctl)"
        $psi.Environment['CASTCADE_STREAM_SOUND'] = '0'
        $psi.Environment['CASTCADE_TEST_BALLLOG'] = '250'
        if ($Check) { $psi.Environment['CASTCADE_TEST_MATCHCHECK'] = '1' }
        if ($StaticRefresh) { $psi.Environment['CASTCADE_STREAM_STATICREFRESH'] = '1' }
        if ($Drive -eq 'game') {
            $n = Write-Input (Join-Path $inst.Dir 'input.txt') ($Seed + [array]::IndexOf($Names, $inst.Name))
            $psi.Environment['CASTCADE_TEST_INPUT'] = (Join-Path $inst.Dir 'input.txt')
            Log "$($inst.Name): $n key events in input.txt"
        }
    } else {
        $psi.Environment['CASTCADE_STREAM'] = 'watch'
        $psi.Environment['CASTCADE_STREAM_ADDR'] = "connect:127.0.0.1:$($I['a'].Stream)"
    }
    $psi.Environment['CASTCADE_STREAM_DELAY_MS'] = "$DelayMs"
    $psi.Environment['CASTCADE_TEST_NOPAUSE'] = '1'
    $psi.Environment['CASTCADE_STREAM_LOG'] = (Join-Path $inst.Dir 'castcade.log')
    if (-not $NoFrameLog) { $psi.Environment['CASTCADE_FRAMELOG'] = (Join-Path $inst.Dir 'frames.log') }
    foreach ($k in $ExtraEnv.Keys) { $psi.Environment[$k] = "$($ExtraEnv[$k])" }
    Log ("start {0} ({1}): `"{2}`" -Play `"{3}`"" -f $inst.Name, $inst.Role, $RunExe, $inst.Table)
    $p = [Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $inst.Proc = $p; $inst.Pid = [uint32]$p.Id; $inst.Launched = $SW.Elapsed.TotalSeconds
    $pidRecords.Add([pscustomobject]@{ name = $inst.Name; pid = $p.Id; start = $p.StartTime.ToString('o') })
    ($pidRecords.ToArray() | ConvertTo-Json -AsArray) | Set-Content -LiteralPath (Join-Path $Run 'pids.json')
    Log "$($inst.Name) PID $($p.Id), control port $($inst.Ctl), stream port $($inst.Stream)"
}
function Poll-Window($inst) {
    if ($inst.Win -or -not $inst.Proc) { return }
    foreach ($w in [MatchWin]::ForPid($inst.Pid)) {
        $sig = "$($w.Cls)|$($w.Title)|$($w.Visible)"
        if ($w.Visible -and -not $inst.Seen.ContainsKey($sig)) { $inst.Seen[$sig] = 1 }
        if ($w.Visible -and $w.Title -eq 'Visual Pinball Player') { $inst.Win = $w.H; $inst.WinAt = $SW.Elapsed.TotalSeconds; Log "$($inst.Name): player window after $([math]::Round($inst.WinAt - $inst.Launched, 1)) s" }
    }
}
# The companion's side of one control connection: accept (again after a loss), read lines, send lines
function Poll-Control($inst) {
    if ($inst.Role -ne 'match') { return }
    if (-not $inst.Client -and $inst.Listener -and $inst.Listener.Pending()) {
        $inst.Client = $inst.Listener.AcceptTcpClient(); $inst.Client.NoDelay = $true
        $inst.Stream0 = $inst.Client.GetStream(); $inst.Buf = ''
        Log "$($inst.Name): control connection accepted"
    }
    if (-not $inst.Client) { return }
    try {
        while ($inst.Stream0.DataAvailable) {
            $b = New-Object byte[] 4096
            $n = $inst.Stream0.Read($b, 0, $b.Length)
            if ($n -le 0) { break }
            $inst.Buf += [Text.Encoding]::UTF8.GetString($b, 0, $n)
        }
    } catch { Log "$($inst.Name): control read failed: $_" }
    while (($nl = $inst.Buf.IndexOf("`n")) -ge 0) {
        $line = $inst.Buf.Substring(0, $nl).TrimEnd("`r"); $inst.Buf = $inst.Buf.Substring($nl + 1)
        $inst.Lines.Add([pscustomobject]@{ t = $SW.Elapsed.TotalSeconds; line = $line })
        Add-Content -LiteralPath (Join-Path $Run "control-$($inst.Name).log") -Value ("{0:N3} < {1}" -f $SW.Elapsed.TotalSeconds, $line)
        if ($line -like 'hello *') { $inst.Hello = $line }
        elseif ($line -like 'state *') { $inst.State = $line }
    }
}
function Send-Control($inst, [string]$line) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($line + "`n")
    $inst.Stream0.Write($bytes, 0, $bytes.Length); $inst.Stream0.Flush()
    Add-Content -LiteralPath (Join-Path $Run "control-$($inst.Name).log") -Value ("{0:N3} > {1}" -f $SW.Elapsed.TotalSeconds, $line)
}
function Check-Alive {
    foreach ($n in $Names) { $x = $I[$n]; if ($x.Proc -and $x.Proc.HasExited -and -not $x.Gone) { throw "$n (PID $($x.Pid)) exited unexpectedly, exit code $($x.Proc.ExitCode); see $($x.Dir)\vpinball.log" } }
}
# Sends one line (or several, in one write) and waits for a line that matches; the answer with the milliseconds it took, or $null
function Ask($inst, [string]$send, [string]$pattern, [double]$timeout) {
    $from = $inst.Lines.Count; $t0 = $SW.Elapsed.TotalSeconds
    Send-Control $inst $send
    $got = Wait-Line $inst $pattern $timeout $from
    if ($got) { [pscustomobject]@{ line = $got.line; ms = [math]::Round(($got.t - $t0) * 1000); index = $inst.Lines.IndexOf($got); t = $got.t } } else { $null }
}
function Pump([double]$seconds) {   # let time pass while the control connections are read
    $until = $SW.Elapsed.TotalSeconds + $seconds
    do { foreach ($n in $Names) { Poll-Control $I[$n] }; Check-Alive; Start-Sleep -Milliseconds 5 } while ($SW.Elapsed.TotalSeconds -lt $until)
}
# Waits for a line of this instance that matches; returns it (with .t) or $null after the timeout. Lines before $from are not looked at.
function Wait-Line($inst, [string]$pattern, [double]$timeout, [int]$from) {
    $until = $SW.Elapsed.TotalSeconds + $timeout
    for (;;) {
        foreach ($n in $Names) { Poll-Control $I[$n] }
        for ($k = $from; $k -lt $inst.Lines.Count; $k++) { if ($inst.Lines[$k].line -match $pattern) { return $inst.Lines[$k] } }
        if ($SW.Elapsed.TotalSeconds -ge $until) { return $null }
        Check-Alive
        Start-Sleep -Milliseconds 2
    }
}
function Get-Shot([IntPtr]$h) {   # the window's picture, in memory
    $r = [MatchWin]::Bounds($h); $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $ht, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $hdc = $g.GetHdc(); [void][MatchWin]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc) } finally { $g.Dispose() }
    $bmp
}
function Get-Pixels($bmp) {
    $small = New-Object System.Drawing.Bitmap 192, 108, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($bmp, 0, 0, 192, 108); $g.Dispose()
    $bd = $small.LockBits((New-Object System.Drawing.Rectangle 0, 0, 192, 108), 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($bd.Stride * 108); [Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bytes, 0, $bytes.Length)
    $small.UnlockBits($bd); $small.Dispose(); , $bytes
}
$SaveEvery = [math]::Max(1, [int][math]::Ceiling($Swaps / 12.0))   # pictures kept as files: every swap that fails, and a sample of the others
function Shot($inst, [string]$tag) {
    $bmp = Get-Shot $inst.Win
    if (-not $bmp) { return $null }
    [pscustomobject]@{ Bmp = $bmp; Pix = (Get-Pixels $bmp); Tag = $tag; T = $SW.Elapsed.TotalSeconds }
}
function Keep([object[]]$shots, [string]$prefix) {
    foreach ($s in $shots) { if ($s -and $s.Bmp) { try { $s.Bmp.Save((Join-Path $Run "shots\$prefix-$($s.Tag).png"), [System.Drawing.Imaging.ImageFormat]::Png) } catch {} } }
}
function Drop([object[]]$shots) { foreach ($s in $shots) { if ($s -and $s.Bmp) { $s.Bmp.Dispose(); $s.Bmp = $null } } }
function D($a, $b) { if ($a -and $b) { [math]::Round(([MatchWin]::Diff($a.Pix, $b.Pix))[0], 3) } else { [double]::NaN } }

# One watch phase of $w on $p's stream (or on a given port), with the pictures around it. Returns a row.
function Watch-Phase($w, [int]$port, $p, [int]$num, [string]$endHow) {
    $row = [ordered]@{ swap = $num; watcher = $w.Name; player = $(if ($p) { $p.Name } else { '' }); result = ''; watching_ms = $null; playing_ms = $null;
        noise = $null; before_after = $null; before_mid = $null; mid_vs_player = $null; t_watch = $null; t_play = $null }
    $b0 = Shot $w 'before0'
    Pump 0.12
    $b1 = Shot $w 'before'
    $from = $w.Lines.Count
    $t0 = $SW.Elapsed.TotalSeconds; $row.t_watch = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    Send-Control $w "watch 127.0.0.1:$port"
    $got = Wait-Line $w '^state (watching|failed .*|playing)$' 8 $from   # the companion gives a watch 5 s
    $mid = $null; $midP = $null
    if ($got -and $got.line -eq 'state watching') {
        $row.watching_ms = [math]::Round(($got.t - $t0) * 1000)
        Pump ($WatchSeconds / 2)
        if ($p) { $midP = Shot $p 'mid-player'; Pump (($DelayMs + 20) / 1000.0) }   # the watcher shows the player's picture of DelayMs ago
        $mid = Shot $w 'mid'
        Pump ($WatchSeconds / 2)
        $from2 = $w.Lines.Count
        $t1 = $SW.Elapsed.TotalSeconds; $row.t_play = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
        if ($endHow -eq 'play') {
            Send-Control $w 'play'
            $back = Wait-Line $w '^state playing$' 5 $from2
        } elseif ($endHow -eq 'cut') {
            Log "$($w.Name): cutting its control connection mid-watch"
            $w.Stream0.Dispose(); $w.Client.Close(); $w.Client = $null; $w.Stream0 = $null
            # it must come back by itself and connect again: hello, state playing
            $back = Wait-Line $w '^state playing$' 8 $from2
        }
        if ($back) { $row.playing_ms = [math]::Round(($back.t - $t1) * 1000); $row.result = 'ok' } else { $row.result = 'no "state playing" after the watch' }
    } elseif ($got -and $got.line -like 'state failed*') {
        $row.result = $got.line
        $row.watching_ms = [math]::Round(($got.t - $t0) * 1000)
        $back = Wait-Line $w '^state playing$' 3 ($w.Lines.IndexOf($got))
        if (-not $back) { $row.result += ' (and no "state playing" after it)' }
    } else { $row.result = 'no answer to watch' }
    $a1 = Shot $w 'after'
    $row.noise = D $b0 $b1; $row.before_after = D $b1 $a1; $row.before_mid = D $b1 $mid; $row.mid_vs_player = D $mid $midP
    $bad = ($row.result -ne 'ok') -or ($row.before_after -gt $MaxPictureDiff)
    if ($bad -or ($num % $SaveEvery) -eq 0 -or $num -le 2) { Keep @($b1, $mid, $midP, $a1) ("s{0:000}-{1}" -f $num, $w.Name) }
    Drop @($b0, $b1, $mid, $midP, $a1)
    [pscustomobject]$row
}

# ---------------------------------------------------------------- run
$fail = $null; $cleanOk = $true; $regOk = $true; $bamOk = $true; $vpmOk = $true
$rows = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
try {
    Log ("run folder {0}; scenario {1}; exe {2} (run as {3})" -f $Run, $Scenario, $Exe, $RunAs)
    Log ("other VPX processes (left alone): {0}" -f ((Get-Process -Name 'VPinballX*' -ErrorAction SilentlyContinue | ForEach-Object Id) -join ','))
    foreach ($n in $Names) {
        $x = $I[$n]
        if ($x.Role -eq 'match') { $x.Listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $x.Ctl); $x.Listener.Start() }
    }
    # one after the other: two instances loading at once take longer than twice one
    foreach ($n in $Names) {
        $x = $I[$n]
        Start-Instance $x
        while (-not $x.Win) {
            foreach ($m in $Names) { Poll-Window $I[$m]; Poll-Control $I[$m] }
            Check-Alive
            if (($SW.Elapsed.TotalSeconds - $x.Launched) -gt $WindowTimeout) { throw "no player window for $n after $WindowTimeout s" }
            Start-Sleep -Milliseconds 20
        }
    }
    foreach ($n in $Names) {
        $x = $I[$n]; if ($x.Role -ne 'match') { continue }
        if (-not (Wait-Line $x '^state playing$' 20 0)) { throw "$n never said 'state playing' on its control connection" }
        # hello 1 <build> <editables> <table hash> <stream port>: the port must be the one its stream listener is bound to
        if ($x.Hello -notmatch '^hello 1 \S+ \d+ [0-9a-f]{16} (\d+)$' -or [int]$Matches[1] -ne $x.Stream) { throw "${n}: unexpected hello line '$($x.Hello)' (the stream port is $($x.Stream))" }
        Log "${n}: $($x.Hello)"
    }
    Log "all instances up; warming up $WarmupSeconds s"
    Pump $WarmupSeconds

    switch ($Scenario) {
        'swap' {
            # A match as the companion runs it: one table is up, the other watches it, and at each grant the watcher is
            # called back while the player is sent to watch it. The two commands go out at the same moment in a match, so
            # here in the three orders by turns: 1 = the watch first and the play 150 ms later (the new watcher reaches a
            # table that is still watching: it must be held, not refused, and get a keyframe of the restored table),
            # 2 = the play first and the watch 150 ms later, 0 = both at once.
            $up = $I.a; $wt = $I.b; $kept = @{}
            $y0 = Shot $wt 'before0'; Pump 0.12; $y1 = Shot $wt 'before'
            $x = Ask $wt "watch 127.0.0.1:$($up.Stream)" '^state (watching|failed .*)$' 8
            if (-not $x -or $x.line -ne 'state watching') { throw "the first watch ($($wt.Name) of $($up.Name)) was answered '$($x.line)'" }
            Drop @($y0); $kept[$wt.Name] = $y1
            Pump $WatchSeconds
            for ($s = 1; $s -le $Swaps; $s++) {
                $X = $wt; $Y = $up; $mode = $s % 3   # X is called back, Y is sent to watch X
                $row = [ordered]@{ swap = $s; watcher = $Y.Name; player = $X.Name; order = @('both at once', 'watch first', 'play first')[$mode]; result = ''; watching_ms = $null; playing_ms = $null;
                    noise = $null; before_after = $null; before_mid = $null; mid_vs_player = $null; t_watch = $null; t_play = $null }
                $y0 = Shot $Y 'before0'; Pump 0.12; $y1 = Shot $Y 'before'
                $fromX = $X.Lines.Count; $fromY = $Y.Lines.Count
                $row.t_watch = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff'); $row.t_play = $row.t_watch
                $watchCmd = "watch 127.0.0.1:$($X.Stream)"
                if ($mode -eq 1) { $tw = $SW.Elapsed.TotalSeconds; Send-Control $Y $watchCmd; Pump 0.15; $tp = $SW.Elapsed.TotalSeconds; Send-Control $X 'play' }
                elseif ($mode -eq 2) { $tp = $SW.Elapsed.TotalSeconds; Send-Control $X 'play'; Pump 0.15; $tw = $SW.Elapsed.TotalSeconds; Send-Control $Y $watchCmd }
                else { $tp = $SW.Elapsed.TotalSeconds; $tw = $tp; Send-Control $Y $watchCmd; Send-Control $X 'play' }
                $px = Wait-Line $X '^state (playing|failed .*)$' 5 $fromX
                $xa = Shot $X 'after'
                $wy = Wait-Line $Y '^state (watching|failed .*|playing)$' 8 $fromY
                if ($px) { $row.playing_ms = [math]::Round(($px.t - $tp) * 1000) }
                if ($wy) { $row.watching_ms = [math]::Round(($wy.t - $tw) * 1000) }
                $row.result = if (-not $px -or $px.line -ne 'state playing') { "$($X.Name) called back: $(if ($px) { $px.line } else { 'no answer' })" }
                              elseif (-not $wy -or $wy.line -ne 'state watching') { "$($Y.Name) sent to watch: $(if ($wy) { $wy.line } else { 'no answer' })" } else { 'ok' }
                $mid = $null; $midP = $null
                if ($row.result -eq 'ok') { Pump ($WatchSeconds / 2); $midP = Shot $X 'mid-player'; Pump (($DelayMs + 20) / 1000.0); $mid = Shot $Y 'mid'; Pump ($WatchSeconds / 2) }
                $row.noise = D $y0 $y1; $row.before_after = D $kept[$X.Name] $xa; $row.before_mid = D $y1 $mid; $row.mid_vs_player = D $mid $midP
                $r = [pscustomobject]$row; $rows.Add($r)
                if ($r.result -ne 'ok') { Log ("swap {0} ({1}): {2}" -f $s, $r.order, $r.result) }
                elseif ($s % 5 -eq 0 -or $s -le 3) { Log ("swap {0} ({1}): {2} playing after {3} ms, {4} watching it after {5} ms; {2}'s picture before its watch / after it {6} (noise {7}); {4} before/mid {8}, mid/player {9}" -f $s, $r.order, $X.Name, $r.playing_ms, $Y.Name, $r.watching_ms, $r.before_after, $r.noise, $r.before_mid, $r.mid_vs_player) }
                if (($r.result -ne 'ok') -or ($r.before_after -gt $MaxPictureDiff) -or ($s % $SaveEvery) -eq 0 -or $s -le 3) {
                    $kept[$X.Name].Tag = 'before'; Keep @($kept[$X.Name], $xa) ("s{0:000}-{1}" -f $s, $X.Name); Keep @($midP, $mid) ("s{0:000}-{1}" -f $s, $Y.Name) }
                Drop @($y0, $mid, $midP, $xa, $kept[$X.Name]); $kept.Remove($X.Name); $kept[$Y.Name] = $y1
                if ($r.result -ne 'ok') {
                    # put the pair back into a known state: X plays, Y watches it
                    [void](Ask $X 'play' '^state playing$' 3); $z = Ask $Y $watchCmd '^state (watching|failed .*)$' 8
                    if (-not $z -or $z.line -ne 'state watching') { throw "could not get back to one playing and one watching after swap $s" }
                    Pump $WatchSeconds
                }
                $up = $X; $wt = $Y
            }
            $z = Ask $wt 'play' '^state playing$' 3
            if (-not $z) { $notes.Add("$($wt.Name) did not answer the last play") }
        }
        'visit' {
            for ($s = 1; $s -le $Swaps; $s++) {
                $w = if ($s % 2) { $I.b } else { $I.a }; $p = if ($s % 2) { $I.a } else { $I.b }
                $r = Watch-Phase $w $p.Stream $p $s 'play'
                $rows.Add($r)
                if ($r.result -ne 'ok') { Log ("swap {0}: {1} watching {2}: {3}" -f $s, $w.Name, $p.Name, $r.result) }
                elseif ($s % 5 -eq 0 -or $s -le 2) { Log ("swap {0}: {1} watched {2}: watching after {3} ms, playing after {4} ms, picture before/after {5} (noise {6}, before/mid {7}, mid/player {8})" -f $s, $w.Name, $p.Name, $r.watching_ms, $r.playing_ms, $r.before_after, $r.noise, $r.before_mid, $r.mid_vs_player) }
                Pump $PlaySeconds
            }
        }
        'nolistener' {
            $r = Watch-Phase $I.a $DeadPort $null 1 'play'
            $rows.Add($r); Log "watch of a dead port: $($r.result) after $($r.watching_ms) ms; picture before/after $($r.before_after)"
            if ($r.result -ne 'state failed timeout') { $notes.Add("expected 'state failed timeout', got '$($r.result)'") }
            if ($r.watching_ms -lt 3500 -or $r.watching_ms -ge 5000) { $notes.Add("the failure came after $($r.watching_ms) ms, expected about 4000 and under 5000") }
            Pump 3
            # and it still takes orders: a play is answered
            $from = $I.a.Lines.Count; Send-Control $I.a 'play'
            if (-not (Wait-Line $I.a '^state playing$' 3 $from)) { $notes.Add('no answer to a play after the failed watch') }
            $from = $I.a.Lines.Count; Send-Control $I.a 'dance'
            if (-not (Wait-Line $I.a '^state playing$' 3 $from)) { $notes.Add('no "state playing" after an unknown command') }
            $from = $I.a.Lines.Count; Send-Control $I.a 'watch nowhere'
            if (-not (Wait-Line $I.a '^state failed badaddress$' 3 $from)) { $notes.Add('no "state failed badaddress" after a watch without an address') }
            Pump 2
        }
        'controllost' {
            $r = Watch-Phase $I.b $I.a.Stream $I.a 1 'cut'
            $rows.Add($r); Log "control connection cut mid-watch: $($r.result), playing again (hello and state on a new connection) after $($r.playing_ms) ms; picture before/after $($r.before_after)"
            Pump 3
            # the new connection works: a normal watch phase
            $r2 = Watch-Phase $I.b $I.a.Stream $I.a 2 'play'
            $rows.Add($r2); Log "after the reconnect: $($r2.result)"
            Pump 2
        }
        'mismatch' {
            $r = Watch-Phase $I.b $I.a.Stream $I.a 1 'play'
            $rows.Add($r); Log "watch of another table: $($r.result) after $($r.watching_ms) ms; picture before/after $($r.before_after)"
            if ($r.result -ne 'state failed refused') { $notes.Add("expected 'state failed refused', got '$($r.result)'") }
            Pump 3
        }
        'reaim' {
            for ($s = 1; $s -le [math]::Max(1, [math]::Min($Swaps, 10)); $s++) {
                $c = $I.c
                $b0 = Shot $c 'before'
                $g1 = Ask $c "watch 127.0.0.1:$($I.a.Stream)" '^state (watching|failed .*)$' 8
                Pump ($WatchSeconds / 2); $m1 = Shot $c 'mid-a'; $pa = Shot $I.a 'mid-player-a'; Pump ($WatchSeconds / 2)
                $fromRe = $c.Lines.Count
                $g2 = Ask $c "watch 127.0.0.1:$($I.b.Stream)" '^state (watching|failed .*)$' 8   # the re-aim must be announced again
                Pump ($WatchSeconds / 2); $m2 = Shot $c 'mid-b'; $pb = Shot $I.b 'mid-player-b'; Pump ($WatchSeconds / 2)
                if (@($c.Lines | Select-Object -Skip $fromRe | Where-Object { $_.line -eq 'state playing' }).Count) { $notes.Add("re-aim ${s}: c said 'state playing' between the two players") }
                $g3 = Ask $c 'play' '^state playing$' 5
                $a1 = Shot $c 'after'
                $ok = $g1 -and $g1.line -eq 'state watching' -and $g2 -and $g2.line -eq 'state watching' -and $g3
                $r = [pscustomobject][ordered]@{ swap = $s; watcher = 'c'; player = 'a then b'; result = $(if ($ok) { 'ok' } else { "watch a: $($g1.line); re-aim b: $($g2.line); play: $($g3.line)" });
                    watching_ms = $(if ($g2) { $g2.ms }); playing_ms = $(if ($g3) { $g3.ms }); noise = $null; before_after = (D $b0 $a1); before_mid = (D $b0 $m2); mid_vs_player = (D $m2 $pb); mid_a_vs_a = (D $m1 $pa); t_watch = $null; t_play = $null }
                $rows.Add($r); Log "re-aim $s`: $($r.result); watching a after $($g1.ms) ms, b after the re-aim after $($g2.ms) ms; c's picture vs a's while watching a: $($r.mid_a_vs_a); vs b's after the re-aim: $($r.mid_vs_player); before/after: $($r.before_after)"
                Keep @($b0, $m1, $pa, $m2, $pb, $a1) ("s{0:000}-c" -f $s); Drop @($b0, $m1, $pa, $m2, $pb, $a1)
                Pump $PlaySeconds
            }
        }
        'contract' {
            $b = $I.b; $aAddr = "127.0.0.1:$($I.a.Stream)"
            $say = { param([string]$what, $ans, [string]$want, [int]$within)
                $ok = $ans -and $ans.line -match $want -and $ans.ms -lt $within
                Log ("contract: {0}: {1}{2}" -f $what, $(if ($ans) { "'$($ans.line)' after $($ans.ms) ms" } else { 'no answer' }), $(if ($ok) { '' } else { "   <- expected $want within $within ms" }))
                if (-not $ok) { $notes.Add("contract: ${what}: $(if ($ans) { "'$($ans.line)' after $($ans.ms) ms" } else { 'no answer' }), expected $want within $within ms") } }
            # 1 and 7: the first two lines of each connection, sent before this script sent anything; the tokens of hello; one identity for one table
            foreach ($n in $Names) {
                $x = $I[$n]
                if ($x.Lines.Count -lt 2 -or $x.Lines[0].line -notlike 'hello *' -or $x.Lines[1].line -ne 'state playing') { $notes.Add("contract: ${n}'s first lines are not hello, state playing") }
                if ($x.Hello -cnotmatch '^hello 1 [\x21-\x7E]{1,128} [\x21-\x7E]{1,128} [\x21-\x7E]{1,128} [\x21-\x7E]{1,128}$') { $notes.Add("contract: ${n}'s hello has a token that is not 1 to 128 printable ASCII characters: '$($x.Hello)'") }
            }
            if ((($I.a.Hello -split ' ')[3, 4] -join ' ') -cne (($I.b.Hello -split ' ')[3, 4] -join ' ')) { $notes.Add("contract: a and b report different identities for the same table file: '$($I.a.Hello)' / '$($I.b.Hello)'") }
            Log "contract: first lines and hello tokens checked; identity '$((($I.a.Hello -split ' ')[3, 4]) -join ' ')' on both"
            # 4: a play while playing
            & $say 'play while playing' (Ask $b 'play' '^state ' 3) '^state playing$' 2000
            # 2, 3, 6: a watch that cannot work is answered within 5 s, and the table says it plays without being told
            $x = Ask $b "watch 127.0.0.1:$DeadPort" '^state ' 8
            & $say 'watch of a port where nothing listens' $x '^state failed timeout$' 5000
            if ($x) { $pl = Wait-Line $b '^state playing$' 2 ($x.index + 1)
                & $say 'state playing by itself after the failure' $(if ($pl) { [pscustomobject]@{ line = $pl.line; ms = [math]::Round(($pl.t - $x.t) * 1000) } }) '^state playing$' 1000 }
            Pump 1
            # 5: play, then watch as soon as state playing is there
            & $say 'watch a' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            Pump 1.5
            & $say 'play' (Ask $b 'play' '^state ' 3) '^state playing$' 2000
            & $say 'watch right after state playing' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            Pump 1.5
            # 5 again: play and watch in one write; both answers, in order
            $from = $b.Lines.Count; $t0 = $SW.Elapsed.TotalSeconds
            Send-Control $b "play`nwatch $aAddr"
            $p1 = Wait-Line $b '^state playing$' 3 $from
            $w1 = if ($p1) { Wait-Line $b '^state (watching|failed .*)$' 8 ($b.Lines.IndexOf($p1) + 1) }
            & $say 'play and watch in one write: the play' $(if ($p1) { [pscustomobject]@{ line = $p1.line; ms = [math]::Round(($p1.t - $t0) * 1000) } }) '^state playing$' 2000
            & $say 'play and watch in one write: the watch' $(if ($w1) { [pscustomobject]@{ line = $w1.line; ms = [math]::Round(($w1.t - $t0) * 1000) } }) '^state watching$' 5000
            Pump 1.5
            # 2 and 11: the same watch again is a re-aim: the stream is dropped and connected again (the fork's log says so),
            # a fresh state watching answers it, and no state playing comes in between
            $from = $b.Lines.Count
            & $say 'the same watch again (a re-aim to the same address)' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            if (@($b.Lines | Select-Object -Skip $from | Where-Object { $_.line -eq 'state playing' }).Count) { $notes.Add('contract: a re-aim to the same address was answered with a state playing in between') }
            Pump 1
            # 10: a watcher that reaches b's stream while b is watching is accepted and held: not a byte until b plays, then b's HELLO
            $raw = [Net.Sockets.TcpClient]::new(); $raw.Connect('127.0.0.1', $b.Stream)
            Pump 1.5
            $early = $raw.Available
            $ans = Ask $b 'play' '^state playing$' 3
            $tPlay = $SW.Elapsed.TotalSeconds; while ($raw.Available -eq 0 -and ($SW.Elapsed.TotalSeconds - $tPlay) -lt 2) { Pump 0.01 }
            $late = $raw.Available; $raw.Close()
            Log "contract: a connection to b's stream while b watches: $early bytes in 1.5 s; after b's 'state playing': $late bytes within $([math]::Round(($SW.Elapsed.TotalSeconds - $tPlay) * 1000)) ms"
            if ($early -ne 0) { $notes.Add("contract: b sent $early bytes to a watcher while it was watching (it must hold the connection silent)") }
            if (-not $ans -or $late -le 0) { $notes.Add('contract: a held watcher was not served after b came back to playing') }
            # 10: held 10 s at most, then closed, still without a byte
            & $say 'watch a' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            $raw = [Net.Sockets.TcpClient]::new(); $raw.Connect('127.0.0.1', $b.Stream); $t0 = $SW.Elapsed.TotalSeconds; $closedAt = $null; $bytes = 0
            while (($SW.Elapsed.TotalSeconds - $t0) -lt 13) {
                Pump 0.05
                if ($raw.Available -gt 0) { $bytes = $raw.Available; break }
                if ($raw.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead) -and $raw.Available -eq 0) { $closedAt = $SW.Elapsed.TotalSeconds - $t0; break }
            }
            $raw.Close()
            Log "contract: a connection held while b keeps watching: $bytes bytes received, closed by b after $(if ($null -ne $closedAt) { [math]::Round($closedAt, 1) } else { 'more than 13' }) s"
            if ($bytes -ne 0 -or $null -eq $closedAt -or $closedAt -lt 9.5 -or $closedAt -gt 11.5) { $notes.Add('contract: a held connection was not closed, silent, after about 10 s') }
            & $say 'play' (Ask $b 'play' '^state ' 3) '^state playing$' 2000
            # back to watching for what follows
            & $say 'watch a' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            Pump 1
            # an unknown command brings the own table back
            & $say 'an unknown command while watching' (Ask $b 'dance' '^state ' 3) '^state playing$' 2000
            & $say 'a watch without an address' (Ask $b 'watch nowhere' '^state ' 3) '^state failed badaddress$' 2000
            Pump 1
            # 6: the watched player closes its table: state failed, then state playing, with no play sent
            & $say 'watch a' (Ask $b "watch $aAddr" '^state ' 8) '^state watching$' 5000
            Pump 1.5
            $from = $b.Lines.Count; $t0 = $SW.Elapsed.TotalSeconds
            $I.a.Gone = $true
            $pl = [MatchWin]::ForPid($I.a.Pid) | Where-Object { $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
            [void][MatchWin]::PostMessage($pl.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            $f1 = Wait-Line $b '^state failed .*$' 12 $from
            $p2 = if ($f1) { Wait-Line $b '^state playing$' 2 ($b.Lines.IndexOf($f1) + 1) }
            & $say 'the watched player closes its table' $(if ($f1) { [pscustomobject]@{ line = $f1.line; ms = [math]::Round(($f1.t - $t0) * 1000) } }) '^state failed closed$' 12000
            & $say 'state playing after it, by itself' $(if ($p2) { [pscustomobject]@{ line = $p2.line; ms = [math]::Round(($p2.t - $f1.t) * 1000) } }) '^state playing$' 1000
            Pump 2
            # 8: no line the fork sent comes near the companion's limit of 2048 bytes
            foreach ($n in $Names) { $long = @($I[$n].Lines | Where-Object { [Text.Encoding]::UTF8.GetByteCount($_.line) -gt 512 }); if ($long.Count) { $notes.Add("contract: $n sent $($long.Count) lines over 512 bytes") } }
        }
        'streamlost' {
            $b = $I.b
            $x = Ask $b "watch 127.0.0.1:$($I.a.Stream)" '^state ' 8
            Log "stream lost: watching: $(if ($x) { "'$($x.line)' after $($x.ms) ms" } else { 'no answer' })"
            Pump 2
            $shotBefore = $null
            $from = $b.Lines.Count; $t0 = $SW.Elapsed.TotalSeconds
            Log "killing a (PID $($I.a.Pid), started by this script): the stream ends without a goodbye"
            $I.a.Gone = $true; Stop-Process -Id $I.a.Pid -Force
            $f1 = Wait-Line $b '^state failed .*$' 8 $from
            $p2 = if ($f1) { Wait-Line $b '^state playing$' 2 ($b.Lines.IndexOf($f1) + 1) }
            Log ("stream lost: {0}; then {1}" -f $(if ($f1) { "'$($f1.line)' after $([math]::Round(($f1.t - $t0) * 1000)) ms" } else { 'no state failed within 8 s' }), $(if ($p2) { "'state playing' $([math]::Round(($p2.t - $f1.t) * 1000)) ms later" } else { 'NO state playing' }))
            if (-not $f1 -or $f1.line -ne 'state failed lost') { $notes.Add("stream lost: expected 'state failed lost', got '$($f1.line)'") }
            elseif (($f1.t - $t0) -lt 2.5 -or ($f1.t - $t0) -gt 4.5) { $notes.Add("stream lost: the failure came after $([math]::Round(($f1.t - $t0) * 1000)) ms, expected about 3000") }
            if (-not $p2) { $notes.Add('stream lost: no "state playing" after the failure') }
            Pump 3
            & { $y = Ask $b 'play' '^state ' 3; if (-not $y -or $y.line -ne 'state playing') { $notes.Add('stream lost: no answer to a play afterwards') } }
        }
        'stalled' {
            # A stream that stays open and delivers nothing: A is suspended (every thread stopped, its sockets stay open)
            $b = $I.b
            $x = Ask $b "watch 127.0.0.1:$($I.a.Stream)" '^state ' 8
            Log "stalled: watching: $(if ($x) { "'$($x.line)' after $($x.ms) ms" } else { 'no answer' })"
            Pump 2
            $from = $b.Lines.Count; $t0 = $SW.Elapsed.TotalSeconds
            Log "suspending a (PID $($I.a.Pid), started by this script)"
            [void][MatchWin]::NtSuspendProcess($I.a.Proc.Handle)
            try {
                $f1 = Wait-Line $b '^state failed .*$' 8 $from
                $p2 = if ($f1) { Wait-Line $b '^state playing$' 2 ($b.Lines.IndexOf($f1) + 1) }
            } finally { [void][MatchWin]::NtResumeProcess($I.a.Proc.Handle); Log 'a resumed' }
            Log ("stalled: {0}; then {1}" -f $(if ($f1) { "'$($f1.line)' after $([math]::Round(($f1.t - $t0) * 1000)) ms" } else { 'no state failed within 8 s' }), $(if ($p2) { "'state playing' $([math]::Round(($p2.t - $f1.t) * 1000)) ms later" } else { 'NO state playing' }))
            if (-not $f1 -or $f1.line -ne 'state failed stalled') { $notes.Add("stalled: expected 'state failed stalled', got '$($f1.line)'") }
            elseif (($f1.t - $t0) -lt 2.5 -or ($f1.t - $t0) -gt 4.5) { $notes.Add("stalled: the failure came after $([math]::Round(($f1.t - $t0) * 1000)) ms, expected about 3000") }
            if (-not $p2) { $notes.Add('stalled: no "state playing" after the failure') }
            Pump 3
            # A runs again: a new watch works
            $r = Watch-Phase $b $I.a.Stream $I.a 2 'play'
            $rows.Add($r); Log "after the stall: a new watch of a: $($r.result)"
            if ($r.result -ne 'ok') { $notes.Add("stalled: the watch after the stall: $($r.result)") }
            Pump 1
        }
        'plainwatch' {
            # W, a plain watch build, watches A the whole time. A and B swap; while A watches B, W must see A's own table, standing still.
            for ($s = 1; $s -le [math]::Max(2, [math]::Min($Swaps, 10)); $s++) {
                $w = if ($s % 2) { $I.a } else { $I.b }; $p = if ($s % 2) { $I.b } else { $I.a }
                $pa0 = Shot $I.a 'a-before'; Pump (($DelayMs + 20) / 1000.0); $pw0 = Shot $I.w 'w-before'
                $r = Watch-Phase $w $p.Stream $p $s 'play'
                Pump 1.0
                $pa1 = Shot $I.a 'a-after'; Pump (($DelayMs + 20) / 1000.0); $pw1 = Shot $I.w 'w-after'
                $r | Add-Member -NotePropertyName w_vs_a_before -NotePropertyValue (D $pa0 $pw0)
                $r | Add-Member -NotePropertyName w_vs_a_after -NotePropertyValue (D $pa1 $pw1)
                $rows.Add($r); Log "plain watcher, swap $s ($($w.Name) watched $($p.Name)): $($r.result); W's picture vs A's before $($r.w_vs_a_before), after $($r.w_vs_a_after)"
                Keep @($pa0, $pw0, $pa1, $pw1) ("s{0:000}-plain" -f $s); Drop @($pa0, $pw0, $pa1, $pw1)
                Pump $PlaySeconds
            }
        }
    }
    Pump 1.5
} catch {
    $fail = "$_"; Log ("at: " + $_.ScriptStackTrace)
    Log "FAILED: $fail"
} finally {
    try {
        foreach ($n in $Names) {
            $x = $I[$n]; if (-not $x.Proc) { continue }
            if (-not $x.Proc.HasExited) {
                $wins = [MatchWin]::ForPid($x.Pid)
                $pl = $wins | Where-Object { $_.Title -eq 'Visual Pinball Player' } | Select-Object -First 1
                if ($pl) { [void][MatchWin]::PostMessage($pl.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
                else { $wins | Where-Object Visible | ForEach-Object { [void][MatchWin]::PostMessage($_.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }
            }
        }
        foreach ($n in $Names) {
            $x = $I[$n]; if (-not $x.Proc) { continue }
            if (-not $x.Proc.WaitForExit(25000)) {
                $frame = [MatchWin]::ForPid($x.Pid) | Where-Object { $_.Cls -eq 'VPinball' } | Select-Object -First 1
                if ($frame) { [void][MatchWin]::PostMessage($frame.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); [void]$x.Proc.WaitForExit(5000) }
            }
            if (-not $x.Proc.HasExited) {
                $cleanOk = $false; $x.Killed = $true
                Log "$n PID $($x.Pid) did not exit; last resort: killing my own PID"
                try { Stop-Process -Id $x.Pid -Force; [void]$x.Proc.WaitForExit(10000) } catch { Log "kill failed: $_" }
            }
            $x.Exit = $x.Proc.ExitCode
            Log ("{0} PID {1} exited, code {2}{3}" -f $n, $x.Pid, $x.Exit, $(if ($x.Killed) { ' (killed by me)' } else { '' }))
        }
        foreach ($n in $Names) { $x = $I[$n]; try { if ($x.Client) { $x.Client.Close() }; if ($x.Listener) { $x.Listener.Stop() } } catch {} }
    } catch { $cleanOk = $false; Log "cleanup error: $_" }
    try { $regOk = Restore-Reg $regBefore } catch { $regOk = $false; Log "registry restore error: $_" }
    try { $bamOk = Restore-Bam } catch { $bamOk = $false; Log "BAM restore error: $_" }
    try { $vpmOk = Restore-Vpm } catch { $vpmOk = $false; Log "VPinMAME restore error: $_" }
}

# ---------------------------------------------------------------- what the instances logged
function Stamp([string]$line) { if ($line -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})') { [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null) } else { $null } }
$Ana = [ordered]@{}   # per instance
foreach ($n in $Names) {
    $x = $I[$n]
    $a = [ordered]@{ phases = 0; touched = 0; never_touched = 0; physics_identical = 0; physics_different = 0; put_back = 0; put_back_own_value = 0; put_back_failed = 0; put_back_lost = 0;
        prepass_puts = 0; prepass_failed = 0; undo_max = 0; keyframes = 0; keyframe_calls_failed = 0; replay_failed = 0; replay_unresolved = 0; check_runs = 0; check_differ = 0; check_approx = 0;
        warnings = 0; errors = 0; leave_ms_max = 0.0; enter_ms_max = 0.0; prepass_ms_max = 0.0; keys_passed = 0; reasons = @{}; switch_times = (New-Object System.Collections.Generic.List[datetime]);
        frames_over_25 = 0; frames_over_100 = 0; frame_max_ms = 0.0; switch_frame_max_ms = 0.0; switch_frames_over = 0; ball_episodes = 0; ball_episodes_after_first_watch = 0; ball_log_lines = 0;
        entered_with_moving_ball = 0; first_watch_gametime = $null; held = 0; held_served = 0; held_closed = 0; held_ms_max = 0.0; first_bad = (New-Object System.Collections.Generic.List[string]) }
    $cl = Join-Path $x.Dir 'castcade.log'
    $balls = New-Object System.Collections.Generic.List[object]; $lastBallT = 0
    if (Test-Path -LiteralPath $cl) {
        foreach ($line in [IO.File]::ReadLines($cl)) {
            if ($line -match '\] balls t=(\d+) n=(\d+)(.*)$') {
                $a.ball_log_lines++
                $t = [int]$Matches[1]; $ys = @(); foreach ($m in [regex]::Matches($Matches[3], '#\d+ (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')) { $ys += [double]$m.Groups[2].Value }
                $balls.Add([pscustomobject]@{ t = $t; ys = $ys }); $lastBallT = $t; continue
            }
            if ($null -eq $a.first_watch_gametime -and $line -match 'entering watch 1 of ') { $a.first_watch_gametime = $lastBallT }
            if ($line -match '\] WARNING ') { $a.warnings++; if ($a.first_bad.Count -lt 12) { $a.first_bad.Add($line) } }
            if ($line -match '\] ERROR ') { $a.errors++; if ($a.first_bad.Count -lt 12) { $a.first_bad.Add($line) } }
            if ($line -match 'entering watch \d+ of .*own game paused in ([\d.]+) ms') {
                $a.phases++; $a.enter_ms_max = [math]::Max($a.enter_ms_max, [double]$Matches[1]); $s = Stamp $line; if ($s) { $a.switch_times.Add($s) }
                # a ball of its own in motion when the watch began (speed over 1 VP unit per 10 ms)
                foreach ($m in [regex]::Matches($line, ' v \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)')) {
                    if (([math]::Abs([double]$m.Groups[1].Value) + [math]::Abs([double]$m.Groups[2].Value)) -gt 1.0) { $a.entered_with_moving_ball++; break } } }
            elseif ($line -match 'keyframe pre-pass: .* (\d+) of (\d+) own properties .*\((\d+) failed, (\d+) with no readable.*\); (\d+) in the undo list; ([\d.]+) ms') {
                $a.prepass_puts += [int]$Matches[1]; $a.prepass_failed += [int]$Matches[3]; $a.prepass_ms_max = [math]::Max($a.prepass_ms_max, [double]$Matches[6]); $s = Stamp $line; if ($s) { $a.switch_times.Add($s) } }
            elseif ($line -match '\[watch pid \d+\] keyframe applied: .*\((\d+) replayed, (\d+) failed or unresolved\)') { $a.keyframes++; $a.keyframe_calls_failed += [int]$Matches[2] }
            elseif ($line -match 'back to playing \((.*?)\) after [\d.]+ s of watch \d+: (.*?); (\d+) of (\d+) properties put back \((\d+) with this table''s own stored value, (\d+) failed, (\d+) lost\), (\d+) materials; own physics state (identical|DIFFERENT).*game clock set back (\d+) ms; (\d+) key events passed on; ([\d.]+) ms') {
                $a.reasons[$Matches[1]] = 1 + [int]$a.reasons[$Matches[1]]
                if ($Matches[2] -like '*never touched*') { $a.never_touched++ } else { $a.touched++ }
                $a.put_back += [int]$Matches[3]; $a.undo_max = [math]::Max($a.undo_max, [int]$Matches[4]); $a.put_back_own_value += [int]$Matches[5]; $a.put_back_failed += [int]$Matches[6]; $a.put_back_lost += [int]$Matches[7]
                if ($Matches[9] -eq 'identical') { $a.physics_identical++ } else { $a.physics_different++ }
                $a.keys_passed += [int]$Matches[11]; $a.leave_ms_max = [math]::Max($a.leave_ms_max, [double]$Matches[12]); $s = Stamp $line; if ($s) { $a.switch_times.Add($s) } }
            elseif ($line -match 'held: this table is not playing yet') { $a.held++ }
            elseif ($line -match 'served after ([\d.]+) ms on hold') { $a.held_served++; $a.held_ms_max = [math]::Max($a.held_ms_max, [double]$Matches[1]) }
            elseif ($line -match 'closed: held 10 s') { $a.held_closed++ }
            elseif ($line -match 'match check: (\d+) properties compared, (\d+) differ, (\d+) equal within') { $a.check_runs++; $a.check_differ += [int]$Matches[2]; $a.check_approx += [int]$Matches[3] }
            elseif ($line -match '\[watch pid \d+\] stats .*failed (\d+), unresolved (\d+)') { $a.replay_failed += [int]$Matches[1]; $a.replay_unresolved += [int]$Matches[2] }
        }
    }
    # ball-in-play episodes on the game clock (which stands still while this instance watches): a ball in the upper 60 % of
    # the range of y any ball was seen at; an episode ends when none has been there for 2 s
    if ($balls.Count) {
        $allY = @($balls | ForEach-Object { $_.ys }) ; if ($allY.Count) {
            $minY = ($allY | Measure-Object -Minimum).Minimum; $maxY = ($allY | Measure-Object -Maximum).Maximum; $limit = $minY + 0.6 * ($maxY - $minY)
            $in = $false; $last = -100000
            if (($maxY - $minY) -gt 500) {   # balls that never leave where they rest are no game
                foreach ($b in $balls) { $up = @($b.ys | Where-Object { $_ -lt $limit }).Count -gt 0
                    if ($up) { if (-not $in -and ($b.t - $last) -gt 2000) { $a.ball_episodes++; if ($null -ne $a.first_watch_gametime -and $b.t -gt $a.first_watch_gametime) { $a.ball_episodes_after_first_watch++ } }; $in = $true; $last = $b.t }
                    elseif ($in -and ($b.t - $last) -gt 2000) { $in = $false } }
            }
        }
    }
    $fl = Join-Path $x.Dir 'frames.log'
    if (Test-Path -LiteralPath $fl) {
        $first = if ($a.switch_times.Count) { ($a.switch_times | Measure-Object -Minimum).Minimum } else { $null }
        foreach ($line in [IO.File]::ReadLines($fl)) {
            if ($line -notmatch '^(\S+ \S+) frame (\d+): ([\d.]+) ms') { continue }
            $ms = [double]$Matches[3]; $at = Stamp $line
            if ($first -and $at -lt $first.AddSeconds(-1)) { continue }   # loading and the first frames are not the test's subject
            $a.frames_over_25++; if ($ms -ge 100) { $a.frames_over_100++ }; $a.frame_max_ms = [math]::Max($a.frame_max_ms, $ms)
            # at a switch: the frame ended within 1 s after a switch line, or began up to 0.3 s before it
            foreach ($st in $a.switch_times) { $d = ($at - $st).TotalSeconds; if ($d -ge -0.3 -and $d -le 1.0) { $a.switch_frame_max_ms = [math]::Max($a.switch_frame_max_ms, $ms); if ($ms -ge $MaxSwitchFrameMs) { $a.switch_frames_over++; if ($a.first_bad.Count -lt 16) { $a.first_bad.Add($(if ($line.Length -gt 300) { $line.Substring(0, 300) } else { $line })) } }; break } }
        }
    }
    $Ana[$n] = $a
}

# ---------------------------------------------------------------- summary
$rows | Export-Csv -LiteralPath (Join-Path $Run 'swaps.csv')
$okRows = @($rows | Where-Object { $_.result -eq 'ok' })
function Stats($vals) { $v = @($vals | Where-Object { $null -ne $_ -and -not [double]::IsNaN([double]$_) } | ForEach-Object { [double]$_ } | Sort-Object); if (-not $v.Count) { return 'n/a' }
    "median {0}, p95 {1}, max {2} (n={3})" -f [math]::Round($v[[int][math]::Floor(($v.Count - 1) * 0.5)], 3), [math]::Round($v[[int][math]::Floor(($v.Count - 1) * 0.95)], 3), [math]::Round($v[-1], 3), $v.Count }
Write-Host ''
Log '================ SUMMARY ================'
Log "run folder: $Run"
Log ("scenario {0}, table {1}{2}" -f $Scenario, (Split-Path -Leaf $Table), $(if ($Scenario -eq 'mismatch') { " against $(Split-Path -Leaf $OtherTable)" } else { '' }))
Log ("watch phases asked for: {0}; ended as asked: {1}; other results: {2}" -f $rows.Count, $okRows.Count, ((@($rows | Where-Object { $_.result -ne 'ok' } | Group-Object result | ForEach-Object { "$($_.Count)x '$($_.Name)'" })) -join ', '))
Log ("'state watching' after the watch command, ms: {0}" -f (Stats ($okRows | ForEach-Object watching_ms)))
Log ("'state playing' after the play command, ms:   {0}" -f (Stats ($okRows | ForEach-Object playing_ms)))
Log 'pictures, mean abs difference 0..255 at 192x108:'
Log ("  before the watch vs after it:                 {0}   <- must stay at or under {1}" -f (Stats ($rows | ForEach-Object before_after)), $MaxPictureDiff)
Log ("  two pictures 120 ms apart, no watch (noise):  {0}" -f (Stats ($rows | ForEach-Object noise)))
Log ("  before the watch vs in the middle of it:      {0}" -f (Stats ($rows | ForEach-Object before_mid)))
Log ("  watcher mid-watch vs the watched player:      {0}" -f (Stats ($rows | ForEach-Object mid_vs_player)))
$problems = New-Object System.Collections.Generic.List[string]
foreach ($n in $Names) {
    $x = $I[$n]; $a = $Ana[$n]
    Log ("{0} ({1}): PID {2}, exit code {3}{4}" -f $n, $x.Role, $x.Pid, $(if ($null -ne $x.Exit) { $x.Exit } else { 'n/a' }), $(if ($x.Killed) { ' (KILLED)' } else { '' }))
    if ($x.Role -eq 'match') {
        Log ("   watch phases {0} (table shown the other player's in {1}, never touched in {2}); ended by: {3}" -f $a.phases, $a.touched, $a.never_touched, (($a.reasons.Keys | ForEach-Object { "$_ x$($a.reasons[$_])" }) -join ', '))
        Log ("   own physics state after the watch: identical {0}, DIFFERENT {1}; properties put back {2} (largest undo list {3}; {4} with the own stored value), failed {5}, lost {6}" -f $a.physics_identical, $a.physics_different, $a.put_back, $a.undo_max, $a.put_back_own_value, $a.put_back_failed, $a.put_back_lost)
        Log ("   keyframes {0}, their calls failed {1}; pre-pass puts to the file's value {2}, failed {3}; replayed calls failed {4}, unresolved {5} (stats lines)" -f $a.keyframes, $a.keyframe_calls_failed, $a.prepass_puts, $a.prepass_failed, $a.replay_failed, $a.replay_unresolved)
        if ($Check) { Log ("   property check: {0} runs, {1} properties differ, {2} equal within 0.01 % only" -f $a.check_runs, $a.check_differ, $a.check_approx) }
        Log ("   longest: entering {0} ms, keyframe pre-pass {1} ms, leaving {2} ms; key events passed on at returns {3}" -f $a.enter_ms_max, $a.prepass_ms_max, $a.leave_ms_max, $a.keys_passed)
        Log ("   watchers that reached it while it was not playing: held {0}, served once it played {1} (longest hold {2} ms), closed after 10 s {3}" -f $a.held, $a.held_served, $a.held_ms_max, $a.held_closed)
        Log ("   balls put in play on its game clock: {0}, of which {1} after its first watch phase; watch phases that began with a ball of its own in motion: {2}" -f $a.ball_episodes, $a.ball_episodes_after_first_watch, $a.entered_with_moving_ball)
        if ($a.physics_different) { $problems.Add("${n}: own physics state DIFFERENT after $($a.physics_different) watch phases") }
        if ($a.put_back_failed -or $a.put_back_lost) { $problems.Add("${n}: $($a.put_back_failed) properties could not be put back, $($a.put_back_lost) lost") }
        if ($a.prepass_failed) { $problems.Add("${n}: $($a.prepass_failed) pre-pass puts failed") }
        if ($a.keyframe_calls_failed -or $a.replay_failed -or $a.replay_unresolved) { $problems.Add("${n}: replayed calls failed (keyframes $($a.keyframe_calls_failed), frames $($a.replay_failed), unresolved $($a.replay_unresolved))") }
        if ($Check -and $a.check_differ) { $problems.Add("${n}: property check found $($a.check_differ) differences") }
        if ($Check -and $a.check_runs -ne $a.phases) { $problems.Add("${n}: property check ran $($a.check_runs) times for $($a.phases) watch phases") }
    }
    if (-not $NoFrameLog) {
        Log ("   frames of 25 ms or more after the first switch: {0}, of 100 ms or more: {1}, longest {2} ms; longest at a switch {3} ms" -f $a.frames_over_25, $a.frames_over_100, $a.frame_max_ms, $a.switch_frame_max_ms)
        if ($a.switch_frames_over) { $problems.Add("${n}: $($a.switch_frames_over) frames of $MaxSwitchFrameMs ms or more at a switch (longest $($a.switch_frame_max_ms) ms)") }
    }
    Log ("   log: {0} warnings, {1} errors" -f $a.warnings, $a.errors)
    foreach ($l in $a.first_bad) { Log "      $l" }
    if (($null -eq $x.Exit -or $x.Exit -ne 0 -or $x.Killed) -and -not ($Scenario -eq 'streamlost' -and $n -eq 'a')) { $problems.Add("${n}: did not exit cleanly (exit code $($x.Exit))") }
}
$overPic = @($rows | Where-Object { $null -ne $_.before_after -and -not [double]::IsNaN([double]$_.before_after) -and [double]$_.before_after -gt $MaxPictureDiff })
if ($overPic.Count) { $problems.Add("$($overPic.Count) watch phases left a picture more than $MaxPictureDiff from the one before (swaps $((($overPic | ForEach-Object swap) | Select-Object -First 10) -join ','))") }
# Over a run of swaps the picture after a watch must be as near the one before it as two pictures of the running game
# 120 ms apart are: a table left with the other player's lamps passes the limit above on most swaps, not this one
function Median($vals) { $v = @($vals | Where-Object { $null -ne $_ -and -not [double]::IsNaN([double]$_) } | ForEach-Object { [double]$_ } | Sort-Object); if ($v.Count) { $v[[int][math]::Floor(($v.Count - 1) * 0.5)] } else { [double]::NaN } }
if (($Scenario -in 'swap', 'visit') -and $rows.Count -ge 6) {
    $mBA = Median ($rows | ForEach-Object before_after); $mN = Median ($rows | ForEach-Object noise); $lim = [math]::Round(1.25 * $mN + 0.1, 3)
    Log ("  median before/after {0} against 1.25 x the median noise + 0.1 = {1}" -f $mBA, $lim)
    if ($mBA -gt $lim) { $problems.Add("the pictures after the watch phases differ from those before them by a median of $mBA, more than the game's own noise allows ($lim)") }
}
$slow = @($rows | Where-Object { $null -ne $_.watching_ms -and [double]$_.watching_ms -ge 5000 })
if ($slow.Count) { $problems.Add("$($slow.Count) watch commands were answered after 5 s or more (the companion waits 5 s)") }
$slowPlay = @($rows | Where-Object { $null -ne $_.playing_ms -and [double]$_.playing_ms -ge 2000 -and $Scenario -ne 'controllost' })
if ($slowPlay.Count) { $problems.Add("$($slowPlay.Count) play commands were answered after 2 s or more (the companion waits 2 s)") }
switch ($Scenario) {
    { $_ -in 'swap', 'visit' } { if ($okRows.Count -ne $Swaps) { $problems.Add("$($okRows.Count) of $Swaps swaps completed") } }
    'controllost' { if ($okRows.Count -ne 2) { $problems.Add('the cut or the watch after it did not end in "state playing"') }; if (-not $Ana.b.reasons.ContainsKey('the control connection is lost')) { $problems.Add('b did not log a return for a lost control connection') } }
    'mismatch' { if ($Ana.b.never_touched -ne 1) { $problems.Add('b''s table was touched by a refused stream') } }
    'nolistener' { if ($Ana.a.never_touched -lt 1) { $problems.Add('a''s table was touched although nothing answered') } }
    'reaim' { if ($okRows.Count -ne $rows.Count) { $problems.Add('a re-aim did not complete') } }
    'plainwatch' { if ($okRows.Count -ne $rows.Count) { $problems.Add('a swap did not complete') } }
}
foreach ($m in $notes) { $problems.Add($m) }
if ($fail) { $problems.Add("the run failed: $fail") }
if (-not ($cleanOk -and $regOk -and $bamOk -and $vpmOk)) { $problems.Add("cleanup: instances clean=$cleanOk, registry restored=$regOk, BAMViewSettings restored=$bamOk, VPinMAME files restored=$vpmOk") }
[ordered]@{ scenario = $Scenario; table = $Table; swaps = $rows.Count; ok = $okRows.Count; instances = $Ana; problems = @($problems); maxPictureDiff = $MaxPictureDiff } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Run 'summary.json')
Log ("cleanup: instances clean={0}, registry restored={1}, BAMViewSettings restored={2}, VPinMAME files restored={3}" -f $cleanOk, $regOk, $bamOk, $vpmOk)
if ($problems.Count) { foreach ($p in $problems) { Log "PROBLEM: $p" }; Log 'MATCH TEST FAILED'; exit 1 }
Log 'MATCH TEST OK'; exit 0
