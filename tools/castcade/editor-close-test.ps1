<#
.SYNOPSIS
  The editor-close crash (I11.11.1): posts the command a click on the table view's close "x" posts (WM_COMMAND, SC_CLOSE)
  to the editor frame of a VPX that is playing a table, and says what happened. See README.md.

.EXAMPLE
  pwsh -NoProfile -File editor-close-test.ps1 -Exe C:\...\cchand_GL64.exe -Table "C:\...\Nudge Test and Calibration.vpx"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,       # not a VPinballX* name: the installed Castcade takes those for games
    [Parameter(Mandatory)][string]$Table,
    [string]$Ini,                             # default: generated from -Template, as match-test.ps1 does
    [string]$Template = 'C:\code\repos\castcade-vpx-deps\run\smoke.template.ini',
    [string]$OutDir,
    [int]$PlaySeconds = 6,
    [int]$AfterSeconds = 8
)
$ErrorActionPreference = 'Stop'
if ((Split-Path -Leaf $Exe) -match '^VPinballX') { Write-Host 'editor-close-test.ps1: -Exe must not be named VPinballX*'; exit 2 }
if (-not $OutDir) { $OutDir = Join-Path 'C:\code\repos\castcade-vpx-deps\run\match' ("{0}-editor-close" -f (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class EdWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr Find(uint want, string cls, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want) {
                var t = new StringBuilder(512); GetWindowText(h, t, 512);
                var c = new StringBuilder(256); GetClassName(h, c, 256);
                // (PowerShell passes $null as an empty string)
                if ((string.IsNullOrEmpty(cls) || c.ToString() == cls) && (string.IsNullOrEmpty(title) || t.ToString() == title)) found = h;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
function Log([string]$m) { $l = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m; Write-Host $l; Add-Content -LiteralPath (Join-Path $OutDir 'editor-close.log') -Value $l }
if (-not $Ini) {
    $Ini = Join-Path $OutDir 'play.ini'
    $set = @{ FullScreen = '0'; Width = '960'; Height = '540'; Display = '-1'; WindowPosX = '20'; WindowPosY = '60'; PlaySound = '0'; PlayMusic = '0'; CacheMode = '0' }
    $out = New-Object System.Collections.Generic.List[string]; $sec = ''; $done = @{}
    foreach ($l in [IO.File]::ReadAllLines($Template)) {
        if ($l -match '^\s*\[(.+?)\]\s*$') { if ($sec -eq 'Player') { foreach ($k in $set.Keys) { if (-not $done[$k]) { $out.Add("$k = $($set[$k])") } } }; $sec = $Matches[1]; $out.Add($l); continue }
        if ($sec -eq 'Player' -and $l -match '^\s*([^=;#]+?)\s*=' -and $set.ContainsKey($Matches[1])) { $out.Add("$($Matches[1]) = $($set[$Matches[1]])"); $done[$Matches[1]] = 1; continue }
        if ($sec -eq 'PlayerVR' -and $l -match '^\s*AskToTurnOn\s*=') { $out.Add('AskToTurnOn = 2'); continue }
        $out.Add($l)
    }
    [IO.File]::WriteAllLines($Ini, $out, (New-Object System.Text.UTF8Encoding($false)))
}
$psi = [Diagnostics.ProcessStartInfo]::new($Exe); $psi.UseShellExecute = $false; $psi.WorkingDirectory = $OutDir
foreach ($a in @('-Ini', $Ini, '/ExtMinimized', '-Play', $Table)) { $psi.ArgumentList.Add($a) }
foreach ($k in @($psi.Environment.Keys | Where-Object { $_ -like 'CASTCADE_*' })) { [void]$psi.Environment.Remove($k) }
$psi.Environment['CASTCADE_TEST_NOPAUSE'] = '1'
$p = [Diagnostics.Process]::Start($psi); $null = $p.Handle
Log "started $Exe, PID $($p.Id)"
$player = [IntPtr]::Zero; $t0 = Get-Date
while ($player -eq [IntPtr]::Zero -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 150) { Start-Sleep -Milliseconds 100; $player = [EdWin]::Find([uint32]$p.Id, $null, 'Visual Pinball Player') }
if ($player -eq [IntPtr]::Zero) { Log 'no player window'; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 2 }
Start-Sleep -Seconds $PlaySeconds
$frame = [EdWin]::Find([uint32]$p.Id, 'VPinball', $null)
if ($frame -eq [IntPtr]::Zero) { Log 'no editor frame window (class VPinball)'; Stop-Process -Id $p.Id -Force; exit 2 }
Log ("posting WM_COMMAND, wParam 0xF060 (SC_CLOSE) to the editor frame 0x{0:X} while the game runs" -f $frame.ToInt64())
[void][EdWin]::PostMessage($frame, 0x0111, [IntPtr]0xF060, [IntPtr]::Zero)
$t1 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t1).TotalSeconds -lt $AfterSeconds) { Start-Sleep -Milliseconds 100 }
if ($p.HasExited) {
    Log ("RESULT: the process ended {0:N1} s after the command, exit code {1} (0x{1:X8})" -f ((Get-Date) - $t1).TotalSeconds, $p.ExitCode)
    exit 1
}
$stillPlaying = [EdWin]::Find([uint32]$p.Id, $null, 'Visual Pinball Player') -ne [IntPtr]::Zero
Log "after $AfterSeconds s: the process runs, the player window $(if ($stillPlaying) { 'is still there' } else { 'is GONE' })"
# the normal way out must still work: close the player window (the table ends, and with /ExtMinimized -Play VPX exits)
[void][EdWin]::PostMessage($player, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $p.WaitForExit(25000)) {
    $frame = [EdWin]::Find([uint32]$p.Id, 'VPinball', $null)
    if ($frame -ne [IntPtr]::Zero) { [void][EdWin]::PostMessage($frame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $p.WaitForExit(10000)) { Log 'did not exit after WM_CLOSE; killing my own PID'; Stop-Process -Id $p.Id -Force; exit 1 }
}
Log ("RESULT: the game went on; closed normally afterwards, exit code {0}" -f $p.ExitCode)
if ($stillPlaying -and $p.ExitCode -eq 0) { exit 0 } else { exit 1 }
