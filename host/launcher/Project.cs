using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AsurasWrath.Launcher
{
    internal enum ShaderPreparation { Missing, Stale, Current }

    internal sealed class GraphicsSettings
    {
        public string Renderer = "d3d12", RenderResolution = "1280x720", DisplayMode = "windowed",
                      AntiAliasing = "none", DlssPreset = "K", Upscaler = "", DisplayMonitor = "";
        public int TextureFiltering = 3, FrameRate = 0;
        public bool VSync = true, GameplayCollectorEnabled = true;

        internal string RuntimeRenderer { get { return Renderer == "vulkan-temporal" ? "vulkan" : Renderer; } }

        internal string OutputResolution
        {
            get { return DisplayMode == "borderless" ? DisplayModes.DesktopResolution(DisplayMonitor) : RenderResolution; }
        }

        internal string ActiveUpscaler(bool temporal)
        {
            return (Renderer == "d3d12" || Renderer == "vulkan-temporal") &&
                temporal && OutputResolution == "2560x1440" &&
                !string.IsNullOrEmpty(Upscaler)
                ? Upscaler : "native";
        }

        internal IEnumerable<string> Arguments(string stage, bool temporal)
        {
            string upscaler = ActiveUpscaler(temporal);
            var arguments = new List<string> { "-Stage", stage, "-Renderer", RuntimeRenderer };
            if (upscaler != "native") arguments.AddRange(new[] { "-Upscaler", upscaler });
            if (stage != "Run") return arguments;
            string output = OutputResolution;
            arguments.AddRange(new[] { "-DisplayMode", DisplayMode, "-OutputResolution", output,
                "-AntiAliasing", output == "1280x720" ? "fxaa_extreme" : AntiAliasing,
                "-TextureFiltering", TextureFiltering.ToString(CultureInfo.InvariantCulture),
                "-FrameRate", FrameRate.ToString(CultureInfo.InvariantCulture) });
            string display = DisplayModes.Resolve(DisplayMonitor).DeviceName;
            if (!string.IsNullOrEmpty(display)) arguments.AddRange(new[] { "-DisplayMonitor", display });
            if (FrameRate == 0)
                arguments.AddRange(new[] { "-DisplayRefreshRate", DisplayModes.DesktopRate(DisplayMonitor).ToString(CultureInfo.InvariantCulture) });
            if (upscaler == "dlss") arguments.AddRange(new[] { "-DlssPreset", DlssPreset });
            if (!VSync) arguments.Add("-DisableVSync");
            if (!GameplayCollectorEnabled) arguments.Add("-SkipGameplayCollector");
            return arguments;
        }
    }

    internal sealed class Project
    {
        private static readonly string[] LegacyResolutions = { "1280x720", "2560x1440", "3840x2160" };
        private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
        private readonly Dictionary<string, Tuple<long, long, string>> shaderDigests =
            new Dictionary<string, Tuple<long, long, string>>(StringComparer.OrdinalIgnoreCase);
        private Dictionary<string, object> saved = new Dictionary<string, object>();
        private readonly string settingsPath;
        internal readonly string Root, Output, Logs;
        internal string Catalog { get { return Path.Combine(Output, "achievements", "achievements.toml"); } }

        internal Project(string root)
        {
            Root = root;
            Output = Path.Combine(root, "out");
            Logs = Path.Combine(Output, "logs");
            settingsPath = Path.Combine(Output, "launcher-settings.json");
        }

        internal bool HasDisc { get { return File.Exists(Path.Combine(Root, @"local\game\default.xex")); } }
        internal bool HasUpdate { get { return File.Exists(Path.Combine(Root, @"local\update\default.xexp")); } }
        internal bool IsBuilt { get { return File.Exists(Path.Combine(Output, @"build\win-amd64\Release\asuras_wrath.exe")); } }
        internal bool HasExperimentalVulkan
        {
            get { return File.Exists(Path.Combine(Output, @"vulkan-upscalers\rexgpu-xenos-vulkan.dll")); }
        }

        internal int DlcCount()
        {
            string path = Path.Combine(Root, @"local\dlc");
            return Directory.Exists(path)
                ? Directory.EnumerateFiles(path).Count(file => Regex.IsMatch(Path.GetFileName(file), "^[0-9A-Fa-f]{42}$"))
                : 0;
        }

        internal bool HasShaderCache(string renderer, bool temporal)
        {
            return RecordedShaderFiles(renderer, temporal).Any();
        }

        private IEnumerable<FileInfo> RecordedShaderFiles(string renderer, bool temporal)
        {
            string path = Path.Combine(Output, renderer == "vulkan"
                ? @"cache\vulkan\shaders\shareable" : @"cache\shaders\shareable");
            string suffix = renderer == "vulkan" ? "vk" : "d3d12";
            string pattern = temporal && renderer == "d3d12"
                ? "43430817.rtv.d3d12.xpso" : "43430817.*." + suffix + ".xpso";
            return Directory.Exists(path)
                ? new DirectoryInfo(path).EnumerateFiles(pattern).Where(file => file.Length > 32)
                : Enumerable.Empty<FileInfo>();
        }

        internal ShaderPreparation ShaderPreparationStatus(GraphicsSettings settings, bool temporal)
        {
            string renderer = settings.RuntimeRenderer;
            bool upscaled = settings.ActiveUpscaler(temporal) != "native";
            string variant = renderer == "vulkan" ? "vulkan-fsi" : upscaled ? "d3d12-rtv" : "d3d12";
            string path = Path.Combine(Output, "shader-preparation", variant + ".prepared");
            if (!File.Exists(path)) return ShaderPreparation.Missing;
            return MatchesShaderPreparation(path, renderer, upscaled)
                ? ShaderPreparation.Current : ShaderPreparation.Stale;
        }

        private bool MatchesShaderPreparation(string path, string renderer, bool upscaled)
        {
            try
            {
                var stamp = Json.Deserialize<ShaderPreparationStamp>(File.ReadAllText(path));
                if (stamp == null || stamp.Version != 1 || stamp.Files == null || stamp.Drivers == null)
                    return false;
                var drivers = DisplayDriverVersions().ToArray();
                if (drivers.Length == 0 || !drivers.SequenceEqual(stamp.Drivers, StringComparer.OrdinalIgnoreCase))
                    return false;

                var build = Path.Combine(Output, @"build\win-amd64\Release");
                string module = "rexgpu-xenos-" + renderer + ".dll";
                var archives = new List<FileInfo>(RecordedShaderFiles(renderer, upscaled));
                if (archives.Count == 0) return false;
                var shaderCode = new FileInfo(Path.Combine(archives[0].DirectoryName, "43430817.xsh"));
                if (shaderCode.Exists) archives.Add(shaderCode);
                var files = new List<FileInfo> { new FileInfo(Path.Combine(build, "asuras_wrath.exe")),
                    new FileInfo(Path.Combine(build, module)) };
                if (renderer == "vulkan")
                {
                    var alternate = new FileInfo(Path.Combine(Output, "vulkan-upscalers", module));
                    if (alternate.Exists) files.Add(alternate);
                }
                else if (upscaled)
                {
                    var alternate = new FileInfo(Path.Combine(Root, "runtime", "upscalers", module));
                    if (!alternate.Exists) alternate = new FileInfo(Path.Combine(Output, "dlss-runtime", module));
                    files.Add(alternate);
                }
                int relativeStart = Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Length + 1;
                return files.All(file => file.Exists) && files.Select(file => string.Format(CultureInfo.InvariantCulture,
                    "{0}\t{1}\t{2}", file.FullName.Substring(relativeStart), file.Length, file.LastWriteTimeUtc.Ticks))
                    .Concat(archives.Select(file => file.FullName.Substring(relativeStart) + "\t" + ShaderDigest(file)))
                    .OrderBy(entry => entry, StringComparer.OrdinalIgnoreCase)
                    .SequenceEqual(stamp.Files, StringComparer.OrdinalIgnoreCase);
            }
            catch (Exception error)
            {
                if (!(error is IOException || error is UnauthorizedAccessException ||
                    error is System.Security.SecurityException || error is ArgumentException ||
                    error is InvalidOperationException)) throw;
                return false;
            }
        }

        private string ShaderDigest(FileInfo file)
        {
            Tuple<long, long, string> prior;
            long size = file.Length, ticks = file.LastWriteTimeUtc.Ticks;
            if (shaderDigests.TryGetValue(file.FullName, out prior) && prior.Item1 == size && prior.Item2 == ticks)
                return prior.Item3;
            using (var sha = System.Security.Cryptography.SHA256.Create())
            using (var input = File.Open(file.FullName, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete))
            {
                string digest = BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "");
                shaderDigests[file.FullName] = Tuple.Create(size, ticks, digest);
                return digest;
            }
        }

        private static IEnumerable<string> DisplayDriverVersions()
        {
            using (var key = Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
            {
                if (key == null) yield break;
                foreach (string name in key.GetSubKeyNames().Where(name => Regex.IsMatch(name, @"^\d{4}$"))
                    .OrderBy(name => name, StringComparer.OrdinalIgnoreCase))
                using (var adapter = key.OpenSubKey(name))
                {
                    if (adapter == null) continue;
                    var version = adapter.GetValue("DriverVersion") as string;
                    if (!string.IsNullOrWhiteSpace(version))
                        yield return string.Join("\t", name, version, adapter.GetValue("InfPath", "").ToString(),
                            adapter.GetValue("MatchingDeviceId", "").ToString());
                }
            }
        }

        private sealed class ShaderPreparationStamp
        {
            public int Version { get; set; }
            public string[] Drivers { get; set; }
            public string[] Files { get; set; }
        }

        internal string[] AvailableUpscalers(string renderer = "d3d12")
        {
            bool vulkan = renderer == "vulkan-temporal";
            bool runtime = vulkan ? HasExperimentalVulkan :
                File.Exists(Path.Combine(Root, @"runtime\upscalers\rexgpu-xenos-d3d12.dll")) ||
                File.Exists(Path.Combine(Output, @"dlss-runtime\rexgpu-xenos-d3d12.dll"));
            if (!runtime) return new string[0];
            string vendors = Path.Combine(Root, @".tools\renderer-deps");
            string nvidia = Path.Combine(vendors, @"streamline-2.14.16\bin\x64");
            string amd = Path.Combine(vendors, @"fsr-2.4.0\FidelityFX-SDK-2.4.0\Kits\FidelityFX\signedbin");
            var result = new List<string>();
            if (new[] { "sl.interposer.dll", "sl.common.dll", "sl.pcl.dll", "sl.dlss.dll", "nvngx_dlss.dll" }
                .All(file => File.Exists(Path.Combine(nvidia, file)))) result.Add("dlss");
            string suffix = vulkan ? "_vk.dll" : "_dx12.dll";
            if (new[] { "amd_fidelityfx_loader", "amd_fidelityfx_upscaler" }
                .All(file => File.Exists(Path.Combine(amd, file + suffix)))) result.Add("fsr3");
            return result.ToArray();
        }

        private T Value<T>(string key, T fallback)
        {
            object value;
            return saved.TryGetValue(key, out value) && value is T ? (T)value : fallback;
        }

        internal GraphicsSettings LoadSettings(bool temporal)
        {
            var g = new GraphicsSettings();
            if (!File.Exists(settingsPath)) return g;
            saved = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(settingsPath)) ??
                new Dictionary<string, object>();
            g.Renderer = Value("Renderer", g.Renderer);
            int scale = Value("Scale", 1);
            scale = Math.Max(1, Math.Min(3, scale));
            g.RenderResolution = Value("RenderResolution", LegacyResolutions[scale - 1]);
            g.DisplayMode = Value("DisplayMode", Value("Fullscreen", false) ? "borderless" : "windowed");
            g.DisplayMonitor = Value("DisplayMonitor", g.DisplayMonitor);
            g.AntiAliasing = Value("AntiAliasing", g.AntiAliasing);
            g.DlssPreset = Value("DlssPreset", g.DlssPreset);
            g.Upscaler = Value("Upscaler", g.Upscaler);
            if (temporal && !saved.ContainsKey("DlssPreset") && !saved.ContainsKey("Upscaler"))
                g.RenderResolution = "1280x720";
            g.TextureFiltering = Value("TextureFiltering", g.TextureFiltering);
            g.FrameRate = Value("FrameRate", g.FrameRate);
            g.VSync = Value("VSync", g.VSync);
            g.GameplayCollectorEnabled = Value("GameplayCollectorEnabled", g.GameplayCollectorEnabled);
            return g;
        }

        internal void SaveSettings(GraphicsSettings settings)
        {
            saved.Remove("MotionBlur");
            saved.Remove("PreserveAspectRatio");
            foreach (var field in typeof(GraphicsSettings).GetFields()) saved[field.Name] = field.GetValue(settings);
            Directory.CreateDirectory(Output);
            string pending = settingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllText(pending, Json.Serialize(saved), new UTF8Encoding(false));
                if (File.Exists(settingsPath)) File.Replace(pending, settingsPath, null);
                else File.Move(pending, settingsPath);
            }
            finally { if (File.Exists(pending)) File.Delete(pending); }
        }

        internal string SaveImportSelection(string[] paths)
        {
            Directory.CreateDirectory(Output);
            string file = Path.Combine(Output, "launcher-input-" + Guid.NewGuid().ToString("N") + ".json");
            File.WriteAllText(file, Json.Serialize(paths), Encoding.UTF8);
            return file;
        }

        internal AchievementSet ReadAchievements()
        {
            string profile = Path.Combine(Root, "userdata");
            if (!Directory.Exists(profile)) profile = Path.Combine(Output, "user");
            string progress = Path.Combine(profile, @"achievements\43430817.toml");
            var unlocked = new HashSet<int>();
            if (File.Exists(progress))
            {
                foreach (string line in File.ReadLines(progress))
                {
                    Match entry = Regex.Match(line, @"^\[unlocked\.(\d+)\]\s*$");
                    if (entry.Success) unlocked.Add(int.Parse(entry.Groups[1].Value, CultureInfo.InvariantCulture));
                }
            }
            var rows = new List<Achievement>();
            foreach (string record in Regex.Split(File.ReadAllText(Catalog), @"(?m)^\[\[achievements\]\]\s*$").Skip(1))
            {
                var fields = new Dictionary<string, string>();
                foreach (Match field in Regex.Matches(record,
                    @"(?m)^(id|label|description|unachieved_description|gamerscore|image_id)\s*=\s*(.+?)\s*$"))
                    fields[field.Groups[1].Value] = field.Groups[2].Value;
                if (fields.Count != 6) throw new InvalidDataException("An achievement record is incomplete.");
                int id = int.Parse(fields["id"], CultureInfo.InvariantCulture);
                bool earned = unlocked.Contains(id);
                string title = Json.Deserialize<string>(fields["label"]);
                string description = Json.Deserialize<string>(fields[earned ? "description" : "unachieved_description"]);
                string icon = Path.Combine(Output, "achievements", "icons", fields["image_id"] + ".png");
                if (!earned && (string.IsNullOrEmpty(description) || description == "<String Placeholder>"))
                {
                    title = "Secret achievement";
                    description = "Details appear when unlocked.";
                }
                rows.Add(new Achievement { Id = id, Title = title, Description = description, Unlocked = earned,
                    Score = fields["gamerscore"] + " G", Icon = File.Exists(icon) ? icon : null,
                    Points = int.Parse(fields["gamerscore"], CultureInfo.InvariantCulture) });
            }
            if (rows.Count == 0) throw new InvalidDataException("Achievement metadata is empty.");
            string summary = string.Format("{0} of {1} unlocked  ·  {2} of {3} G",
                rows.Count(row => row.Unlocked), rows.Count, rows.Where(row => row.Unlocked).Sum(row => row.Points),
                rows.Sum(row => row.Points));
            foreach (int id in unlocked.Except(rows.Select(row => row.Id)).ToArray())
                rows.Add(new Achievement { Id = id, Title = "Achievement " + id, Unlocked = true,
                    Description = "Details are unavailable in the imported metadata." });
            return new AchievementSet { Summary = summary,
                Rows = rows.OrderByDescending(row => row.Unlocked).ThenBy(row => row.Id).ToArray() };
        }
    }

    internal sealed class AchievementSet
    {
        internal string Summary;
        internal Achievement[] Rows;
    }

    internal sealed class Achievement
    {
        public int Id { get; set; }
        public int Points { get; set; }
        public string Title { get; set; }
        public string Description { get; set; }
        public string Icon { get; set; }
        public string Score { get; set; }
        public bool Unlocked { get; set; }
        public string State { get { return Unlocked ? "Unlocked" : "Locked"; } }
    }

    internal static class DisplayModes
    {
        internal sealed class Monitor
        {
            internal string Key, DeviceName, Label;
        }

        internal static Monitor[] Monitors()
        {
            return System.Windows.Forms.Screen.AllScreens.OrderByDescending(screen => screen.Primary)
                .ThenBy(screen => screen.DeviceName, StringComparer.OrdinalIgnoreCase)
                .Select(screen =>
                {
                    var device = new DisplayDevice { Size = Marshal.SizeOf(typeof(DisplayDevice)) };
                    bool found = EnumDisplayDevices(screen.DeviceName, 0, ref device, 1);
                    string number = Regex.Match(screen.DeviceName, @"\d+$").Value;
                    string label = string.IsNullOrEmpty(number) ? screen.DeviceName : "Display " + number;
                    if (found && !string.IsNullOrWhiteSpace(device.Description) &&
                        !device.Description.StartsWith("Generic", StringComparison.OrdinalIgnoreCase))
                        label += " · " + device.Description;
                    if (screen.Primary) label += " (primary)";
                    return new Monitor
                    {
                        Key = found && !string.IsNullOrEmpty(device.Id) ? device.Id : screen.DeviceName,
                        DeviceName = screen.DeviceName,
                        Label = label
                    };
                }).ToArray();
        }

        internal static Monitor Resolve(string key)
        {
            var monitors = Monitors();
            return monitors.FirstOrDefault(monitor => string.Equals(monitor.Key, key,
                StringComparison.OrdinalIgnoreCase)) ?? monitors.FirstOrDefault() ??
                new Monitor { Key = "", DeviceName = "", Label = "Primary display" };
        }

        private static Mode Desktop(string deviceName)
        {
            var mode = new Mode { Size = (ushort)Marshal.SizeOf(typeof(Mode)) };
            if (!string.IsNullOrEmpty(deviceName) && EnumDisplaySettings(deviceName, -1, ref mode)) return mode;
            return new Mode { Width = 1280, Height = 720, Frequency = 60 };
        }

        internal static string DesktopResolution(string key)
        {
            var mode = Desktop(Resolve(key).DeviceName);
            return mode.Width + "x" + mode.Height;
        }

        internal static int DesktopRate(string key)
        {
            int frequency = Desktop(Resolve(key).DeviceName).Frequency;
            return frequency >= 24 ? Math.Min(frequency, 240) : 60;
        }

        internal static string[] Supported(string key)
        {
            string deviceName = Resolve(key).DeviceName;
            var desktop = Desktop(deviceName);
            var found = new HashSet<Tuple<int, int>> { Tuple.Create(desktop.Width, desktop.Height) };
            if (!string.IsNullOrEmpty(deviceName))
            {
                var mode = new Mode { Size = (ushort)Marshal.SizeOf(typeof(Mode)) };
                for (int index = 0; EnumDisplaySettings(deviceName, index, ref mode); ++index)
                    if (mode.Width > 0 && mode.Height > 0 && mode.Width <= 16384 && mode.Height <= 16384)
                        found.Add(Tuple.Create(mode.Width, mode.Height));
            }
            return found.OrderBy(size => (long)size.Item1 * size.Item2).ThenBy(size => size.Item1)
                .Select(size => size.Item1 + "x" + size.Item2).ToArray();
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DisplayDevice
        {
            public int Size;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
            public uint State;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Key;
        }

        [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode, Size = 220)]
        private struct Mode
        {
            [FieldOffset(68)] public ushort Size;
            [FieldOffset(172)] public int Width;
            [FieldOffset(176)] public int Height;
            [FieldOffset(184)] public int Frequency;
        }

        [DllImport("user32.dll", EntryPoint = "EnumDisplaySettingsW", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplaySettings(string device, int index, ref Mode mode);

        [DllImport("user32.dll", EntryPoint = "EnumDisplayDevicesW", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice info, uint flags);
    }
}
