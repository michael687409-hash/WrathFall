using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace AsurasWrath.Launcher
{
    internal static class FrameReport
    {
        internal const string Command = "--frame-report";
        private const long UnitsPerMs = 10000;
        private const string TraceHeader = "stage\tid\trelated_id\tbegin_qpc_100ns\tend_qpc_100ns\tvk_result";
        private const string CaptureHeader = "unix_ms,segment,compositor_qpc_100ns,received_qpc_100ns";
        private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

        internal static int Run(Project project, string[] args)
        {
            if (args.Length > 2) throw new ArgumentException("Use --frame-report with an optional collector session folder.");
            bool open = args.Length == 2 && args[1] == "--open";
            string session = args.Length == 2 && !open ? Path.GetFullPath(args[1]) : LatestSession(project);
            if (session == null || !File.Exists(Path.Combine(session, "session.txt")))
                throw new FileNotFoundException("No collector session was found. Select an existing diagnostics session folder.");

            string report = Path.Combine(session, "frame-report.txt");
            string contents = Generate(project.Logs, session);
            GameLaunchInputs.AtomicWrite(report, pending => File.WriteAllText(pending, contents));
            Console.WriteLine("Frame report saved to " + report);
            if (open) Process.Start(new ProcessStartInfo(report) { UseShellExecute = true });
            return 0;
        }

        private static string LatestSession(Project project)
        {
            string diagnostics = Path.Combine(project.Output, "diagnostics");
            return Directory.Exists(diagnostics) ? Directory.EnumerateDirectories(diagnostics)
                .OrderByDescending(Path.GetFileName, StringComparer.Ordinal)
                .FirstOrDefault(dir => File.Exists(Path.Combine(dir, "session.txt"))) : null;
        }

        private static string Generate(string logs, string session)
        {
            var metadata = ReadProperties(Path.Combine(session, "session.txt"));
            string traceName = Get(metadata, "vulkan_frame_trace");
            var traceLabel = Regex.Match(traceName, @"^vulkan-frame-([0-9]+)-[0-9]+\.tsv$");
            string traceFile = null;
            if (traceLabel.Success && traceLabel.Groups[1].Value == Get(metadata, "pid"))
            {
                traceFile = new[] { Path.Combine(session, traceName), Path.Combine(logs, traceName) }
                    .FirstOrDefault(File.Exists);
            }

            var captures = ReadRows(Path.Combine(session, "capture-timing.csv"), CaptureHeader, ',')
                .Select(ParseCapture).Where(row => row != null).OrderBy(row => row.Qpc).ToArray();
            var trace = ReadRows(traceFile, TraceHeader, '\t').Select(ParseSpan).Where(span => span != null).ToArray();
            var presents = trace.Where(span => span.Stage == "paint-present").OrderBy(span => span.End).ToArray();
            var sources = trace.Where(span => span.Stage == "paint-source" && span.Id.HasValue)
                .OrderByDescending(span => span.Begin).ToLookup(span => span.Id.Value);
            var ready = trace.Where(span => span.Stage == "guest-ready" && span.Related.HasValue)
                .OrderByDescending(span => span.Begin).ToLookup(span => span.Related.Value);
            var submissions = trace.Where(span => span.Stage == "guest-submit" && span.Id.HasValue)
                .OrderByDescending(span => span.End).ToLookup(span => span.Id.Value);

            var report = new StringBuilder();
            report.AppendLine("WrathFall frame report");
            report.AppendLine("Session " + Path.GetFileName(session));
            report.AppendLine("Renderer " + Get(metadata, "renderer") + " | Upscaler " + Get(metadata, "upscaler"));
            report.AppendLine("Compositor captures " + captures.Length + " | Vulkan spans " + trace.Length +
                " | Present calls " + presents.Length);
            report.AppendLine(trace.Length == 0 ? "No usable Vulkan frame trace was recorded for this session." :
                "Vulkan trace " + traceFile);
            report.AppendLine("Vulkan durations are host-side timings. Compositor captures and present calls are aligned by time.");

            var marks = Directory.EnumerateDirectories(session).Select(ReadMark).Where(mark => mark != null)
                .OrderBy(mark => mark.UnixMs).ThenBy(mark => mark.Directory, StringComparer.Ordinal).ToArray();
            if (marks.Length == 0) report.AppendLine("No L marks or automatic frame-pacing events were saved.");

            foreach (var mark in marks)
            {
                report.AppendLine().AppendLine(Path.GetFileName(mark.Directory) + " | " +
                    (mark.Reason == "manual" ? "L mark" : mark.Reason));
                report.AppendLine("Mark UTC " + DateTimeOffset.FromUnixTimeMilliseconds(mark.UnixMs).ToString("O", Invariant));
                if (mark.Observed != "" || mark.Baseline != "")
                    report.AppendLine("Collector observed " + mark.Observed + " ms | baseline " + mark.Baseline + " ms");

                long qpc = mark.Qpc;
                string origin = "recorded QPC";
                if (qpc == 0 && captures.Length != 0)
                {
                    var exact = mark.Reason == "manual" ? null : captures.FirstOrDefault(row => row.UnixMs == mark.UnixMs);
                    long epochOffset = captures[0].UnixMs - captures[0].Qpc / UnitsPerMs;
                    qpc = exact == null ? (mark.UnixMs - epochOffset) * UnitsPerMs : exact.Qpc;
                    origin = exact == null ? "estimated from collector clock mapping" : "matched compositor capture";
                }
                if (qpc <= 0)
                {
                    report.AppendLine("Shared clock unavailable for this mark.");
                    continue;
                }

                int followingIndex = LowerBound(captures, qpc);
                var before = followingIndex == 0 ? null : captures[followingIndex - 1];
                var after = followingIndex == captures.Length ? null : captures[followingIndex];
                report.AppendLine("Mark QPC (100 ns) " + qpc.ToString(Invariant) + " (" + origin + ")");
                if (before != null && after != null && before.Segment == after.Segment)
                    report.AppendLine("Compositor gap around mark " + Ms(after.Qpc - before.Qpc) +
                        " ms (" + Offset(before.Qpc, qpc) + " to " + Offset(after.Qpc, qpc) + ")");
                else report.AppendLine("No continuous compositor interval around this mark.");
                if (after != null && after.Qpc - qpc <= 150 * UnitsPerMs && after.Received >= after.Qpc)
                    report.AppendLine("Collector received the next capture " + Ms(after.Received - after.Qpc) +
                        " ms after the compositor timestamp.");

                long start = qpc - (mark.Reason == "possible-fps-drop" ? 500 : 250) * UnitsPerMs;
                long end = qpc + 150 * UnitsPerMs;
                if (mark.Reason == "possible-frame-stutter" && before != null && after != null &&
                    before.Segment == after.Segment && after.Qpc == qpc)
                {
                    start = before.Qpc;
                    end = qpc;
                }

                AppendPresent(report, "Last present at or before mark", presents.LastOrDefault(span =>
                    span.End >= start - 150 * UnitsPerMs && span.End <= qpc), qpc, sources, ready, submissions);
                AppendPresent(report, "First present after mark", presents.FirstOrDefault(span =>
                    span.End > qpc && span.End <= end + 150 * UnitsPerMs), qpc, sources, ready, submissions);
                var stages = trace.Where(span => span.End >= start && span.Begin <= end && span.End > span.Begin)
                    .GroupBy(span => span.Stage).Select(group => group.OrderByDescending(span => span.End - span.Begin).First())
                    .OrderByDescending(span => span.End - span.Begin).ToArray();
                if (stages.Length == 0) report.AppendLine("No Vulkan timing spans overlapped this event window.");
                else
                {
                    report.AppendLine("Longest span by stage from " + Offset(start, qpc) + " to " + Offset(end, qpc));
                    foreach (var span in stages)
                        report.AppendLine("  " + span.Stage + " | " + Ms(span.End - span.Begin) + " ms | " +
                            Offset(span.Begin, qpc) + " to " + Offset(span.End, qpc) + Identifiers(span));
                }
            }
            return report.ToString();
        }

        private static void AppendPresent(StringBuilder report, string label, Span present, long qpc,
            ILookup<long, Span> sources, ILookup<long, Span> ready, ILookup<long, Span> submissions)
        {
            if (present == null) return;
            // Join through the recorded mailbox serial, never by guessing the guest frame from time.
            var source = present.Id.HasValue ? sources[present.Id.Value]
                .FirstOrDefault(span => span.Begin <= present.Begin) : null;
            var guest = source == null || !source.Related.HasValue ? null : ready[source.Related.Value]
                .FirstOrDefault(span => span.Begin <= source.Begin);
            bool joined = guest != null && guest.Id.HasValue;
            report.AppendLine(label + " at " + Offset(present.End, qpc) + " | " +
                Ms(present.End - present.Begin) + " ms in present" + Identifiers(present) +
                (joined ? " | guest frame " + guest.Id.Value.ToString(Invariant) : " | guest frame unavailable"));
            var submit = joined ? submissions[guest.Id.Value].FirstOrDefault(span => span.End <= guest.Begin) : null;
            if (submit != null)
                report.AppendLine("  Guest queue submit ended at " + Offset(submit.End, qpc) + " | " +
                    Ms(submit.End - submit.Begin) + " ms in submit" + Identifiers(submit));
        }

        private static string Identifiers(Span span)
        {
            return (span.Id.HasValue ? " | id " + span.Id.Value.ToString(Invariant) : "") +
                (span.Related.HasValue ? " | related " + span.Related.Value.ToString(Invariant) : "") +
                (span.Result == "" ? "" : " | VkResult " + span.Result);
        }

        private static int LowerBound(Capture[] captures, long qpc)
        {
            int low = 0, high = captures.Length;
            while (low < high)
            {
                int middle = low + (high - low) / 2;
                if (captures[middle].Qpc < qpc) low = middle + 1;
                else high = middle;
            }
            return low;
        }

        private static string Ms(long qpc) { return (qpc / (double)UnitsPerMs).ToString("0.000", Invariant); }
        private static string Offset(long time, long mark)
        {
            return (time >= mark ? "+" : "") + Ms(time - mark) + " ms";
        }

        private static IEnumerable<string[]> ReadRows(string path, string header, char delimiter)
        {
            if (!File.Exists(path)) yield break;
            using (var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new StreamReader(stream))
            {
                if (reader.ReadLine() != header) yield break;
                string line;
                while ((line = reader.ReadLine()) != null) yield return line.Split(delimiter);
            }
        }

        private static Dictionary<string, string> ReadProperties(string path)
        {
            var settings = new Dictionary<string, string>(StringComparer.Ordinal);
            if (!File.Exists(path)) return settings;
            foreach (string line in File.ReadLines(path))
            {
                int split = line.IndexOf('=');
                if (split > 0) settings[line.Substring(0, split)] = line.Substring(split + 1);
            }
            return settings;
        }

        private static string Get(Dictionary<string, string> settings, string key)
        {
            string value;
            return settings.TryGetValue(key, out value) ? value : "";
        }

        private static long? Number(string value)
        {
            long number;
            return long.TryParse(value, NumberStyles.Integer, Invariant, out number) ? (long?)number : null;
        }

        private static Capture ParseCapture(string[] row)
        {
            if (row.Length != 4) return null;
            long unix = Number(row[0]).GetValueOrDefault(), qpc = Number(row[2]).GetValueOrDefault();
            return unix > 0 && qpc > 0 ? new Capture { UnixMs = unix, Qpc = qpc, Segment = row[1],
                Received = Number(row[3]).GetValueOrDefault() } : null;
        }

        private static Span ParseSpan(string[] row)
        {
            if (row.Length != 6 || row[0] == "") return null;
            long begin = Number(row[3]).GetValueOrDefault(), end = Number(row[4]).GetValueOrDefault();
            return begin <= 0 || end < begin ? null : new Span { Stage = row[0], Id = Number(row[1]),
                Related = Number(row[2]), Begin = begin, End = end, Result = row[5] };
        }

        private static Mark ReadMark(string directory)
        {
            var properties = ReadProperties(Path.Combine(directory, "event.txt"));
            string reason = Get(properties, "event");
            if (reason != "manual" && reason != "possible-frame-stutter" && reason != "possible-fps-drop") return null;
            long unix = Number(Get(properties, "mark_unix_ms")).GetValueOrDefault();
            if (unix <= 0 || unix > 253402300799999L) return null;
            return new Mark { Directory = directory, Reason = reason, UnixMs = unix,
                Qpc = Number(Get(properties, "mark_qpc_100ns")).GetValueOrDefault(),
                Baseline = Get(properties, "baseline_frame_time_ms"), Observed = Get(properties, "observed_frame_time_ms") };
        }

        private sealed class Capture { internal long UnixMs, Qpc, Received; internal string Segment; }
        private sealed class Span { internal string Stage, Result; internal long Begin, End; internal long? Id, Related; }
        private sealed class Mark { internal string Directory, Reason, Baseline, Observed; internal long UnixMs, Qpc; }
    }
}
