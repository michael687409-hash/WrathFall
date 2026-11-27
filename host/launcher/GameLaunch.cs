using System;
using System.Collections.Specialized;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace AsurasWrath.Launcher
{
    internal sealed class ShaderPreparationRequest
    {
        public string Renderer = "d3d12", Upscaler = "native", DisplayMode = "windowed", AntiAliasing = "none";
        public int ResolutionScale = 1, TextureFiltering = 3;
        public bool VSync = true;
    }

    internal static class GameLaunch
    {
        internal const string WorkerCommand = "--play-worker";
        internal const string ShaderWorkerCommand = "--prepare-shaders-worker";
        internal const string CollectCommand = "--collect";
        private static readonly string[] UpscalerVariables =
        {
            "ASURA_LIVE_UPSCALER", "ASURA_UPSCALER_REPORT_DIR", "ASURA_LIVE_DLSS_QA",
            "ASURA_LIVE_DLSS_PRESENT", "ASURA_LIVE_DLSS_RUNTIME", "ASURA_DLSS_PRESET",
            "ASURA_LIVE_DLSS_DIAGNOSTICS", "ASURA_REMOVE_MOTION_BLUR", "WRATHFALL_VK_TEMPORAL",
            "WRATHFALL_VK_SCENE_TRACE", "WRATHFALL_VK_DLSS_PROBE_RUNTIME", "WRATHFALL_VK_FSR_PROBE_RUNTIME"
        };

        internal static string Serialize(GraphicsSettings settings)
        {
            return new JavaScriptSerializer().Serialize(settings);
        }

        internal static string SerializePreparation(ShaderPreparationRequest request)
        {
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(request)));
        }

        internal static int PrepareShaders(Project project, string[] args)
        {
            if (args.Length != 2) throw new ArgumentException("The shader preparation settings are missing.");
            string json = Encoding.UTF8.GetString(Convert.FromBase64String(args[1]));
            var request = new JavaScriptSerializer().Deserialize<ShaderPreparationRequest>(json);
            if (request == null || !new[] { "d3d12", "vulkan" }.Contains(request.Renderer) ||
                !new[] { "native", "dlss", "fsr3" }.Contains(request.Upscaler) ||
                !new[] { "windowed", "borderless", "fullscreen" }.Contains(request.DisplayMode) ||
                !new[] { "none", "fxaa", "fxaa_extreme" }.Contains(request.AntiAliasing) ||
                request.TextureFiltering < -1 || request.TextureFiltering > 5 ||
                request.ResolutionScale < 1 || request.ResolutionScale > 3)
                throw new ArgumentException("The shader preparation settings are invalid.");
            return PrepareShaders(project, request);
        }

        private static int PrepareShaders(Project project, ShaderPreparationRequest request)
        {
            using (GameLaunchInputs.Lock(project.Output))
            {
                string renderer = request.Renderer;
                bool vulkan = renderer == "vulkan", temporal = request.Upscaler != "native";
                bool needsAlternateRenderer = temporal && !vulkan;
                VerifyInstallation(project);
                if (!project.HasShaderCache(renderer, temporal))
                    throw new InvalidOperationException("No recorded shaders for this renderer are available to precompile.");
                if (needsAlternateRenderer)
                {
                    if (request.ResolutionScale != 1)
                        throw new InvalidOperationException(ProviderName(request.Upscaler) + " requires 720p internal resolution.");
                    if (!project.AvailableUpscalers(renderer).Contains(request.Upscaler))
                        throw new FileNotFoundException(ProviderName(request.Upscaler) + " renderer or vendor runtime is missing.");
                }

                string snapshot = null;
                try { snapshot = project.ShaderPreparationSnapshot(renderer, temporal); }
                catch (Exception error) { Console.WriteLine("Could not verify shader preparation inputs. " + error.Message); }

                string executable = SelectRuntime(project, vulkan, temporal);
                string user = Path.Combine(project.Output, "shader-preparation", "user");
                string logs = Path.Combine(project.Logs, "shader-preparation");
                Directory.CreateDirectory(logs);
                var arguments = RuntimeArguments(project, renderer, temporal, user, logs, request.ResolutionScale);
                AddGraphicsArguments(arguments, request.DisplayMode, request.VSync, request.TextureFiltering, request.AntiAliasing);
                arguments.AddRange(new[] { "--ui.driver=none", "--log.level=info",
                    "--gpu." + renderer + ".pipeline_creation_threads=0" });
                var start = RuntimeStart(project, executable, arguments);
                start.WindowStyle = ProcessWindowStyle.Hidden;
                start.EnvironmentVariables["ASURAS_WRATH_FRAME_RATE"] = "30";
                start.EnvironmentVariables["ASURAS_WRATH_PREPARE_SHADERS"] = "1";

                Console.WriteLine("Precompiling recorded shaders...");
                using (var process = StartRuntime(start))
                {
                    process.WaitForExit();
                    if (process.ExitCode != 0)
                    {
                        Console.Error.WriteLine("Shader preparation exited with code " + Number(process.ExitCode) + ". See " + logs);
                        return process.ExitCode;
                    }
                }
                Console.WriteLine("Recorded shader precompilation finished.");
                try
                {
                    if (snapshot != null && snapshot == project.ShaderPreparationSnapshot(renderer, temporal))
                        project.SaveShaderPreparation(renderer, temporal, snapshot);
                    else Console.WriteLine("The shader preparation inputs could not be verified. Recompile shaders to update their status.");
                }
                catch (Exception error) { Console.WriteLine("Could not save the shader preparation status. " + error.Message); }
                return 0;
            }
        }

        internal static int Run(Project project, string[] args)
        {
            if (args.Length != 2 && args.Length != 3) throw new ArgumentException("The game launch settings are missing.");
            long click = Stopwatch.GetTimestamp();
            long requested;
            if (args.Length == 3 && long.TryParse(args[2], NumberStyles.None, CultureInfo.InvariantCulture, out requested) &&
                requested > 0 && requested <= click) click = requested;
            var settings = new JavaScriptSerializer().Deserialize<GraphicsSettings>(args[1]);
            if (settings == null) throw new InvalidDataException("The game launch settings are invalid.");
            using (GameLaunchInputs.Lock(project.Output))
                return RunLocked(project, settings, click);
        }

        private static int RunLocked(Project project, GraphicsSettings settings, long click)
        {
            const string native = "native";
            string output = settings.OutputResolution;
            string provider = settings.ActiveUpscaler(!string.IsNullOrEmpty(settings.Upscaler), output);
            Validate(settings, output);
            string renderer = settings.RuntimeRenderer;
            bool vulkan = renderer == "vulkan", temporal = provider != native;
            if (temporal && !project.AvailableUpscalers(settings.Renderer).Contains(provider))
                throw new FileNotFoundException(ProviderName(provider) + " renderer or vendor runtime is missing.");
            string game = Path.Combine(project.Root, @"local\game");
            VerifyInstallation(project);

            bool traceVulkan = vulkan && settings.GameplayCollectorEnabled && project.HasExperimentalVulkan;
            string executable = SelectRuntime(project, vulkan, temporal);
            string user = GameLaunchInputs.PreparePlayerData(project);
            Console.WriteLine("Using the save folder at " + user);
            string cache = Path.Combine(project.Output, "cache");
            string overridePath = GameLaunchInputs.PrepareGraphicsOverride(game, cache, temporal, settings.FrameRate != 30);
            string monitor = DisplayModes.Resolve(settings.DisplayMonitor).DeviceName;
            Directory.CreateDirectory(project.Logs);
            var arguments = RuntimeArguments(project, renderer, temporal, user, project.Logs, 1);
            AddGraphicsArguments(arguments, settings.DisplayMode, settings.VSync, settings.TextureFiltering,
                output == "1280x720" ? "fxaa_extreme" : settings.AntiAliasing);
            arguments.AddRange(new[] { "--ui.resolution=" + output,
                "--ui.video_mode_width=1280", "--ui.video_mode_height=720", "--gpu.frame_limit=" + Number(settings.FrameRate)
            });
            if (string.IsNullOrEmpty(monitor)) arguments.Add("--ui.monitor=1");
            if (settings.FrameRate == 0)
                arguments.Add("--ui.video_mode_refresh_rate=" + Number(DisplayModes.DesktopRate(settings.DisplayMonitor)));
            else if (settings.FrameRate > 60)
                arguments.Add("--ui.video_mode_refresh_rate=" + Number(settings.FrameRate));

            var start = RuntimeStart(project, executable, arguments);
            var environment = start.EnvironmentVariables;
            environment["ASURAS_WRATH_FRAME_RATE"] = Number(settings.FrameRate);
            if (!string.IsNullOrEmpty(monitor)) environment["ASURAS_WRATH_DISPLAY_MONITOR"] = monitor;
            environment["ASURAS_WRATH_GRAPHICS_OVERRIDE"] = overridePath;
            if (traceVulkan) environment["WRATHFALL_VK_FRAME_TRACE_DIR"] = project.Logs;
            else if (vulkan && settings.GameplayCollectorEnabled)
                Console.WriteLine("Vulkan frame tracing is unavailable without the WrathFall Vulkan renderer.");
            string trace = temporal ? ConfigureUpscaler(environment, project, settings, renderer, provider) : null;

            using (var process = StartRuntime(start))
            {
                double milliseconds = (Stopwatch.GetTimestamp() - click) * 1000.0 / Stopwatch.Frequency;
                Console.WriteLine("Game process started after " + milliseconds.ToString("F0", CultureInfo.InvariantCulture) + " ms.");
                if (settings.GameplayCollectorEnabled)
                {
                    try { WatchCollector(project, process, renderer, provider); }
                    catch (Exception error) { Console.WriteLine("Gameplay collector unavailable. " + error.Message); }
                }
                else Console.WriteLine("Gameplay collector disabled.");
                process.WaitForExit();
                if (process.ExitCode != 0)
                {
                    Console.Error.WriteLine("Game exited with code " + Number(process.ExitCode) + ". See " + project.Logs);
                    return process.ExitCode;
                }
            }

            if (temporal) VerifyUpscalerTrace(trace, vulkan, provider);
            return 0;
        }

        private static void VerifyInstallation(Project project)
        {
            GameLaunchInputs.VerifyAndInstallPatch(Path.Combine(project.Root, @"local\game"),
                Path.Combine(project.Root, @"local\update"));
            if (!project.IsBuilt) throw new FileNotFoundException("Build the PC version first. Missing " + project.GameExecutable);
        }

        private static List<string> RuntimeArguments(Project project, string renderer, bool temporal,
                                                      string user, string logs, int scale)
        {
            var arguments = new List<string>
            {
                "--title.game_root=" + Path.Combine(project.Root, @"local\game"),
                "--title.update_root=" + Path.Combine(project.Root, @"local\update"),
                "--app.user_root=" + user, "--app.log_root=" + logs,
                "--app.cache_root=" + project.ShaderCacheRoot(renderer), "--gpu.driver=" + renderer,
                "--gpu.resolution_scale_x=" + Number(scale), "--gpu.resolution_scale_y=" + Number(scale),
                "--hid.shared_pads=true", "--gpu.xenos.present_letterbox=true", "--gpu.xenos.store_shaders=true",
                "--gpu.xenos.async_shader_compilation=false"
            };
            if (renderer == "vulkan") arguments.Add("--gpu.vulkan.render_target_path=fsi");
            else if (temporal) arguments.Add("--gpu.d3d12.render_target_path=rtv");
            return arguments;
        }

        private static void AddGraphicsArguments(List<string> arguments, string mode, bool vsync, int filter, string aa)
        {
            arguments.AddRange(new[] { "--ui.fullscreen=" + Flag(mode != "windowed"),
                "--ui.fullscreen_exclusive=" + Flag(mode == "fullscreen"), "--gpu.vsync=" + Flag(vsync),
                "--gpu.anisotropic_override=" + Number(filter), "--gpu.xenos.swap_post_effect=" + aa });
        }

        private static ProcessStartInfo RuntimeStart(Project project, string executable, IEnumerable<string> arguments)
        {
            var start = new ProcessStartInfo(executable, string.Join(" ", arguments.Select(BackgroundJob.Quote)))
            {
                WorkingDirectory = project.Root, UseShellExecute = false, CreateNoWindow = true
            };
            var environment = start.EnvironmentVariables;
            foreach (string variable in UpscalerVariables) environment.Remove(variable);
            environment.Remove("WRATHFALL_VK_FRAME_TRACE_DIR");
            environment.Remove("ASURAS_WRATH_PREPARE_SHADERS");
            environment.Remove("ASURAS_WRATH_GRAPHICS_OVERRIDE");
            environment.Remove("ASURAS_WRATH_DISPLAY_MONITOR");
            return start;
        }

        private static Process StartRuntime(ProcessStartInfo start)
        {
            var process = Process.Start(start);
            if (process == null) throw new InvalidOperationException("The game runtime could not be started.");
            return process;
        }

        private static void Validate(GraphicsSettings settings, string output)
        {
            var resolution = Regex.Match(output ?? "", @"^([1-9][0-9]{0,4})x([1-9][0-9]{0,4})$");
            if (!resolution.Success || int.Parse(resolution.Groups[1].Value, CultureInfo.InvariantCulture) > 16384 ||
                int.Parse(resolution.Groups[2].Value, CultureInfo.InvariantCulture) > 16384)
                throw new ArgumentException("Output resolution must be within 16384 × 16384.");
            if (!new[] { "d3d12", "vulkan", "vulkan-temporal" }.Contains(settings.Renderer) ||
                !new[] { "windowed", "borderless", "fullscreen" }.Contains(settings.DisplayMode) ||
                !new[] { "none", "fxaa", "fxaa_extreme" }.Contains(settings.AntiAliasing) ||
                !new[] { 0, 30, 60, 120, 144 }.Contains(settings.FrameRate) ||
                settings.TextureFiltering < -1 || settings.TextureFiltering > 5)
                throw new ArgumentException("One or more game launch settings are invalid.");
        }

        private static string SelectRuntime(Project project, bool vulkan, bool temporal)
        {
            bool alternate = vulkan ? project.HasExperimentalVulkan : temporal;
            return alternate ? StageRenderer(project, vulkan) : project.GameExecutable;
        }

        private static string StageRenderer(Project project, bool vulkan)
        {
            string module = project.TemporalModule(vulkan);
            string directory = Path.Combine(project.Output, vulkan ? "vulkan-upscalers" : "dlss-runtime", "game");
            Directory.CreateDirectory(directory);
            string moduleName = Path.GetFileName(module);
            var files = Directory.EnumerateFiles(Path.GetDirectoryName(project.GameExecutable), "*.dll")
                .Where(file => !string.Equals(Path.GetFileName(file), moduleName, StringComparison.OrdinalIgnoreCase))
                .Concat(new[] { module, project.GameExecutable });
            LaunchCache.Materialize(directory, files, "renderer-v1", (file, pending) => File.Copy(file, pending));
            return Path.Combine(directory, Path.GetFileName(project.GameExecutable));
        }

        private static string ConfigureUpscaler(StringDictionary environment, Project project, GraphicsSettings settings,
                                               string renderer, string provider)
        {
            string model = provider == "dlss" ? settings.DlssPreset : null;
            if (provider == "dlss" && string.IsNullOrEmpty(model)) model = "K";
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
            string log = Path.Combine(project.Logs, renderer + "-" + provider + "-" + stamp);
            Directory.CreateDirectory(log);
            string vendor = project.VendorRuntime(provider);
            environment["ASURA_LIVE_UPSCALER"] = provider;
            environment["ASURA_UPSCALER_REPORT_DIR"] = project.Logs;
            environment["ASURA_LIVE_DLSS_QA"] = log;
            environment["ASURA_LIVE_DLSS_PRESENT"] = "1";
            environment["ASURA_LIVE_DLSS_RUNTIME"] = vendor;
            if (model != null) environment["ASURA_DLSS_PRESET"] = model;
            environment["ASURA_LIVE_DLSS_DIAGNOSTICS"] = "0";
            environment["ASURA_REMOVE_MOTION_BLUR"] = "1";
            if (renderer == "vulkan")
            {
                environment["WRATHFALL_VK_TEMPORAL"] = provider;
                environment["WRATHFALL_VK_SCENE_TRACE"] = Path.Combine(log, "vulkan-scene.tsv");
                environment[provider == "dlss" ? "WRATHFALL_VK_DLSS_PROBE_RUNTIME" : "WRATHFALL_VK_FSR_PROBE_RUNTIME"] = vendor;
            }
            Console.WriteLine(ProviderName(provider) + " on " + renderer + ", 1280x720 to 2560x1440" +
                (model == null ? "" : ", preset " + model) + ". Log folder " + log);
            return log;
        }

        private static void WatchCollector(Project project, Process game, string renderer, string provider)
        {
            string executable = Path.Combine(Path.GetDirectoryName(project.GameExecutable), "asura_gameplay_collector.exe");
            if (!File.Exists(executable)) throw new FileNotFoundException("Build the PC version to install the collector.");
            IntPtr window = IntPtr.Zero;
            while (!game.HasExited)
            {
                game.Refresh();
                window = game.MainWindowHandle;
                if (window != IntPtr.Zero || game.WaitForExit(100)) break;
            }
            if (window == IntPtr.Zero || game.HasExited) return;

            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
            string log = Path.Combine(project.Logs, "gameplay-collector-" + stamp + "-" + Number(game.Id) + ".log");
            string error = Path.ChangeExtension(log, ".err.log");
            using (var collector = BackgroundJob.StartCollector(project.Root, executable,
                new[] { project.Root, Number(game.Id), window.ToInt64().ToString(CultureInfo.InvariantCulture), renderer, provider },
                log, error))
            {
                Console.WriteLine("Collector log: " + log);
                bool active = false;
                while (!game.WaitForExit(250))
                {
                    uint exit;
                    if (collector.TryGetExitCode(out exit))
                    {
                        Console.WriteLine("Gameplay collector stopped. See " + error);
                        return;
                    }
                    if (!active && new FileInfo(log).Length != 0)
                    {
                        active = true;
                        Console.WriteLine("Gameplay collector active. Press L in the game to mark an issue.");
                    }
                }
                // Keep the game handle alive while the collector saves pending marks after exit.
                collector.WaitForExit(10000);
            }
        }

        internal static int Collect(Project project)
        {
            string executable = Path.Combine(Path.GetDirectoryName(project.GameExecutable), "asura_gameplay_collector.exe");
            if (!File.Exists(executable)) throw new FileNotFoundException("Build the PC version to install the collector.");
            string[] paths = { project.GameExecutable, Path.Combine(project.Output, @"dlss-runtime\game\asuras_wrath.exe"),
                Path.Combine(project.Output, @"vulkan-upscalers\game\asuras_wrath.exe") };
            var games = Process.GetProcessesByName("asuras_wrath");
            try
            {
                foreach (var game in games)
                {
                    IntPtr window;
                    try
                    {
                        window = game.MainWindowHandle;
                        if (window == IntPtr.Zero || !paths.Contains(game.MainModule.FileName, StringComparer.OrdinalIgnoreCase)) continue;
                    }
                    catch (Win32Exception) { continue; }
                    catch (InvalidOperationException) { continue; }
                    var arguments = new[] { project.Root, Number(game.Id), window.ToInt64().ToString(CultureInfo.InvariantCulture) };
                    using (var collector = Process.Start(new ProcessStartInfo(executable, string.Join(" ", arguments.Select(BackgroundJob.Quote)))
                        { UseShellExecute = false, WorkingDirectory = project.Root }))
                    {
                        if (collector == null) throw new InvalidOperationException("The gameplay collector could not be started.");
                        collector.WaitForExit();
                        return collector.ExitCode;
                    }
                }
                throw new InvalidOperationException("Start the game from this launcher before opening the collector.");
            }
            finally { foreach (var game in games) game.Dispose(); }
        }

        private static void VerifyUpscalerTrace(string directory, bool vulkan, string provider)
        {
            string name = ProviderName(provider);
            string error = Path.Combine(directory, "live-temporal-error.txt");
            if (!vulkan && File.Exists(error)) throw new InvalidOperationException(name + " could not start. " + File.ReadAllText(error));
            string trace = Path.Combine(directory, vulkan ? "vulkan-scene.tsv" : "live-temporal.tsv");
            if (!File.Exists(trace)) throw new InvalidOperationException(name + " renderer did not start. See " + directory);
            if (vulkan)
            {
                if (File.ReadLines(trace).Any(line => Regex.IsMatch(line,
                    @"^(dlss-(instance|context)-error|fsr-context-error|temporal-init-error|upscale-error)\t")))
                    throw new InvalidOperationException(name + " reported an error. See " + directory);
            }
            else
            {
                string failure = File.ReadLines(trace).LastOrDefault(line => line.StartsWith("disabled\t", StringComparison.Ordinal));
                if (failure != null) throw new InvalidOperationException(name + " stopped during play. " + failure.Substring(9));
            }
        }

        private static string Number(int value) { return value.ToString(CultureInfo.InvariantCulture); }
        private static string Flag(bool value) { return value ? "true" : "false"; }
        private static string ProviderName(string provider) { return provider == "fsr3" ? "FSR 3" : "DLSS"; }
    }
}
