# Reads a frame-hitch log (CASTCADE_FRAMELOG; Castcade Record.bat writes castcade-record-frames.log) and says, for every
# slow frame, which phase took the time and where the render thread was (its most-sampled stack); with -Journal, lines
# the script-heavy frames up against the Castcade hook's journal of that session.
#   pwsh tools\castcade\framelog-analyze.ps1 -Log <castcade-record-frames.log> [-Journal <%LOCALAPPDATA%\Castcade\journal\table.jsonl>]
param([string]$Log, [string]$Journal, [double]$MinMs = 45)
$lines = Get-Content $Log
$hdr = $lines | Where-Object { $_ -match 'probe start' } | Select-Object -First 1
$null = $hdr -match '^(\S+ \S+) probe start.*clock NowUsec (\d+)'
$wall0 = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null); $clk0 = [double]$Matches[2] / 1e6
$frames = New-Object System.Collections.Generic.List[object]
for ($i = 0; $i -lt $lines.Count; $i++) {
  $l = $lines[$i]
  if ($l -match ' frame (\d+): ([\d.]+) ms, at ([\d.]+)-([\d.]+) s; physics clock \+(\d+) ms \((\d+) steps\) \| (.*?) \| script events: (.*?) \| timers: (.*?) \| (\d+) stack samples') {
    $f = [ordered]@{ n = [int]$Matches[1]; ms = [double]$Matches[2]; t0 = [double]$Matches[3]; t1 = [double]$Matches[4]; physMs = [int]$Matches[5]; phases = $Matches[7]; events = $Matches[8]; timers = $Matches[9]; samples = [int]$Matches[10]; stacks = @() }
    $ph = @{}; foreach ($m in [regex]::Matches($f.phases, '(\w+) ([\d.]+)')) { $ph[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
    $f.ph = $ph
    $top = $ph.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
    $f.dom = $top.Key; $f.domMs = $top.Value
    $tt = [regex]::Matches($f.timers, '(\S+) x(\d+) ([\d.]+)') | ForEach-Object { [pscustomobject]@{ name = $_.Groups[1].Value; ms = [double]$_.Groups[3].Value } } | Sort-Object ms -Descending | Select-Object -First 1
    $f.topTimer = if ($tt) { "$($tt.name) $($tt.ms)" } else { '' }
    $j = $i + 1
    while ($j -lt $lines.Count -and $lines[$j] -match "^\s+frame $($f.n) stack (\d+)/(\d+): (.*)$") { $f.stacks += [pscustomobject]@{ c = [int]$Matches[1]; s = $Matches[3] }; $j++ }
    $f.sec = $f.t0 - $clk0
    $frames.Add([pscustomobject]$f)
  }
}
$sum = $lines | Where-Object { $_ -match ' summary: ' } | Select-Object -Last 1
"last summary line: " + ($sum -replace '^\S+ \S+ ', '')
$end = $lines | Where-Object { $_ -match 'probe end|faulted|unwind' } | Select-Object -Last 3
$end | ForEach-Object { "end: " + $_ }
"slow frames logged (>= 25 ms): $($frames.Count); span $([math]::Round(($frames[-1].sec),0)) s"
function Tag($f) {
  $s = ($f.stacks | Sort-Object c -Descending | Select-Object -First 1).s
  if (-not $s) { return '(no sample)' }
  $mods = [regex]::Matches($s, '([A-Za-z0-9_.\-]+)[!+]') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -notmatch '^(ntdll|KERNELBASE|KERNEL32|VPinballX_GL64_castcade|win32u|user32|USER32|GDI32|gdi32full)$' } | Select-Object -Unique
  $leaf = ($s -split ' < ')[0]
  return ($leaf -replace '\+0x[0-9a-f]+', '') + ' <- ' + (($mods | Select-Object -First 5) -join ' < ')
}
foreach ($f in $frames) { $f | Add-Member tag (Tag $f) }
"=== by dominant phase (frames >= 25 ms)"
$frames | Group-Object dom | Sort-Object { ($_.Group | Measure-Object ms -Sum).Sum } -Descending | ForEach-Object { "{0,-9} {1,4} frames  total {2,8:N0} ms  longest {3,7:N1} ms" -f $_.Name, $_.Count, ($_.Group | Measure-Object ms -Sum).Sum, ($_.Group | Measure-Object ms -Maximum).Maximum }
"=== frames >= $MinMs ms, by dominant phase and where the thread was (most-sampled stack)"
$frames | Where-Object { $_.ms -ge $MinMs } | Group-Object { $_.dom + ' | ' + $_.tag } | Sort-Object { ($_.Group | Measure-Object ms -Sum).Sum } -Descending | Select-Object -First 25 | ForEach-Object { "{0,3} frames  total {1,7:N0} ms  longest {2,6:N0}  {3}" -f $_.Count, ($_.Group | Measure-Object ms -Sum).Sum, ($_.Group | Measure-Object ms -Maximum).Maximum, $_.Name }
"=== the 25 longest frames"
$frames | Sort-Object ms -Descending | Select-Object -First 25 | ForEach-Object { "{0,7:N1} s  {1,6:N0} ms  {2,-8} {3,6:N0}  timer[{4}]  {5}" -f $_.sec, $_.ms, $_.dom, $_.domMs, $_.topTimer, $_.tag }
if ($Journal) {
  $all = Get-Content $Journal
  $last = ($all | Select-String -Pattern '"event":"table_loaded"' | Select-Object -Last 1).LineNumber
  $sess = $all[($last - 1)..($all.Count - 1)]
  $jt = $sess | ForEach-Object { if ($_ -match '"event":"([a-z_]+)","t":([0-9.]+)') { [pscustomobject]@{ ev = $Matches[1]; t = [double]$Matches[2] } } }
  $exitT = ($jt | Where-Object ev -eq 'table_exit' | Select-Object -Last 1).t
  $jEnd = (Get-Item $Journal).LastWriteTime
  $jT0 = $jEnd.AddSeconds(-$exitT)
  "=== journal: last session $($jt.Count) lines ($((($jt | Group-Object ev | ForEach-Object { $_.Name + '=' + $_.Count }) -join ' '))), T0 about $($jT0.ToString('HH:mm:ss.fff'))"
  $jsec = $jt | ForEach-Object { ($jT0.AddSeconds($_.t) - $wall0).TotalSeconds }
  $scriptFrames = $frames | Where-Object { $_.ph['script'] -ge 20 }
  "frames with 20 ms or more in script: $($scriptFrames.Count), total script ms $([math]::Round(($scriptFrames | ForEach-Object { $_.ph['script'] } | Measure-Object -Sum).Sum))"
  $hit = 0; $deltas = @()
  foreach ($f in $scriptFrames) { $near = $jsec | Where-Object { $_ -ge ($f.sec - 0.25) -and $_ -le ($f.sec + $f.ms / 1000 + 0.25) } | Sort-Object { [math]::Abs($_ - $f.sec) } | Select-Object -First 1; if ($null -ne $near) { $hit++; $deltas += ($near - $f.sec) } }
  "of those, a journal line within 0.25 s: $hit; offsets (journal - frame start) median $([math]::Round((($deltas | Sort-Object)[[int]($deltas.Count/2)]),3)) s"
  "script-heavy frames by top timer:"
  $scriptFrames | Group-Object { ($_.topTimer -split ' ')[0] } | Sort-Object Count -Descending | Select-Object -First 8 | ForEach-Object { "  {0,-22} {1,4} frames  script total {2,7:N0} ms  longest {3,6:N0}" -f $_.Name, $_.Count, ($_.Group | ForEach-Object { $_.ph['script'] } | Measure-Object -Sum).Sum, ($_.Group | ForEach-Object { $_.ph['script'] } | Measure-Object -Maximum).Maximum }
}
