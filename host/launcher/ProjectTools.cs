using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace AsurasWrath.Launcher
{
    internal static class ProjectTools
    {
        internal const string BuildCommand = "--build";
        internal const string CodegenCommand = "--codegen";
        internal const string AchievementsCommand = "--achievements";
        internal const string InstallDlssCommand = "--install-dlss-worker";
        private const string StreamlineHash = "32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B";
        private static readonly string[] StreamlineFiles =
        {
            "license.txt", "3rd-party-licenses.md", "bin/x64/nvngx_dlss.license.txt",
            "bin/x64/sl.interposer.dll", "bin/x64/sl.common.dll", "bin/x64/sl.pcl.dll",
            "bin/x64/sl.dlss.dll", "bin/x64/nvngx_dlss.dll"
        };

        internal static bool Handles(string command)
        {
            return command == BuildCommand || command == CodegenCommand ||
                command == AchievementsCommand || command == InstallDlssCommand;
        }

        internal static int Run(Project project, string[] args)
        {
            string command = args[0];
            if (command == InstallDlssCommand)
            {
                if (args.Length != 2) throw new ArgumentException("Select one NVIDIA Streamline 2.14.16 x64 ZIP.");
                InstallDlss(project, args[1]);
                return 0;
            }
            int jobs = 8;
            if (args.Length > 1 && (command != BuildCommand || args.Length != 2 ||
                !int.TryParse(args[1], NumberStyles.None, CultureInfo.InvariantCulture, out jobs) || jobs < 1 || jobs > 32))
                throw new ArgumentException("Use --build with an optional job count from 1 to 32.");
            using (GameLaunchInputs.Lock(project.Output))
            {
                string game = Path.Combine(project.Root, @"local\game");
                GameLaunchInputs.VerifyAndInstallPatch(game, Path.Combine(project.Root, @"local\update"));
                string sdk = Path.Combine(project.Root, @".tools\rexglue-0.11.0\win-amd64");
                string rex = Path.Combine(sdk, @"bin\rexglue.exe");
                string manifest = Path.Combine(project.Root, Project.InstallManifest);
                if (command == CodegenCommand)
                    RunTool(project.Root, rex, new[] { "codegen", manifest, "--ignore-stamp" });
                else if (command == AchievementsCommand)
                    GenerateAchievements(project, rex, Path.Combine(game, "default.xex"));
                else Build(project, sdk, rex, manifest, jobs);
            }
            return 0;
        }

        private static void Build(Project project, string sdk, string rex, string manifest, int jobs)
        {
            string installer = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                @"Microsoft Visual Studio\Installer\vswhere.exe");
            if (!File.Exists(installer))
                throw new FileNotFoundException("Install Visual Studio Build Tools with the Desktop development with C++ workload.");
            string vs = ReadTool(project.Root, installer,
                Arguments("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath")).Trim();
            if (string.IsNullOrEmpty(vs))
                throw new InvalidOperationException("No Visual Studio installation with x64 C++ Build Tools was found.");
            string tools = Path.Combine(vs, @"Common7\IDE\CommonExtensions\Microsoft\CMake");
            string cmake = Path.Combine(tools, @"CMake\bin\cmake.exe");
            string ninja = Path.Combine(tools, @"Ninja\ninja.exe");
            if (!File.Exists(cmake) || !File.Exists(ninja))
                throw new FileNotFoundException("Add C++ CMake tools for Windows to your Visual Studio installation.");
            string clang = Path.Combine(project.Root, @".tools\llvm-20.1.8\bin");
            if (!File.Exists(Path.Combine(clang, "clang++.exe")))
                throw new FileNotFoundException("The pinned Clang 20.1.8 toolchain is missing.");
            if (!File.Exists(Path.Combine(project.Root, @"generated\default\sources.cmake")))
            {
                Console.WriteLine("Generating the PC source from your verified Xbox 360 game and update.");
                RunTool(project.Root, rex, new[] { "codegen", manifest });
            }
            Console.WriteLine("Loading the Visual Studio x64 build environment...");
            var environment = BuildEnvironment(project.Root, vs);
            environment["PATH"] = clang + ";" + environment["PATH"];
            RunTool(project.Root, cmake,
                new[] { "--preset", "win-amd64", "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_PREFIX_PATH=" + sdk },
                environment);
            var arguments = new List<string> { "--build", "--preset", "win-amd64-release", "--parallel",
                jobs.ToString(CultureInfo.InvariantCulture), "--target", "asuras_runtime" };
            bool checks = File.Exists(Path.Combine(project.Root, @"accuracy\guest_layout.cpp"));
            if (checks) arguments.AddRange(new[] { "guest_layout_check", "prompt_controller_check" });
            RunTool(project.Root, cmake, arguments, environment);
            if (!project.IsBuilt) throw new InvalidOperationException("The build did not produce the PC executable.");
            if (checks)
            {
                string release = Path.GetDirectoryName(project.GameExecutable);
                RunTool(project.Root, Path.Combine(release, "guest_layout_check.exe"), new string[0], environment);
                RunTool(project.Root, Path.Combine(release, "prompt_controller_check.exe"), new string[0], environment);
            }
        }

        private static Dictionary<string, string> BuildEnvironment(string root, string visualStudio)
        {
            string vsdev = Path.Combine(visualStudio, @"Common7\Tools\VsDevCmd.bat");
            if (!File.Exists(vsdev)) throw new FileNotFoundException("Visual Studio's VsDevCmd.bat is missing.");
            string cmd = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");
            string output = ReadTool(root, cmd,
                "/d /u /s /c \"call \"%WRATHFALL_VSDEVCMD%\" -arch=x64 -host_arch=x64 -no_logo >nul && set\"",
                new Dictionary<string, string> { { "WRATHFALL_VSDEVCMD", vsdev } }, Encoding.Unicode);
            var environment = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string line in output.Split('\n'))
            {
                int separator = line.IndexOf('=');
                if (separator > 0) environment[line.Substring(0, separator)] = line.Substring(separator + 1).TrimEnd('\r');
            }
            environment.Remove("WRATHFALL_VSDEVCMD");
            if (!environment.ContainsKey("PATH") || !environment.ContainsKey("INCLUDE") || !environment.ContainsKey("LIB"))
                throw new InvalidOperationException("Visual Studio did not provide a complete x64 build environment.");
            return environment;
        }

        private static void GenerateAchievements(Project project, string rex, string game)
        {
            string target = Path.GetDirectoryName(project.Catalog);
            string staging = target + "." + Guid.NewGuid().ToString("N");
            string previous = staging + ".previous";
            try
            {
                RunTool(project.Root, rex, new[] { "init", "achievements", game, staging });
                string catalog = Path.Combine(staging, Path.GetFileName(project.Catalog));
                if (!File.Exists(catalog) || !File.ReadLines(catalog).Any(line => line.Trim() == "[[achievements]]"))
                    throw new InvalidDataException("ReXGlue did not produce a valid achievements catalog.");
                if (Directory.Exists(target)) Directory.Move(target, previous);
                try { Directory.Move(staging, target); }
                catch
                {
                    if (Directory.Exists(previous) && !Directory.Exists(target)) Directory.Move(previous, target);
                    throw;
                }
                RemoveStaging(previous);
                Console.WriteLine("Achievement catalog generated.");
            }
            finally { RemoveStaging(staging); }
        }

        private static void InstallDlss(Project project, string selection)
        {
            object selected = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(selection));
            var paths = selected as object[] ?? new[] { selected };
            string path = paths.Length == 1 ? paths[0] as string : null;
            if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Select one NVIDIA Streamline 2.14.16 x64 ZIP.");
            using (var stream = File.OpenRead(path))
            {
                using (var sha = SHA256.Create())
                {
                    string actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
                    if (actual != StreamlineHash)
                        throw new InvalidDataException("This ZIP does not match the official NVIDIA Streamline 2.14.16 x64 release. Download it from the Graphics page.");
                }
                stream.Position = 0;
                using (var archive = new ZipArchive(stream, ZipArchiveMode.Read, true))
                using (GameLaunchInputs.Lock(project.Output))
                {
                    string root = Directory.GetParent(Directory.GetParent(project.VendorRuntime("dlss")).FullName).FullName;
                    string staging = Path.Combine(project.Output, "dlss-import-" + Guid.NewGuid().ToString("N"));
                    bool preserveBackup = false;
                    try
                    {
                        foreach (string name in StreamlineFiles)
                        {
                            var entry = archive.GetEntry(name);
                            if (entry == null) throw new InvalidDataException("The NVIDIA archive is missing " + name + ".");
                            string target = Path.Combine(staging, "files", name);
                            Directory.CreateDirectory(Path.GetDirectoryName(target));
                            entry.ExtractToFile(target);
                            if (new FileInfo(target).Length != entry.Length) throw new IOException("Incomplete NVIDIA file " + name + ".");
                        }
                        PublishDlssFiles(root, staging, ref preserveBackup);
                    }
                    finally { if (!preserveBackup) RemoveStaging(staging); }
                }
            }
            Console.WriteLine("NVIDIA DLSS runtime installed.");
        }

        private static void PublishDlssFiles(string root, string staging, ref bool preserveBackup)
        {
            var published = new List<string>();
            try
            {
                foreach (string name in StreamlineFiles)
                {
                    string target = Path.Combine(root, name);
                    string file = Path.Combine(staging, "files", name);
                    string backup = Path.Combine(staging, "previous", name);
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    if (File.Exists(target))
                    {
                        Directory.CreateDirectory(Path.GetDirectoryName(backup));
                        File.Replace(file, target, backup);
                    }
                    else File.Move(file, target);
                    published.Add(name);
                }
            }
            catch (Exception installError)
            {
                Exception restoreError = null;
                for (int i = published.Count - 1; i >= 0; --i)
                {
                    string name = published[i], target = Path.Combine(root, name);
                    string backup = Path.Combine(staging, "previous", name);
                    try
                    {
                        if (File.Exists(backup)) File.Replace(backup, target, null);
                        else File.Delete(target);
                    }
                    catch (Exception error) { restoreError = error; }
                }
                if (restoreError != null)
                {
                    preserveBackup = true;
                    throw new IOException("DLSS installation could not restore every file. Previous files are in " +
                        Path.Combine(staging, "previous") + ". " + restoreError.Message, installError);
                }
                throw;
            }
        }

        private static void RemoveStaging(string folder)
        {
            var directory = new DirectoryInfo(folder);
            if (!directory.Exists || (directory.Attributes & FileAttributes.ReparsePoint) != 0) return;
            try { directory.Delete(true); }
            catch (IOException error) { Console.WriteLine("Could not remove temporary build data. " + error.Message); }
            catch (UnauthorizedAccessException error) { Console.WriteLine("Could not remove temporary build data. " + error.Message); }
        }

        private static string Arguments(params string[] args) { return string.Join(" ", args.Select(BackgroundJob.Quote)); }

        private static ProcessStartInfo Tool(string root, string executable, string arguments,
                                            Dictionary<string, string> environment)
        {
            if (!File.Exists(executable)) throw new FileNotFoundException("Required tool is missing: " + executable);
            var start = new ProcessStartInfo(executable, arguments)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root, RedirectStandardOutput = true
            };
            if (environment != null)
                foreach (var variable in environment) start.EnvironmentVariables[variable.Key] = variable.Value;
            return start;
        }

        private static string ReadTool(string root, string executable, string arguments,
                                       Dictionary<string, string> environment = null, Encoding encoding = null)
        {
            var start = Tool(root, executable, arguments, environment);
            if (encoding != null) start.StandardOutputEncoding = encoding;
            using (var process = Process.Start(start))
            {
                string output = process.StandardOutput.ReadToEnd();
                WaitForTool(process, executable);
                return output;
            }
        }

        private static void RunTool(string root, string executable, IEnumerable<string> arguments,
                                    Dictionary<string, string> environment = null)
        {
            var start = Tool(root, executable, string.Join(" ", arguments.Select(BackgroundJob.Quote)), environment);
            start.RedirectStandardError = true;
            using (var process = new Process { StartInfo = start })
            {
                process.OutputDataReceived += (sender, row) => { if (row.Data != null) Console.WriteLine(row.Data); };
                process.ErrorDataReceived += (sender, row) => { if (row.Data != null) Console.Error.WriteLine(row.Data); };
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                WaitForTool(process, executable);
            }
        }

        private static void WaitForTool(Process process, string executable)
        {
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new InvalidOperationException(Path.GetFileName(executable) + " exited with code " +
                    process.ExitCode.ToString(CultureInfo.InvariantCulture) + ".");
        }
    }
}
