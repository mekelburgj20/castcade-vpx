<#
Castcade replay: plays a recording to a watcher on this PC, as if the player were live.

A recording is made by a player started with CASTCADE_STREAM_RECORD ("Castcade Record.bat" does that). It holds
exactly what a watcher would have received from the first frame on, each message stamped with the time it was
sent. This script listens on 127.0.0.1 only (nothing outside this PC can connect, and Windows Firewall does not
ask), and to every watcher that connects it sends the recording from its first byte at the recorded pace: a
watcher that reconnects starts again from the beginning. Whatever the watcher sends is read and thrown away.

"Castcade Watch Recording.bat" starts it. To run it yourself (Windows PowerShell 5.1, nothing to install):
  powershell -NoProfile -ExecutionPolicy Bypass -File castcade-replay.ps1 -Recording <file.castrec> [-Connection fair] [-Loop]
then start a watcher with CASTCADE_STREAM=watch and CASTCADE_STREAM_ADDR=connect:127.0.0.1:47801.

-Connection puts a SYNTHETIC internet line between this script and the watcher: a fixed delay, random jitter
and now and then a stall, delivered in order as TCP does (a late message holds back the ones behind it). The
presets are made-up numbers picked to resemble kinds of lines. They are NOT measurements of anyone's line.
  perfect  nothing added (as on this PC)
  good     20 ms, jitter mostly under 5 ms (at most 15 ms), no stalls
  fair     45 ms, jitter mostly under 20 ms (at most 60 ms), a 0.15 to 0.3 s stall about every 30 s
  poor     90 ms, jitter mostly under 50 ms (at most 150 ms), a 0.25 to 0.6 s stall about every 8 s
A fixed delay on its own changes nothing you can see but how far behind the player you are: the watcher
measures it and waits CASTCADE_STREAM_DELAY_MS on top. Jitter and stalls beyond that delay make the ball stop
and jump. -LatencyMs, -JitterMs (average), -JitterMaxMs, -StallEverySec, -StallMinMs and -StallMaxMs replace
a preset's numbers; -Seed picks another random sequence (the same seed gives the same line every time).

-Loop: at the end, close the connection without the player's goodbye. The watcher reconnects (it retries every
2 s) and the game starts over. Without -Loop the replay ends after one full pass: the watcher is told that the
player closed the table and keeps showing the last picture.

-WatchPid <pid>: end when that process (the watcher) exits. -LogFile <file>: also append every line there.
-List <folder>: write one object per recording in the folder (newest first) and exit; -Presets: write the
presets and exit. The launcher uses both.
#>
param(
    [string]$Recording,
    [int]$Port = 47801,
    [ValidateSet('perfect', 'good', 'fair', 'poor')][string]$Connection = 'perfect',
    [double]$LatencyMs = -1,
    [double]$JitterMs = -1,
    [double]$JitterMaxMs = -1,
    [double]$StallEverySec = -1,
    [double]$StallMinMs = -1,
    [double]$StallMaxMs = -1,
    [int]$Seed = 1,
    [switch]$Loop,
    [int]$WatchPid = 0,
    [string]$LogFile,
    [string]$List,
    [switch]$Presets
)
$ErrorActionPreference = 'Stop'

$presetTable = [ordered]@{
    perfect = @{ LatencyMs = 0; JitterMs = 0; JitterMaxMs = 0; StallEverySec = 0; StallMinMs = 0; StallMaxMs = 0; Text = 'nothing added (as on this PC)' }
    good    = @{ LatencyMs = 20; JitterMs = 2; JitterMaxMs = 15; StallEverySec = 0; StallMinMs = 0; StallMaxMs = 0; Text = '20 ms, a little jitter, no stalls' }
    fair    = @{ LatencyMs = 45; JitterMs = 7; JitterMaxMs = 60; StallEverySec = 30; StallMinMs = 150; StallMaxMs = 300; Text = '45 ms, some jitter, a short stall about every 30 s' }
    poor    = @{ LatencyMs = 90; JitterMs = 18; JitterMaxMs = 150; StallEverySec = 8; StallMinMs = 250; StallMaxMs = 600; Text = '90 ms, heavy jitter, a stall of up to 0.6 s about every 8 s' }
}

$engineSource = @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace CastcadeReplay
{
    // A recording: the header, and an index of its messages (see src/castcade/CastcadeWire.h for the layout)
    public class Recording
    {
        public const int MaxFormat = 1;
        public string Path, Name;
        public long FileBytes;
        public int Format, Protocol, HeaderSize;
        public DateTime Start;
        public bool Closed;              // the recorder completed the header: VPX closed the table normally
        public uint PartCount;
        public ulong Hash;
        public string Table = "", Build = "";
        public byte[] Data;              // the whole file (Read with data)
        public int Count, Frames;
        public long[] Times, Offsets;    // per message: microseconds since the first; where its bytes start
        public int[] Lengths;
        public byte[] Types;
        public bool HasKeyframe, EndsWithBye;
        public string Problem;           // why the readable part ends early, or null
        public long DurationUs { get { return Count > 0 ? Times[Count - 1] : 0; } }

        static readonly byte[] Magic = { 0x43, 0x43, 0x53, 0x54, 0x52, 0x45, 0x43, 0x1A }; // "CCSTREC" 0x1A

        public static string Clock(long us)
        {
            long s = us / 1000000;
            return string.Format("{0}:{1:00}", s / 60, s % 60);
        }

        static void ReadFully(Stream s, byte[] b, int at, int n)
        {
            while (n > 0)
            {
                int got = s.Read(b, at, n);
                if (got <= 0) throw new InvalidDataException("could not be read to its end");
                at += got;
                n -= got;
            }
        }

        // data: keep every byte (to play it); otherwise only the index (to list it)
        public static Recording Read(string path, bool data)
        {
            Recording r = new Recording();
            r.Path = System.IO.Path.GetFullPath(path);
            r.Name = System.IO.Path.GetFileName(r.Path);
            // ReadWrite sharing: a recording still being written can be read too
            using (FileStream fs = new FileStream(r.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                r.FileBytes = fs.Length;
                if (fs.Length == 0)
                    throw new InvalidDataException("is empty: VPX closed or crashed before the recording's first second was written");
                byte[] head = new byte[(int)Math.Min(fs.Length, 16384)];
                ReadFully(fs, head, 0, head.Length);
                r.ParseHeader(head);
                if (data)
                {
                    if (fs.Length > int.MaxValue)
                        throw new InvalidDataException("is too large to play (over 2 GB)");
                    r.Data = new byte[fs.Length];
                    fs.Position = 0;
                    ReadFully(fs, r.Data, 0, r.Data.Length);
                    r.Index(null);
                }
                else
                    r.Index(fs);
            }
            if (r.Count == 0)
                throw new InvalidDataException(r.Problem != null ? r.Problem : "holds no messages");
            return r;
        }

        string Str(byte[] h, ref int p)
        {
            if (p + 2 > h.Length)
                throw new InvalidDataException("has a damaged header");
            int n = BitConverter.ToUInt16(h, p);
            p += 2;
            if (p + n > h.Length)
                throw new InvalidDataException("has a damaged header");
            string s = Encoding.UTF8.GetString(h, p, n);
            p += n;
            return s;
        }

        void ParseHeader(byte[] h)
        {
            if (h.Length < 16)
                throw new InvalidDataException("is not a Castcade recording");
            for (int i = 0; i < 8; i++)
                if (h[i] != Magic[i])
                    throw new InvalidDataException("is not a Castcade recording");
            Format = BitConverter.ToUInt16(h, 8);
            if (Format < 1 || Format > MaxFormat)
                throw new InvalidDataException("is a recording in format " + Format + ", which this script does not read (it reads format " + MaxFormat + "): use the castcade-replay.ps1 that came with the VPX build that made it");
            if (h.Length < 48)
                throw new InvalidDataException("is cut off inside its header");
            HeaderSize = BitConverter.ToUInt16(h, 10);
            Protocol = BitConverter.ToUInt16(h, 12);
            long ft = BitConverter.ToInt64(h, 16);
            try { Start = DateTime.FromFileTimeUtc(ft).ToLocalTime(); }
            catch (ArgumentOutOfRangeException) { throw new InvalidDataException("has a damaged header"); }
            Closed = BitConverter.ToUInt32(h, 32) != 0;
            PartCount = BitConverter.ToUInt32(h, 36);
            Hash = BitConverter.ToUInt64(h, 40);
            int p = 48;
            Table = Str(h, ref p);
            Build = Str(h, ref p);
            if (HeaderSize < p || HeaderSize > FileBytes)
                throw new InvalidDataException(HeaderSize > FileBytes ? "is cut off inside its header" : "has a damaged header");
        }

        void Index(FileStream fs)
        {
            List<long> times = new List<long>(), offsets = new List<long>();
            List<int> lengths = new List<int>();
            List<byte> types = new List<byte>();
            long pos = HeaderSize, end = FileBytes, prev = 0;
            byte[] c = new byte[18]; // the chunk's time and size, then the message's length, type and flags
            while (pos < end)
            {
                if (end - pos < 18)
                {
                    Problem = "cut off at " + Clock(prev) + " (the file ends inside a message)";
                    break;
                }
                if (fs != null)
                {
                    fs.Position = pos;
                    ReadFully(fs, c, 0, 18);
                }
                else
                    Buffer.BlockCopy(Data, (int)pos, c, 0, 18);
                long t = BitConverter.ToInt64(c, 0);
                long n = BitConverter.ToUInt32(c, 8);
                long len = BitConverter.ToUInt32(c, 12);
                byte type = c[16], flags = c[17];
                if (n < 6 || len != n - 4 || type < 1 || type > 4 || flags > 1 || t < prev || t > 7L * 24 * 3600 * 1000000)
                {
                    Problem = times.Count == 0 ? "is not a Castcade recording (its first message is damaged)" : "damaged after " + Clock(prev) + " (message " + (times.Count + 1) + ")";
                    break;
                }
                if (pos + 12 + n > end)
                {
                    Problem = "cut off at " + Clock(prev) + " (the file ends inside a message)";
                    break;
                }
                if (times.Count == 0 && type != 1)
                {
                    Problem = "is not a Castcade recording (it does not start with the player's HELLO)";
                    break;
                }
                times.Add(t);
                offsets.Add(pos + 12);
                lengths.Add((int)n);
                types.Add(type);
                prev = t;
                pos += 12 + n;
            }
            Count = times.Count;
            Times = times.ToArray();
            Offsets = offsets.ToArray();
            Lengths = lengths.ToArray();
            Types = types.ToArray();
            for (int i = 0; i < Count; i++)
            {
                if (Types[i] == 3) Frames++;
                if (Types[i] == 2) HasKeyframe = true;
            }
            EndsWithBye = Count > 0 && Types[Count - 1] == 4;
        }
    }

    // A synthetic line: when each message reaches the watcher, in order
    public class LineModel
    {
        public string Name = "perfect";
        public double LatencyMs, JitterMs, JitterMaxMs, StallEverySec, StallMinMs, StallMaxMs;
        public bool IsPerfect { get { return LatencyMs <= 0 && JitterMs <= 0 && (StallEverySec <= 0 || StallMaxMs <= 0); } }

        static double Exp(Random r, double mean) { return -mean * Math.Log(1.0 - r.NextDouble()); }

        // Delivery time of each of the first n messages, microseconds from the start of the pass
        public long[] Schedule(Recording rec, int n, int seed, out string summary)
        {
            long[] d = new long[n];
            Random rng = new Random(seed);
            double endMs = (n > 0 ? rec.Times[n - 1] / 1000.0 : 0) + 60000;
            List<double[]> stalls = new List<double[]>();
            if (StallEverySec > 0 && StallMaxMs > 0)
            {
                double t = Exp(rng, StallEverySec * 1000);
                while (t < endMs)
                {
                    double len = StallMinMs + rng.NextDouble() * Math.Max(0, StallMaxMs - StallMinMs);
                    stalls.Add(new double[] { t, t + len });
                    t += len + Exp(rng, StallEverySec * 1000);
                }
            }
            long prev = 0;
            double sumExtra = 0, maxExtra = 0;
            int held = 0, stallsHit = 0, lastStall = -1;
            for (int i = 0; i < n; i++)
            {
                double s = rec.Times[i] / 1000.0;
                double c = s + LatencyMs;
                if (JitterMs > 0)
                    c += Math.Min(JitterMaxMs > 0 ? JitterMaxMs : double.MaxValue, Exp(rng, JitterMs));
                for (int k = 0; k < stalls.Count; k++)
                    if (c >= stalls[k][0] && c < stalls[k][1])
                    {
                        c = stalls[k][1]; // the line delivers nothing until the stall ends
                        if (k != lastStall) { stallsHit++; lastStall = k; }
                        break;
                    }
                long us = (long)Math.Round(c * 1000);
                if (us < prev)
                {
                    us = prev; // TCP is ordered: held back behind a late one
                    held++;
                }
                d[i] = us;
                prev = us;
                double extra = (us - rec.Times[i]) / 1000.0;
                sumExtra += extra;
                if (extra > maxExtra) maxExtra = extra;
            }
            summary = IsPerfect ? null : string.Format("line ({0}): added delay {1:0.0} ms on average, worst {2:0} ms; {3} stalls; {4} messages held back behind a later one",
                Name, n > 0 ? sumExtra / n : 0, maxExtra, stallsHit, held);
            return d;
        }
    }

    public class Replayer
    {
        [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint period);
        [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint period);
        [DllImport("kernel32.dll")] static extern void GetSystemTimePreciseAsFileTime(out long fileTime);

        // For the log: wall clock to the millisecond (DateTime.Now in .NET Framework follows the 15.6 ms tick)
        static string PreciseNow()
        {
            long ft;
            GetSystemTimePreciseAsFileTime(out ft);
            return DateTime.FromFileTimeUtc(ft).ToLocalTime().ToString("yyyy-MM-ddTHH:mm:ss.fff");
        }

        readonly Recording rec;
        readonly LineModel line;
        readonly bool loop;
        readonly int seed;
        volatile TcpListener listener;
        Thread thread;
        volatile bool stop, playing, finished, failed;
        volatile int pass;
        long position;
        public readonly ConcurrentQueue<string> Messages = new ConcurrentQueue<string>();
        public int Port;
        public int Pass { get { return pass; } }
        public bool Playing { get { return playing; } }
        public bool Finished { get { return finished; } }
        public bool Failed { get { return failed; } }
        public long PositionUs { get { return Interlocked.Read(ref position); } }

        public Replayer(Recording rec, LineModel line, bool loop, int seed)
        {
            this.rec = rec;
            this.line = line;
            this.loop = loop;
            this.seed = seed;
        }

        void Say(string s) { Messages.Enqueue(s); }

        // Loopback only: nothing outside this PC can connect and Windows Firewall does not ask
        public void Start(int port)
        {
            TcpListener l = new TcpListener(IPAddress.Loopback, port);
            l.ExclusiveAddressUse = true;
            l.Start(4);
            Port = ((IPEndPoint)l.LocalEndpoint).Port;
            listener = l;
            thread = new Thread(Run);
            thread.IsBackground = true;
            thread.Name = "castcade replay";
            thread.Priority = ThreadPriority.AboveNormal;
            thread.Start();
        }

        public void Stop()
        {
            stop = true;
            if (thread != null)
                thread.Join(5000);
            TcpListener l = listener;
            listener = null;
            if (l != null)
                try { l.Stop(); } catch (SocketException) { }
        }

        void Run()
        {
            timeBeginPeriod(1); // 1 ms sleeps instead of 15.6 ms; the last part of each wait is spun
            try
            {
                Socket next = null;
                while (!stop)
                {
                    if (next == null)
                    {
                        TcpListener l = listener;
                        if (l == null || !l.Pending())
                        {
                            Thread.Sleep(5);
                            continue;
                        }
                        next = l.AcceptSocket();
                    }
                    Socket s = next;
                    next = PlayPass(s);
                }
                if (next != null)
                    Close(next);
            }
            catch (Exception e)
            {
                Say("The replay stopped: " + e.Message);
                failed = true;
            }
            finally
            {
                timeEndPeriod(1);
            }
        }

        static void Close(Socket s)
        {
            try { s.Shutdown(SocketShutdown.Both); } catch (SocketException) { } catch (ObjectDisposedException) { }
            s.Close();
        }

        // Reads and discards what the watcher sent. False once the watcher has closed the connection.
        static bool Drain(Socket s, byte[] sink)
        {
            try
            {
                while (s.Available > 0)
                    s.Receive(sink, Math.Min(sink.Length, s.Available), SocketFlags.None);
                return !(s.Poll(0, SelectMode.SelectRead) && s.Available == 0);
            }
            catch (SocketException) { return false; }
            catch (ObjectDisposedException) { return false; }
        }

        // Plays the recording once on s. Returns a connection that replaced s, or null.
        Socket PlayPass(Socket s)
        {
            s.NoDelay = true;
            s.SendTimeout = 15000;
            string who = s.RemoteEndPoint.ToString();
            int n = rec.Count;
            if (loop && rec.EndsWithBye)
                n--; // the goodbye would tell the watcher the table closed
            int number = ++pass;
            string summary;
            long[] due = line.Schedule(rec, n, seed + number - 1, out summary);
            long[] late = new long[n];
            byte[] sink = new byte[65536];
            double usPerTick = 1e6 / Stopwatch.Frequency;
            TimeSpan cpu0 = Process.GetCurrentProcess().TotalProcessorTime;
            Interlocked.Exchange(ref position, 0);
            Say(number == 1 ? "Watcher connected (" + who + "): playing the recording from the start."
                            : "Watcher connected (" + who + "): playing the recording from the start (pass " + number + ").");
            Say("#pass " + number + " start " + PreciseNow());
            Stopwatch sw = Stopwatch.StartNew();
            playing = true;
            Socket replacement = null;
            string ended = null;
            int i = 0, frames = 0;
            for (; i < n && !stop; i++)
            {
                long dueUs = due[i];
                for (;;)
                {
                    long remaining = dueUs - (long)(sw.ElapsedTicks * usPerTick);
                    if (remaining <= 0)
                        break;
                    if (remaining > 2000)
                    {
                        if (!Drain(s, sink)) { ended = "left"; break; }
                        TcpListener l = listener;
                        if (l != null && l.Pending()) { replacement = l.AcceptSocket(); ended = "replaced"; break; }
                        if (stop) break;
                        Thread.Sleep(1);
                    }
                    else if (remaining > 200)
                        Thread.Sleep(0);
                    else
                        Thread.SpinWait(20);
                }
                if (ended != null || stop)
                    break;
                late[i] = (long)(sw.ElapsedTicks * usPerTick) - dueUs;
                try
                {
                    int off = (int)rec.Offsets[i], len = rec.Lengths[i];
                    while (len > 0)
                    {
                        int sent = s.Send(rec.Data, off, len, SocketFlags.None);
                        off += sent;
                        len -= sent;
                    }
                }
                catch (SocketException) { ended = "left"; break; }
                catch (ObjectDisposedException) { ended = "left"; break; }
                if (rec.Types[i] == 3) frames++;
                Interlocked.Exchange(ref position, rec.Times[i]);
            }
            playing = false;
            double wallS = sw.Elapsed.TotalSeconds;
            double cpu = (Process.GetCurrentProcess().TotalProcessorTime - cpu0).TotalSeconds;
            long at = PositionUs;
            string where = Recording.Clock(at) + " (" + (rec.DurationUs > 0 ? (int)(100.0 * at / rec.DurationUs) : 0) + "%)";
            if (ended == null && !stop && i == n)
            {
                if (loop)
                {
                    Say("End of pass " + number + " at " + Recording.Clock(at) + " (" + frames + " frames sent): closing the connection without the player's goodbye, so the watcher reconnects and the game starts over.");
                    Close(s);
                }
                else
                {
                    Say("End of the recording at " + Recording.Clock(at) + " (" + frames + " frames sent). " + (rec.EndsWithBye
                        ? "The watcher was told that the player closed the table; it keeps showing the last picture."
                        : "The recording has no goodbye at its end (it was cut off), so the connection is simply closed."));
                    // Let the watcher read the goodbye and close its end first
                    try
                    {
                        s.Shutdown(SocketShutdown.Send);
                        Stopwatch w = Stopwatch.StartNew();
                        while (w.ElapsedMilliseconds < 2000 && Drain(s, sink))
                            Thread.Sleep(10);
                    }
                    catch (SocketException) { }
                    s.Close();
                    // One pass only: a watcher that reconnected now would see the game start over
                    TcpListener l = listener;
                    listener = null;
                    if (l != null)
                        l.Stop();
                    finished = true;
                }
            }
            else
            {
                if (ended == "replaced")
                    Say("A new connection replaces the current one at " + where + ".");
                else if (ended == "left")
                    Say("The watcher left at " + where + ".");
                Close(s);
            }
            Say("#pass " + number + " end " + PreciseNow() + " frames " + frames + " messages " + i);
            if (i > 0)
            {
                long[] sorted = new long[i];
                Array.Copy(late, sorted, i);
                Array.Sort(sorted);
                double sum = 0;
                int over2 = 0;
                for (int k = 0; k < i; k++)
                {
                    sum += sorted[k];
                    if (sorted[k] > 2000) over2++;
                }
                Say(string.Format("  pacing: {0} messages sent {1:0.000} ms after their time on average, 99% within {2:0.000} ms, worst {3:0.000} ms, {4} more than 2 ms late; CPU {5:0}% of one core",
                    i, sum / i / 1000.0, sorted[Math.Min(i - 1, (int)(i * 0.99))] / 1000.0, sorted[i - 1] / 1000.0, over2, wallS > 0 ? 100.0 * cpu / wallS : 0));
                if (summary != null)
                    Say("  " + summary);
            }
            return replacement;
        }
    }
}
'@

function Load-Engine {
    if (-not ('CastcadeReplay.Replayer' -as [type])) {
        try { Add-Type -TypeDefinition $engineSource -Language CSharp }
        catch { throw "Windows PowerShell could not prepare the replay engine: $($_.Exception.Message)" }
    }
}

if ($Presets) {
    foreach ($k in $presetTable.Keys) { [pscustomobject]@{ Name = $k; Text = $presetTable[$k].Text } }
    return
}

# -List: one object per recording in a folder, newest first (the launcher picks from these)
if ($List) {
    Load-Engine
    if (-not (Test-Path -LiteralPath $List -PathType Container)) { return }
    $items = foreach ($f in Get-ChildItem -LiteralPath $List -Filter '*.castrec' -File) {
        try {
            $r = [CastcadeReplay.Recording]::Read($f.FullName, $false)
            [pscustomobject]@{ Path = $f.FullName; Name = $f.Name; Table = $r.Table; Start = $r.Start; Seconds = $r.DurationUs / 1e6; Frames = $r.Frames; Bytes = $f.Length; Closed = $r.Closed; Problem = $r.Problem; Playable = $r.HasKeyframe }
        } catch {
            $msg = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
            [pscustomobject]@{ Path = $f.FullName; Name = $f.Name; Table = ''; Start = $f.LastWriteTime; Seconds = 0; Frames = 0; Bytes = $f.Length; Closed = $false; Problem = $msg; Playable = $false }
        }
    }
    $items | Sort-Object Start -Descending
    return
}

$logPath = $null
if ($LogFile) { $logPath = [IO.Path]::GetFullPath($LogFile) }
function Say([string]$text) {
    if (-not $text.StartsWith('#')) { Write-Host $text }
    if ($logPath) { try { [IO.File]::AppendAllText($logPath, ('{0:yyyy-MM-dd HH:mm:ss.fff} {1}' -f (Get-Date), $text) + "`r`n") } catch { } }
}
function Clock([double]$us) { [CastcadeReplay.Recording]::Clock([long]$us) }

try {
    if (-not $Recording) { Say 'Which recording? Give its path with -Recording.'; exit 1 }
    if (-not (Test-Path -LiteralPath $Recording -PathType Leaf)) { Say "There is no recording at $Recording"; exit 1 }
    Load-Engine
    try { $rec = [CastcadeReplay.Recording]::Read((Resolve-Path -LiteralPath $Recording).ProviderPath, $true) }
    catch {
        $msg = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        Say "$(Split-Path -Leaf $Recording) $msg."
        exit 1
    }
    if (-not $rec.HasKeyframe) { Say "$($rec.Name) cannot be played: it ends before the player's first full picture (the keyframe)."; exit 1 }

    $p = $presetTable[$Connection]
    $line = New-Object CastcadeReplay.LineModel
    $custom = $false
    foreach ($k in 'LatencyMs', 'JitterMs', 'JitterMaxMs', 'StallEverySec', 'StallMinMs', 'StallMaxMs') {
        $v = Get-Variable -Name $k -ValueOnly
        if ($v -ge 0) { $line.$k = $v; $custom = $true } else { $line.$k = $p[$k] }
    }
    $line.Name = if ($custom) { "custom, from $Connection" } else { $Connection }
    $lineText = if ($custom) { ('{0:0} ms, jitter {1:0} ms on average (at most {2:0}), stalls of {3:0} to {4:0} ms about every {5:0} s' -f $line.LatencyMs, $line.JitterMs, $line.JitterMaxMs, $line.StallMinMs, $line.StallMaxMs, $line.StallEverySec) } else { $p.Text }

    Say ''
    Say 'Castcade replay'
    Say "  Recording:  $($rec.Name)"
    Say "  Table:      $($rec.Table)"
    Say ('  Recorded:   {0:yyyy-MM-dd HH:mm}, {1} long, {2:N0} frames, {3:N1} MB' -f $rec.Start, (Clock $rec.DurationUs), $rec.Frames, ($rec.FileBytes / 1MB))
    Say "  Connection: $($line.Name): $lineText"
    if (-not $line.IsPerfect) { Say '              (synthetic: made-up numbers, not a measurement of anyone''s line)' }
    Say ('  Loop:       {0}' -f $(if ($Loop) { 'on: at the end the game starts over' } else { 'off: one pass' }))
    if ($rec.Problem) { Say "  Note:       this recording is $($rec.Problem): it plays up to there." }
    elseif (-not $rec.Closed) { Say '  Note:       VPX did not close this recording normally (crash?); every message in it is complete.' }

    $engine = New-Object CastcadeReplay.Replayer($rec, $line, [bool]$Loop, $Seed)
    try { $engine.Start($Port) }
    catch {
        $msg = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        Say "Cannot listen on 127.0.0.1:${Port} ($msg). Is another replay still running? Close it, or pass -Port with another number."
        exit 1
    }
    $watcher = $null
    if ($WatchPid) {
        try { $watcher = [Diagnostics.Process]::GetProcessById($WatchPid) } catch { Say "The watcher (process $WatchPid) is not running."; $engine.Stop(); exit 1 }
    }
    Say "Waiting for the watcher on 127.0.0.1:$($engine.Port) ..."
    $nextProgress = [DateTime]::MaxValue
    $code = 0
    $toldEnd = $false
    try {
        while ($true) {
            $m = $null
            while ($engine.Messages.TryDequeue([ref]$m)) { Say $m }
            if ($engine.Failed) { $code = 1; break }
            if ($engine.Finished -and $watcher -and -not $toldEnd) { Say 'When you are done, close the table and then VPX: this window closes with it.'; $toldEnd = $true }
            if ($engine.Playing) {
                if ($nextProgress -eq [DateTime]::MaxValue) { $nextProgress = (Get-Date).AddSeconds(5) }
                elseif ((Get-Date) -ge $nextProgress) {
                    $pos = $engine.PositionUs
                    Say ('  {0} / {1}  {2,3}%' -f (Clock $pos), (Clock $rec.DurationUs), [int](100.0 * $pos / [math]::Max(1, $rec.DurationUs)))
                    $nextProgress = (Get-Date).AddSeconds(5)
                }
            } else { $nextProgress = [DateTime]::MaxValue }
            if ($watcher -and $watcher.HasExited) { Say 'The watcher closed: the replay ends.'; break }
            if ($engine.Finished -and -not $watcher) { break }
            Start-Sleep -Milliseconds 200
        }
    } finally {
        $engine.Stop()
        $m = $null
        while ($engine.Messages.TryDequeue([ref]$m)) { Say $m }
    }
    exit $code
} catch {
    Say "The replay stopped: $($_.Exception.Message)"
    exit 1
}
