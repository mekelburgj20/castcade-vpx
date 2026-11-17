# How long does a plain append to an already-open file take on this PC?
# Two writers for -Seconds: 61 bytes every 20 ms (like the Castcade hook's journal) and 60 KB every second
# (like the stream recorder), each through one held handle, timed per write. Temp files in -Dir, removed at the end.
#   pwsh tools\castcade\write-baseline.ps1 -Seconds 180
# On the owner's PC, quiet, 2026-11-16: 14 of 5,519 small writes took 50 ms or more, the longest 2.3 s (WORK C16.2).
param([string]$Dir = (Join-Path $env:TEMP 'castcade-write-baseline'), [int]$Seconds = 180)
New-Item -ItemType Directory -Force $Dir | Out-Null
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Diagnostics; using System.Threading; using System.Collections.Generic;
public static class CastcadeWriteBaseline {
  public static string Run(string dir, int seconds) {
    var small = new List<double>(); var big = new List<double>();
    byte[] line = new byte[61]; for (int i = 0; i < 60; i++) line[i] = (byte)'x'; line[60] = (byte)'\n';
    byte[] chunk = new byte[60000];
    var t = new Thread(() => {
      using (var f = new FileStream(Path.Combine(dir, "recorder-like.bin"), FileMode.Create, FileAccess.Write, FileShare.Read, 1)) {
        var sw = Stopwatch.StartNew(); var end = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < end) { sw.Restart(); f.Write(chunk, 0, chunk.Length); f.Flush(); big.Add(sw.Elapsed.TotalMilliseconds); Thread.Sleep(1000); }
      }
    });
    t.Start();
    using (var f = new FileStream(Path.Combine(dir, "journal-like.jsonl"), FileMode.Create, FileAccess.Write, FileShare.ReadWrite, 1)) {
      f.Write(new byte[400000], 0, 400000); f.Flush();
      var sw = Stopwatch.StartNew(); var end = DateTime.UtcNow.AddSeconds(seconds);
      while (DateTime.UtcNow < end) { sw.Restart(); f.Write(line, 0, line.Length); f.Flush(); small.Add(sw.Elapsed.TotalMilliseconds); Thread.Sleep(20); }
    }
    t.Join();
    Func<List<double>, string> s = l => { l.Sort(); int n = l.Count; int a = 0, b = 0, c = 0; foreach (var x in l) { if (x >= 5) a++; if (x >= 50) b++; if (x >= 200) c++; } return string.Format("{0} writes, median {1:F3} ms, p99 {2:F3} ms, >=5 ms {3}, >=50 ms {4}, >=200 ms {5}, longest {6:F1} ms", n, l[n/2], l[(int)(n*0.99)], a, b, c, l[n-1]); };
    return "journal-like (61 B every 20 ms, held handle): " + s(small) + "\nrecorder-like (60 KB every 1 s, held handle): " + s(big);
  }
}
'@
"folder: $Dir, $Seconds s"
[CastcadeWriteBaseline]::Run($Dir, $Seconds)
Remove-Item $Dir -Recurse -Force
