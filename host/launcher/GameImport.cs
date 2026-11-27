using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace AsurasWrath.Launcher
{
    internal static class GameImport
    {
        internal const string WorkerCommand = "--import-worker";
        private static readonly Regex UpdatePackage = new Regex(@"\A(?:Cache/)?TU_[A-Z0-9_.]+\z",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Regex DlcPackage = new Regex(@"\A(?:43430817/00000002/)?[A-Fa-f0-9]{42}\z");
        private static readonly Regex DlcName = new Regex(@"\A[A-F0-9]{42}\z");

        internal static int Run(Project project, string[] args)
        {
            if (args.Length != 3) throw new ArgumentException("Select the type of game file and at least one file.");
            var kind = args[1];
            if (!string.Equals(kind, "Disc", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(kind, "Update", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(kind, "Dlc", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Select a disc, title update, or DLC package.");

            object loaded = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(args[2]));
            var selection = loaded as object[] ?? new[] { loaded };
            var paths = selection.Select(value => value as string).ToArray();
            if (paths.Length == 0 || paths.Any(string.IsNullOrWhiteSpace))
                throw new ArgumentException("Select at least one file.");
            if (!string.Equals(kind, "Dlc", StringComparison.OrdinalIgnoreCase) && paths.Length != 1)
                throw new ArgumentException("Select one disc or update file at a time.");

            string local = Path.GetFullPath(Path.Combine(project.Root, "local"));
            string game = Path.Combine(local, "game");
            string update = Path.Combine(local, "update");
            string dlc = Path.Combine(local, "dlc");
            using (GameLaunchInputs.Lock(project.Output))
            {
                string staging = Path.Combine(local, "import-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(staging);
                try
                {
                    for (int i = 0; i < paths.Length; ++i)
                    {
                        string path = paths[i];
                        string input = Path.GetFullPath(path);
                        if (!File.Exists(input)) throw new FileNotFoundException("Expected a file: " + path);
                        string item = Path.Combine(staging, i.ToString(CultureInfo.InvariantCulture));
                        Directory.CreateDirectory(item);
                        if (kind.Equals("Disc", StringComparison.OrdinalIgnoreCase)) ImportDisc(project, input, item, game);
                        else if (kind.Equals("Update", StringComparison.OrdinalIgnoreCase)) ImportUpdate(project, input, item, update);
                        else ImportDlc(input, item, dlc);
                    }
                    GameLaunchInputs.InstallPatchIfReady(game, update);
                }
                finally { CleanStaging(staging, local); }
            }
            return 0;
        }

        private static void ImportDisc(Project project, string input, string staging, string game)
        {
            if (GameLaunchInputs.HasSupportedDisc(game))
            {
                Console.WriteLine("The supported base game is already staged.");
                return;
            }
            EnsureUnoccupied(game, "game");
            if (HasExtension(input, ".7z"))
                input = ExpandDiscArchive(project, input, staging);
            else if (!HasExtension(input, ".iso"))
                throw new InvalidDataException("Select an Xbox 360 ISO or a 7z archive containing the ISO.");

            string expanded = Path.Combine(staging, "disc");
            Console.WriteLine("Unpacking the Xbox 360 disc...");
            ExpandContainer(project, input, expanded);
            if (!GameLaunchInputs.HasSupportedDisc(expanded))
                throw new InvalidDataException("This project targets the disc revision with media ID 11DFADD6.");
            Directory.Move(expanded, game);
            Console.WriteLine("Verified and imported the Xbox 360 disc.");
        }

        private static void ImportUpdate(Project project, string input, string staging, string update)
        {
            if (GameLaunchInputs.HasMatchingTitleUpdate(update))
            {
                Console.WriteLine("The matching TU1 patch is already staged.");
                return;
            }
            EnsureUnoccupied(update, "update");
            if (HasExtension(input, ".zip"))
                input = ExpandZipPackage(input, UpdatePackage, staging);
            string expanded = Path.Combine(staging, "update");
            if (HasExtension(input, ".xexp"))
            {
                Directory.CreateDirectory(expanded);
                File.Copy(input, Path.Combine(expanded, "default.xexp"));
            }
            else ExpandContainer(project, input, expanded);
            if (!GameLaunchInputs.HasMatchingTitleUpdate(expanded))
                throw new InvalidDataException("The update does not match this disc revision. Use TU1 for media ID 11DFADD6.");
            Directory.Move(expanded, update);
            Console.WriteLine("Verified and imported Asura's Wrath TU1.");
        }

        private static void ImportDlc(string input, string staging, string dlc)
        {
            if (HasExtension(input, ".zip"))
                input = ExpandZipPackage(input, DlcPackage, staging);
            string name = Path.GetFileName(input).ToUpperInvariant();
            if (!DlcName.IsMatch(name))
                throw new InvalidDataException("Use the original 42-character Xbox content package filename.");
            VerifyDlcHeader(input);
            Directory.CreateDirectory(dlc);
            string target = Path.Combine(dlc, name);
            if (File.Exists(target))
            {
                if (GameLaunchInputs.Hash(input) != GameLaunchInputs.Hash(target))
                    throw new IOException("A different package already uses " + name + ".");
                Console.WriteLine("Already imported " + name + ".");
                return;
            }
            string pending = Path.Combine(staging, name + ".ready");
            File.Copy(input, pending);
            VerifyDlcHeader(pending);
            File.Move(pending, target);
            Console.WriteLine("Imported " + name + ".");
        }

        private static string ExpandDiscArchive(Project project, string archive, string staging)
        {
            string tar = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
            string entry;
            using (var process = StartTool(project, tar, true, "-tf", archive))
            {
                string listing = process.StandardOutput.ReadToEnd().TrimEnd('\r', '\n');
                process.WaitForExit();
                if (process.ExitCode != 0 || !Regex.IsMatch(listing, @"\A[^/\\:\r\n]+\.iso\z", RegexOptions.IgnoreCase))
                    throw new InvalidDataException("Select a 7z archive containing one Xbox 360 disc ISO.");
                entry = listing;
            }
            // Stream the archived ISO to our own name. The archive cannot choose an output path.
            string output = Path.Combine(staging, "disc.iso");
            Console.WriteLine("Extracting the ISO from the disc archive...");
            using (var file = File.Open(output, FileMode.CreateNew, FileAccess.Write))
            using (var process = StartTool(project, tar, true, "-xOf", archive, "--", entry))
            {
                process.StandardOutput.BaseStream.CopyTo(file, 1024 * 1024);
                process.WaitForExit();
                if (process.ExitCode != 0 || file.Length == 0)
                    throw new IOException("Disc archive extraction failed.");
            }
            return output;
        }

        private static string ExpandZipPackage(string archive, Regex pattern, string staging)
        {
            using (var zip = ZipFile.OpenRead(archive))
            {
                var packages = zip.Entries.Where(entry => pattern.IsMatch(entry.FullName)).Take(2).ToArray();
                if (packages.Length != 1) throw new InvalidDataException("Expected one matching package in " + archive + ".");
                var package = packages[0];
                string target = Path.Combine(staging, package.Name);
                package.ExtractToFile(target);
                if (new FileInfo(target).Length != package.Length) throw new IOException("Incomplete package: " + archive);
                return target;
            }
        }

        private static void VerifyDlcHeader(string path)
        {
            var header = new byte[0x364];
            using (var input = File.OpenRead(path))
            {
                int offset = 0;
                while (offset < header.Length)
                {
                    int count = input.Read(header, offset, header.Length - offset);
                    if (count == 0) throw new InvalidDataException("Incomplete DLC header: " + path);
                    offset += count;
                }
            }
            string magic = Encoding.ASCII.GetString(header, 0, 4);
            if ((magic != "LIVE" && magic != "PIRS" && magic != "CON ") ||
                BitConverter.ToString(header, 0x344, 4) != "00-00-00-02" ||
                BitConverter.ToString(header, 0x360, 4) != "43-43-08-17")
                throw new InvalidDataException("Expected Asura's Wrath Xbox 360 DLC: " + path);
        }

        private static void ExpandContainer(Project project, string source, string destination)
        {
            string rex = Path.Combine(project.Root, @".tools\rexglue-0.11.0\win-amd64\bin\rexglue.exe");
            using (var process = StartTool(project, rex, false, "vfs", "dump", source, destination))
            {
                process.WaitForExit();
                if (process.ExitCode != 0) throw new IOException("ReXGlue could not unpack " + source + ".");
            }
        }

        private static Process StartTool(Project project, string executable, bool redirectOutput, params string[] args)
        {
            if (!File.Exists(executable))
                throw new FileNotFoundException("Required import tool is missing: " + executable);
            var start = new ProcessStartInfo(executable, string.Join(" ", args.Select(BackgroundJob.Quote)))
            {
                WorkingDirectory = project.Root, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = redirectOutput
            };
            var process = Process.Start(start);
            if (process == null) throw new IOException("Could not start " + Path.GetFileName(executable) + ".");
            return process;
        }

        private static bool HasExtension(string path, string extension)
        {
            return string.Equals(Path.GetExtension(path), extension, StringComparison.OrdinalIgnoreCase);
        }

        private static void EnsureUnoccupied(string path, string kind)
        {
            if (Directory.Exists(path) || File.Exists(path))
                throw new IOException("Move the existing " + kind + " folder before importing: " + path);
        }

        private static void CleanStaging(string staging, string local)
        {
            var directory = new DirectoryInfo(staging);
            if (!directory.Exists || (directory.Attributes & FileAttributes.ReparsePoint) != 0 ||
                !string.Equals(directory.Parent.FullName, local, StringComparison.OrdinalIgnoreCase)) return;
            try { directory.Delete(true); }
            catch (IOException error) { Console.WriteLine("Could not clean the temporary import folder. " + error.Message); }
            catch (UnauthorizedAccessException error) { Console.WriteLine("Could not clean the temporary import folder. " + error.Message); }
        }
    }
}
