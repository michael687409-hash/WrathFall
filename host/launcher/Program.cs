using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Threading;
using Microsoft.Win32;

[assembly: AssemblyTitle("WrathFall Launcher")]
[assembly: AssemblyProduct("WrathFall")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace AsurasWrath.Launcher
{
    internal static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            string command = args.Length == 0 ? null : args[0];
            bool projectTool = ProjectTools.Handles(command);
            bool worker = projectTool || command == GameLaunch.WorkerCommand || command == GameLaunch.ShaderWorkerCommand ||
                command == GameImport.WorkerCommand || command == GameLaunch.CollectCommand;
            var stdout = GetStdHandle(-11);
            if (worker && (stdout == IntPtr.Zero || stdout == new IntPtr(-1))) AttachConsole(uint.MaxValue);
            try
            {
                string root = AppDomain.CurrentDomain.BaseDirectory;
                string engine = Path.Combine(root, "engine");
                if (File.Exists(Path.Combine(engine, Project.InstallManifest))) root = engine;
                if (!File.Exists(Path.Combine(root, Project.InstallManifest)))
                    throw new FileNotFoundException("Extract the complete download before opening the launcher.");
                var project = new Project(root);
                if (projectTool) return ProjectTools.Run(project, args);
                if (command == GameLaunch.WorkerCommand) return GameLaunch.Run(project, args);
                if (command == GameLaunch.ShaderWorkerCommand) return GameLaunch.PrepareShaders(project, args);
                if (command == GameImport.WorkerCommand) return GameImport.Run(project, args);
                if (command == GameLaunch.CollectCommand) return GameLaunch.Collect(project);
                return new Application().Run(new LauncherWindow(project).Window);
            }
            catch (Exception error)
            {
                if (worker) Console.Error.WriteLine(error.Message);
                else MessageBox.Show(error.GetBaseException().Message, "WrathFall",
                    MessageBoxButton.OK, MessageBoxImage.Error);
                return 1;
            }
        }

        [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int handle);
        [DllImport("kernel32.dll")] private static extern bool AttachConsole(uint processId);
    }

    internal sealed class LauncherWindow
    {
        private static readonly string[] Pages = { "Play", "Setup", "Graphics", "DlssNr", "Achievements" };
        private static readonly string[] ComboNames = { "Renderer", "Resolution", "DisplayMode", "DisplayMonitor", "FrameRate",
            "Upscaler", "DlssPreset", "AntiAliasing", "TextureFiltering" };
        private static readonly string[] CheckNames = { "VSync", "Collector" };
        private static readonly string[] JobButtons = { "ChooseDisc", "ChooseUpdate", "ChooseDlc" };
        private readonly Project project;
        private readonly DlssNrPassPreview[] nrPasses = new DlssNrPassPreview[30];
        private bool TemporalAvailable { get { return !string.IsNullOrEmpty(Selected("Upscaler")); } }
        private readonly DispatcherTimer timer;
        private BackgroundJob job;
        private bool applying, changed;
        private string page, status, preferredResolution;
        internal readonly Window Window;

        internal LauncherWindow(Project project)
        {
            this.project = project;
            using (Stream xaml = typeof(LauncherWindow).Assembly.GetManifestResourceStream("LauncherWindow.xaml"))
                Window = (Window)XamlReader.Load(xaml);
            SetBackdrop();
            using (var icon = System.Drawing.Icon.ExtractAssociatedIcon(typeof(Program).Assembly.Location))
                if (icon != null) Window.Icon = Imaging.CreateBitmapSourceFromHIcon(icon.Handle,
                    Int32Rect.Empty, System.Windows.Media.Imaging.BitmapSizeOptions.FromEmptyOptions());
            Add("Renderer", "d3d12", "Direct3D 12");
            Add("Renderer", "vulkan", "Vulkan");
            if (project.HasExperimentalVulkan)
                Add("Renderer", "vulkan-temporal", "Vulkan (experimental upscaling)");
            Add("DisplayMode", "windowed", "Windowed");
            Add("DisplayMode", "borderless", "Borderless windowed");
            Add("DisplayMode", "fullscreen", "Exclusive fullscreen");
            foreach (string rate in new[] { "30", "60", "120", "144" })
                Add("FrameRate", rate, rate + " FPS");
            Add("FrameRate", "0", "Uncapped");
            Add("DlssPreset", "Default", "Driver default");
            foreach (string model in new[] { "J", "K", "L", "M" })
                Add("DlssPreset", model, "Model " + model);
            Add("AntiAliasing", "none", "Off");
            Add("AntiAliasing", "fxaa", "FXAA");
            Add("AntiAliasing", "fxaa_extreme", "FXAA High");
            string[] filtering = { "Automatic", "Off", "1×", "2×", "4×", "8×", "16×" };
            for (int i = 0; i < filtering.Length; ++i)
                Add("TextureFiltering", (i - 1).ToString(CultureInfo.InvariantCulture), filtering[i]);

            GraphicsSettings initial;
            try { initial = project.LoadSettings(project.AvailableUpscalers().Length != 0); }
            catch (Exception error)
            {
                initial = new GraphicsSettings();
                Log("Could not load launcher settings. " + error.Message);
            }
            Apply(initial);
            SetUpDlssNrPreview();
            foreach (string name in ComboNames.Where(name => name != "Resolution" && name != "DisplayMode" &&
                name != "DisplayMonitor" && name != "Renderer"))
                Control<ComboBox>(name).SelectionChanged += delegate { GraphicsChanged(); };
            Control<ComboBox>("Renderer").SelectionChanged += delegate
            {
                if (applying) return;
                applying = true;
                try { LoadUpscalers(Selected("Upscaler")); }
                finally { applying = false; }
                GraphicsChanged();
            };
            Control<ComboBox>("Resolution").SelectionChanged += delegate
            {
                if (applying) return;
                preferredResolution = Selected("Resolution");
                GraphicsChanged();
            };
            foreach (string name in new[] { "DisplayMode", "DisplayMonitor" })
            {
                Control<ComboBox>(name).SelectionChanged += delegate
                {
                    if (applying) return;
                    applying = true;
                    try { LoadResolutions(); }
                    finally { applying = false; }
                    GraphicsChanged();
                };
            }
            foreach (string name in CheckNames)
            {
                Control<CheckBox>(name).Checked += delegate { GraphicsChanged(); };
                Control<CheckBox>(name).Unchecked += delegate { GraphicsChanged(); };
            }

            foreach (string target in Pages) On("Nav" + target, delegate { Navigate(target); });
            On("GoSetup", delegate { Navigate("Setup"); });
            On("ProjectFolder", delegate { OpenFolder(project.Root); });
            On("OpenLogs", delegate { OpenFolder(project.Logs); });
            On("ChooseDisc", delegate { Choose("Disc", "Importing disc", "Xbox 360 disc images (*.iso;*.7z)|*.iso;*.7z", false); });
            On("ChooseUpdate", delegate { Choose("Update", "Importing TU1", "Title updates (*.zip;*.xexp)|*.zip;*.xexp|All files|*.*", false); });
            On("ChooseDlc", delegate { Choose("Dlc", "Importing DLC", "DLC archives (*.zip)|*.zip|Raw Xbox content packages|*.*", true); });
            On("DownloadDlss", delegate
            {
                Process.Start(new ProcessStartInfo(
                    "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip")
                    { UseShellExecute = true });
            });
            On("ImportDlss", ImportDlss);
            On("BuildPc", delegate
            {
                if (!project.IsBuilt) Start("Building PC version", ProjectTools.BuildCommand);
            });
            On("PrepareShaders", delegate
            {
                if (job == null)
                    BeginJob(BackgroundJob.StartShaderPreparation(project, ReadGraphics(), TemporalAvailable));
            });
            On("Play", delegate
            {
                if (job != null) return;
                long click = Stopwatch.GetTimestamp();
                applying = true;
                try
                {
                    LoadMonitors(Selected("DisplayMonitor"));
                    LoadResolutions();
                }
                finally { applying = false; }
                GraphicsSettings settings = ReadGraphics();
                Save();
                BeginJob(BackgroundJob.StartGame(project, settings, click));
            });
            On("LoadAchievements", delegate
            {
                if (File.Exists(project.Catalog)) LoadAchievements();
                else Start("Loading achievements", ProjectTools.AchievementsCommand);
            });

            timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(450) };
            timer.Tick += delegate
            {
                try { Poll(); }
                catch (Exception error)
                {
                    timer.Stop();
                    if (job != null) { job.Dispose(); job = null; }
                    Report(error);
                }
            };
            var screen = System.Windows.Forms.Screen.FromPoint(System.Windows.Forms.Cursor.Position);
            Window.SourceInitialized += delegate { Position(screen.WorkingArea); };
            Window.Closing += OnClosing;
            Window.Closed += delegate { timer.Stop(); if (job != null) job.Dispose(); };
            Navigate("Play");
            Refresh();
        }

        internal T Control<T>(string name) where T : FrameworkElement { return (T)Window.FindName(name); }

        private void SetBackdrop()
        {
            using (Stream art = typeof(LauncherWindow).Assembly.GetManifestResourceStream("Hero.jpg"))
            {
                if (art == null) return;
                var bitmap = new System.Windows.Media.Imaging.BitmapImage();
                bitmap.BeginInit();
                bitmap.StreamSource = art;
                bitmap.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                bitmap.EndInit();
                bitmap.Freeze();
                var brush = new System.Windows.Media.ImageBrush(bitmap)
                {
                    Stretch = System.Windows.Media.Stretch.UniformToFill,
                    AlignmentX = System.Windows.Media.AlignmentX.Right,
                    AlignmentY = System.Windows.Media.AlignmentY.Center
                };
                Control<Border>("Backdrop").Background = brush;
            }
        }

        private void On(string name, Action action)
        {
            Control<Button>(name).Click += delegate
            {
                try { action(); }
                catch (Exception error) { Report(error); }
            };
        }

        private void Add(string name, string tag, string label)
        {
            Control<ComboBox>(name).Items.Add(new ComboBoxItem { Tag = tag, Content = label });
        }

        private void Pick(string name, string tag, string fallback)
        {
            var combo = Control<ComboBox>(name);
            var options = combo.Items.Cast<ComboBoxItem>().ToArray();
            combo.SelectedItem = options.FirstOrDefault(x => (string)x.Tag == tag) ??
                options.FirstOrDefault(x => (string)x.Tag == fallback) ?? options.FirstOrDefault();
        }

        private string Selected(string name)
        {
            var item = Control<ComboBox>(name).SelectedItem as ComboBoxItem;
            return item == null ? "" : (string)item.Tag;
        }

        private bool Check(string name) { return Control<CheckBox>(name).IsChecked == true; }

        private void LoadMonitors(string preferred)
        {
            Control<ComboBox>("DisplayMonitor").Items.Clear();
            var monitors = DisplayModes.Monitors();
            if (monitors.Length == 0)
            {
                Add("DisplayMonitor", "", "Primary display");
                Pick("DisplayMonitor", "", "");
                return;
            }
            foreach (var monitor in monitors)
                Add("DisplayMonitor", monitor.Key, monitor.Label);
            Pick("DisplayMonitor", preferred, monitors[0].Key);
        }

        private void LoadResolutions()
        {
            string monitor = Selected("DisplayMonitor");
            string desktop = DisplayModes.DesktopResolution(monitor);
            Control<ComboBox>("Resolution").Items.Clear();
            if (Selected("DisplayMode") == "borderless")
            {
                Add("Resolution", desktop, desktop.Replace("x", " × ") + " (desktop)");
                Pick("Resolution", desktop, desktop);
                return;
            }
            foreach (string resolution in DisplayModes.Supported(monitor))
                Add("Resolution", resolution, resolution.Replace("x", " × "));
            Pick("Resolution", preferredResolution, desktop);
            preferredResolution = Selected("Resolution");
        }

        private void Apply(GraphicsSettings value)
        {
            applying = true;
            Pick("Renderer", value.Renderer, "d3d12");
            Pick("DisplayMode", value.DisplayMode, "windowed");
            preferredResolution = value.RenderResolution;
            LoadMonitors(value.DisplayMonitor);
            LoadResolutions();
            Pick("FrameRate", value.FrameRate.ToString(CultureInfo.InvariantCulture), "0");
            LoadUpscalers(value.Upscaler);
            Pick("DlssPreset", value.DlssPreset, "K");
            Pick("AntiAliasing", value.AntiAliasing, "none");
            Pick("TextureFiltering", value.TextureFiltering.ToString(CultureInfo.InvariantCulture), "3");
            Control<CheckBox>("VSync").IsChecked = value.VSync;
            Control<CheckBox>("Collector").IsChecked = value.GameplayCollectorEnabled;
            applying = false;
        }

        private void LoadUpscalers(string preferred)
        {
            Control<ComboBox>("Upscaler").Items.Clear();
            foreach (string provider in project.AvailableUpscalers(Selected("Renderer")))
                Add("Upscaler", provider, ProviderName(provider));
            Pick("Upscaler", preferred, "dlss");
        }

        private GraphicsSettings ReadGraphics()
        {
            return new GraphicsSettings
            {
                Renderer = Selected("Renderer"), RenderResolution = preferredResolution,
                DisplayMode = Selected("DisplayMode"), DisplayMonitor = Selected("DisplayMonitor"),
                Upscaler = Selected("Upscaler"),
                DlssPreset = Selected("DlssPreset"), AntiAliasing = Selected("AntiAliasing"),
                TextureFiltering = int.Parse(Selected("TextureFiltering"), CultureInfo.InvariantCulture),
                FrameRate = int.Parse(Selected("FrameRate"), CultureInfo.InvariantCulture),
                VSync = Check("VSync"),
                GameplayCollectorEnabled = Check("Collector")
            };
        }

        private void SetUpDlssNrPreview()
        {
            var passCount = Control<Slider>("NrPassCount");
            var passSelector = Control<ComboBox>("NrPassSelector");
            passSelector.SelectionChanged += delegate
            {
                if (passSelector.SelectedIndex >= 0) ShowDlssNrPass(passSelector.SelectedIndex);
            };
            passCount.ValueChanged += delegate { UpdateDlssNrPasses(); };
            Control<ComboBox>("NrPlacement").SelectionChanged += delegate { UpdateDlssNrResolution(ReadGraphics().OutputResolution); };
            Control<Slider>("NrWorkScale").ValueChanged += delegate { UpdateDlssNrResolution(ReadGraphics().OutputResolution); };
            On("NrResetPass", delegate
            {
                int pass = passSelector.SelectedIndex;
                nrPasses[pass] = new DlssNrPassPreview();
                ShowDlssNrPass(pass);
            });
            UpdateDlssNrPasses();
        }

        private void UpdateDlssNrPasses()
        {
            var selector = Control<ComboBox>("NrPassSelector");
            int count = (int)Control<Slider>("NrPassCount").Value;
            int selected = Math.Max(0, Math.Min(selector.SelectedIndex, count - 1));
            selector.ItemsSource = Enumerable.Range(1, count).Select(pass => "Pass " + pass).ToArray();
            selector.SelectedIndex = selected;
        }

        private void ShowDlssNrPass(int pass)
        {
            if (nrPasses[pass] == null) nrPasses[pass] = new DlssNrPassPreview();
            Control<FrameworkElement>("NrPassControls").DataContext = nrPasses[pass];
        }

        private void UpdateDlssNrResolution(string output)
        {
            bool upstream = Control<ComboBox>("NrPlacement").SelectedIndex == 1;
            string[] extent = (upstream ? "1280x720" : output).Split('x');
            int width, height;
            if (extent.Length != 2 || !int.TryParse(extent[0], out width) || !int.TryParse(extent[1], out height))
            {
                SetText("NrWorkSize", "Select an output resolution in Graphics.");
                return;
            }
            double scale = Control<Slider>("NrWorkScale").Value / 100;
            SetText("NrWorkSize", string.Format(CultureInfo.CurrentCulture, "{0:0} × {1:0} estimated",
                width * scale, height * scale));
        }

        private sealed class DlssNrPassPreview
        {
            public int Model { get; set; }
            public int Style { get; set; }
            public int Depth { get; set; }
            public double Intensity { get; set; }
            public double LocalTone { get; set; }
            public double GlobalTone { get; set; }
            public double LocalStructure { get; set; }
            public double SkinDetail { get; set; }
            public bool AutoMask { get; set; }
            public bool UiCorrection { get; set; }
            public double MotionX { get; set; }
            public double MotionY { get; set; }

            internal DlssNrPassPreview()
            {
                Intensity = 1;
                LocalTone = GlobalTone = LocalStructure = SkinDetail = 2;
                MotionX = MotionY = 1;
            }
        }

        private void GraphicsChanged()
        {
            if (applying) return;
            changed = true;
            if (job == null) status = null;
            Refresh();
        }

        private void Save()
        {
            project.SaveSettings(ReadGraphics());
            changed = false;
        }

        private void OnClosing(object sender, CancelEventArgs args)
        {
            if (!changed) return;
            try { Save(); }
            catch (Exception error)
            {
                args.Cancel = MessageBox.Show(Window, "Could not save launcher settings. " + error.Message +
                    "\n\nClose without saving?", Window.Title, MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.No;
            }
        }

        private void Navigate(string target)
        {
            page = target;
            foreach (string available in Pages)
            {
                Visible(available + "Page", available == target);
                Control<Button>("Nav" + available).Tag = available == target ? "active" : null;
            }
            if (target == "Achievements") LoadAchievements();
        }

        private void LoadAchievements()
        {
            var list = Control<ListBox>("AchievementList");
            if (!File.Exists(project.Catalog))
            {
                list.ItemsSource = null;
                SetText("AchievementSummary", project.HasDisc && project.HasUpdate
                    ? "Generate the achievement list from your imported game." : "Import the disc and title update to see achievements.");
                return;
            }
            try
            {
                var result = project.ReadAchievements();
                list.ItemsSource = result.Rows;
                SetText("AchievementSummary", result.Summary);
            }
            catch (Exception error)
            {
                list.ItemsSource = null;
                SetText("AchievementSummary", "Could not read achievements. " + error.Message);
            }
        }

        private void Refresh()
        {
            bool available = job == null;
            bool disc = project.HasDisc, update = project.HasUpdate, built = project.IsBuilt;
            bool ready = disc && update;
            int dlc = project.DlcCount();
            GraphicsSettings graphics = ReadGraphics();
            UpdateDlssNrResolution(graphics.OutputResolution);
            string upscaler = graphics.ActiveUpscaler(TemporalAvailable);
            bool cache = project.HasShaderCache(graphics.RuntimeRenderer, upscaler != "native");
            bool original = graphics.OutputResolution == "1280x720";
            Visible("UpscalerPanel", upscaler != "native");
            Visible("ModelPanel", upscaler == "dlss");
            Visible("AaPanel", upscaler == "native");
            Visible("AntiAliasing", !original);
            Visible("NativeAa", original);
            Visible("VulkanUpscalingNote", graphics.Renderer == "vulkan-temporal");

            foreach (string name in ComboNames.Concat(CheckNames).Concat(JobButtons))
                Control<Control>(name).IsEnabled = available;
            Control<ComboBox>("Resolution").IsEnabled = available && graphics.DisplayMode != "borderless";
            var build = Control<Button>("BuildPc");
            build.IsEnabled = available && ready && !built;
            build.Content = built ? "Installed" : "Build";
            build.Tag = built ? "installed" : null;
            Control<Button>("PrepareShaders").IsEnabled = available && ready && built && cache;
            var preparation = project.ShaderPreparationStatus(graphics, TemporalAvailable);
            Control<Button>("PrepareShaders").Content = preparation == ShaderPreparation.Missing
                ? "Precompile Shaders" : "Recompile Shaders";
            Control<Button>("LoadAchievements").IsEnabled = available && (ready || File.Exists(project.Catalog));
            Control<Button>("LoadAchievements").Content = File.Exists(project.Catalog) ? "Refresh" : "Load achievements";
            Control<Button>("Play").IsEnabled = available && ready && built;
            Visible("DlssSetup", !Control<ComboBox>("Upscaler").Items.Cast<ComboBoxItem>().Any(item => (string)item.Tag == "dlss"));
            Control<Button>("ImportDlss").IsEnabled = available;

            SetText("DiscState", disc ? "Disc files found" : "Select an ISO or a 7z archive");
            SetText("UpdateState", update ? "TU1 patch found" : "Select TU1 for media ID 11DFADD6");
            SetText("DlcState", dlc == 1 ? "1 package staged" : dlc + " packages staged");
            SetText("PlayHeading", built && ready ? "Ready to play" : ready ? "Ready to build" : "Set up your game");
            SetText("BuildState", built && ready ? "Your PC installation is ready." : ready
                ? "Disc and title update verified. Select Build to create your PC installation."
                : "Import your Xbox 360 disc and matching title update to continue.");
            Visible("GoSetup", !ready);
            SetText("CacheState", !cache ? "Play the game to record shaders for precompilation." :
                preparation == ShaderPreparation.Current ? "Recorded shaders prepared for this renderer." :
                preparation == ShaderPreparation.Stale ? "Shader preparation is out of date. Recompile shaders." :
                "Recorded shaders are ready to precompile.");
            SetText("Status", status ?? (ready && built ? "Ready to play" : ready ? "Ready to build" : "Setup required"));
            SetText("StatusDetail", !available ? "See Activity for output" : !ready ? "Import the disc and title update"
                : !built ? "Build the PC version in Game files"
                : Check("Collector") ? "Gameplay collector enabled" : "Gameplay collector off");
        }

        private void Choose(string kind, string title, string filter, bool multiple)
        {
            if (job != null) return;
            var dialog = new OpenFileDialog { Filter = filter, Multiselect = multiple, CheckFileExists = true };
            if (dialog.ShowDialog(Window) != true) return;
            string input = project.SaveImportSelection(dialog.FileNames);
            try { BeginJob(BackgroundJob.StartImport(project, title, kind, input)); }
            catch { DeleteSelection(input); throw; }
        }

        private void ImportDlss()
        {
            var dialog = new OpenFileDialog { Filter = "NVIDIA Streamline SDK (*.zip)|*.zip", CheckFileExists = true };
            if (dialog.ShowDialog(Window) != true) return;
            string input = project.SaveImportSelection(dialog.FileNames);
            try { BeginJob(BackgroundJob.StartWorker(project, "Installing DLSS runtime", new[] { ProjectTools.InstallDlssCommand, input }, input)); }
            catch { DeleteSelection(input); throw; }
        }

        private void Start(string title, string command)
        {
            if (job != null) return;
            BeginJob(BackgroundJob.StartWorker(project, title, new[] { command }));
        }

        private void BeginJob(BackgroundJob started)
        {
            job = started;
            status = job.Title + "…";
            Log(status);
            if (page != "Achievements") Navigate("Play");
            Refresh();
            timer.Start();
        }

        internal void Poll()
        {
            if (job == null) return;
            uint exitCode;
            bool finished = job.TryGetExitCode(out exitCode);
            string output = Tail(job.OutputFile, 80);
            string errors = Tail(job.ErrorFile, 20);
            if (job.IsGame)
            {
                string collector = output.Split('\n').LastOrDefault(line => line.StartsWith("Gameplay collector ", StringComparison.Ordinal));
                if (collector != null)
                {
                    if (collector.StartsWith("Gameplay collector active.", StringComparison.Ordinal)) status = "Collector active (L to mark)";
                    else if (collector.StartsWith("Gameplay collector stopped.", StringComparison.Ordinal)) status = "Collector stopped";
                    else if (collector.StartsWith("Gameplay collector unavailable.", StringComparison.Ordinal)) status = "Collector unavailable";
                    else if (collector.StartsWith("Gameplay collector disabled.", StringComparison.Ordinal)) status = "Running game (collector off)";
                }
                if (Directory.Exists(project.Logs))
                {
                    var log = new DirectoryInfo(project.Logs).EnumerateFiles("asuras_wrath_*.log")
                        .Where(x => x.LastWriteTimeUtc >= job.Started).OrderByDescending(x => x.LastWriteTimeUtc).FirstOrDefault();
                    if (log != null) output += Environment.NewLine + Tail(log.FullName, 30);
                }
                SetText("Status", status);
            }
            string combined = (output + Environment.NewLine + errors).Trim();
            if (combined.Length != 0) Log(combined);
            if (!finished) return;

            timer.Stop();
            string title = job.Title;
            DeleteSelection(job.InputFile);
            job.Dispose();
            job = null;
            status = exitCode != 0 ? "Operation failed (" + exitCode + ")" :
                title == "Running game" ? "Game closed" : "Operation complete";
            if (exitCode == 0 && title == "Installing DLSS runtime")
            {
                if (project.AvailableUpscalers(Selected("Renderer")).Contains("dlss"))
                {
                    applying = true;
                    try { LoadUpscalers(Selected("Upscaler")); }
                    finally { applying = false; }
                    status = "DLSS runtime installed";
                    Navigate("Graphics");
                }
                else status = "DLSS runtime unavailable";
            }
            Refresh();
            if (exitCode == 0 && page == "Achievements") LoadAchievements();
        }

        private static string Tail(string path, int lines)
        {
            try
            {
                using (var file = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (file.Length > 32768) file.Seek(-32768, SeekOrigin.End);
                    using (var reader = new StreamReader(file)) 
                    {
                        string[] rows = reader.ReadToEnd().TrimEnd().Split('\n');
                        return string.Join("\n", rows.Skip(Math.Max(0, rows.Length - lines)));
                    }
                }
            }
            catch (IOException) { return ""; }
        }

        private static void DeleteSelection(string path)
        {
            if (path == null) return;
            try { File.Delete(path); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        private static void OpenFolder(string path)
        {
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }

        private void Report(Exception error)
        {
            status = "Operation failed";
            Navigate("Play");
            Log(error.GetBaseException().Message);
            Refresh();
        }

        private void SetText(string name, string text) { Control<TextBlock>(name).Text = text; }
        private void Visible(string name, bool display)
        {
            Control<FrameworkElement>(name).Visibility = display ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Log(string text)
        {
            var output = Control<TextBox>("OutputLog");
            if (output.Text == text) return;
            output.Text = text;
            output.ScrollToEnd();
        }

        private static string ProviderName(string provider)
        {
            if (provider == "dlss") return "NVIDIA DLSS";
            if (provider == "fsr3") return "AMD FSR 3";
            return "Native";
        }

        private void Position(System.Drawing.Rectangle work)
        {
            IntPtr handle = new WindowInteropHelper(Window).Handle;
            const int ImmersiveDarkMode = 20;
            int on = 1;
            DwmSetWindowAttribute(handle, ImmersiveDarkMode, ref on, sizeof(int));
            RectPixels rect;
            if (!GetWindowRect(handle, out rect)) return;
            int width = Math.Min(rect.Right - rect.Left, work.Width);
            int height = Math.Min(rect.Bottom - rect.Top, work.Height);
            SetWindowPos(handle, IntPtr.Zero, work.Left + (work.Width - width) / 2,
                         work.Top + (work.Height - height) / 2, width, height, 0x0014);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RectPixels { public int Left, Top, Right, Bottom; }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr window, out RectPixels bounds);

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    }
}
