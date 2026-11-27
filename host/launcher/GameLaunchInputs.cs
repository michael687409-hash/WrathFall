using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace AsurasWrath.Launcher
{
    internal static class GameLaunchInputs
    {
        // Keep these title and TU1 checks in sync with ProjectInputs.ps1 used by the importer.
        private const string DiscHash = "C7E324A807A4407239B8FBFABE4377C0623889DAA772EADAC42C8B8F86782479";
        private const string PatchHash = "918CDEE90389F27DEE0BFE1BDFD758BC52CC4148666DCAE1E35A1C0297F2005C";

        internal static FileStream Lock(string output)
        {
            Directory.CreateDirectory(output);
            try { return File.Open(Path.Combine(output, ".active"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException error)
            {
                throw new IOException("Another import, build, or game session is using this folder.", error);
            }
        }

        internal static void VerifyAndInstallPatch(string game, string update)
        {
            if (!HasSupportedDisc(game) || !HasMatchingTitleUpdate(update))
                throw new InvalidDataException(
                    "Expected the verified base disc and TU1 pair for media ID 11DFADD6. Import or verify them in the launcher.");
            InstallVerifiedPatch(game, update);
        }

        internal static bool HasSupportedDisc(string game)
        {
            return Matches(Path.Combine(game, "default.xex"), DiscHash);
        }

        internal static bool HasMatchingTitleUpdate(string update)
        {
            return Matches(Path.Combine(update, "default.xexp"), PatchHash);
        }

        internal static void InstallPatchIfReady(string game, string update)
        {
            if (HasSupportedDisc(game) && HasMatchingTitleUpdate(update)) InstallVerifiedPatch(game, update);
        }

        private static void InstallVerifiedPatch(string game, string update)
        {
            string source = Path.Combine(update, "default.xexp");
            string target = Path.Combine(game, "default.xexp");
            if (File.Exists(target))
            {
                if (!Matches(target, PatchHash)) throw new InvalidDataException("An unexpected patch already exists at " + target + ".");
                return;
            }

            AtomicWrite(target, pending =>
            {
                File.Copy(source, pending);
                if (!Matches(pending, PatchHash)) throw new InvalidDataException("Could not verify the copied TU1 patch.");
            });
            Console.WriteLine("TU1 is ready beside the base executable.");
        }

        internal static string PreparePlayerData(Project project)
        {
            string target = Path.Combine(project.Root, "userdata");
            if (Directory.Exists(target)) return target;
            string legacy = Path.Combine(project.Output, "user");
            if (!Directory.Exists(legacy)) return Directory.CreateDirectory(target).FullName;

            // Publish the migrated profile only after verifying every copied file.
            string pending = target + ".pending." + Guid.NewGuid().ToString("N");
            try
            {
                CopyProfile(new DirectoryInfo(legacy), pending);
                Directory.Move(pending, target);
                return target;
            }
            finally
            {
                if (Directory.Exists(pending)) Directory.Delete(pending, true);
            }
        }

        private static void CopyProfile(DirectoryInfo source, string destination)
        {
            if ((source.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Could not migrate a linked profile directory. The original is in " + source.FullName + ".");
            Directory.CreateDirectory(destination);
            foreach (FileInfo file in source.EnumerateFiles())
            {
                string target = Path.Combine(destination, file.Name);
                file.CopyTo(target);
                if (!Matches(target, Hash(file.FullName)))
                    throw new IOException("Could not verify the copied player file " + file.FullName + ".");
            }
            foreach (DirectoryInfo directory in source.EnumerateDirectories())
                CopyProfile(directory, Path.Combine(destination, directory.Name));
        }

        private static bool Matches(string path, string expected)
        {
            return File.Exists(path) && string.Equals(Hash(path), expected, StringComparison.OrdinalIgnoreCase);
        }

        internal static string Hash(string path)
        {
            using (var sha = SHA256.Create())
            using (var file = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "");
        }

        internal static void AtomicWrite(string target, Action<string> write)
        {
            string pending = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                write(pending);
                if (File.Exists(target)) File.Replace(pending, target, null);
                else File.Move(pending, target);
            }
            finally { if (File.Exists(pending)) File.Delete(pending); }
        }

        internal static string PrepareGraphicsOverride(string game, string cache, bool temporal, bool disableSmoothing)
        {
            string source = Path.Combine(game, @"BCGame\CookedXbox360");
            var configs = Directory.Exists(source) ? Directory.GetFiles(source, "Coalesced_*.bin") : new string[0];
            if (!configs.Any()) throw new FileNotFoundException("The game is missing its compiled graphics configuration.");
            string target = Path.Combine(cache, temporal ? "blur-off-temporal" : "motion-blur-off");
            Directory.CreateDirectory(target);
            foreach (string config in configs)
            {
                byte[] sourceBytes = File.ReadAllBytes(config);
                byte[] patched = PatchGraphicsConfig(sourceBytes, config, temporal, disableSmoothing);
                AtomicWrite(Path.Combine(target, Path.GetFileName(config)), pending => File.WriteAllBytes(pending, patched));
            }
            return target;
        }

        internal static byte[] PatchGraphicsConfig(byte[] bytes, string source, bool keepVelocity, bool disableSmoothing)
        {
            using (var input = new MemoryStream(bytes, false))
            using (var output = new MemoryStream(bytes.Length))
            using (var reader = new BinaryReader(input))
            using (var writer = new BinaryWriter(output))
            {
                int remaining = 0;
                var fields = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                int files = ReadCount(reader);
                for (int f = 0; f < files; ++f)
                {
                    string name = ReadString(reader);
                    int sections = ReadCount(reader);
                    for (int s = 0; s < sections; ++s)
                    {
                        string section = ReadString(reader);
                        int count = ReadCount(reader);
                        for (int k = 0; k < count; ++k)
                        {
                            string key = ReadString(reader);
                            int start = (int)input.Position;
                            string value = ReadString(reader);
                            bool blur = Same(section, "SystemSettings") && (Same(key, "MotionBlur") || Same(key, "AllowRadialBlur"));
                            bool smoothing = disableSmoothing && Same(section, "Engine.GameEngine") && Same(key, "bSmoothFrameRate");
                            if (!name.EndsWith(@"\Xbox360-BCEngine.ini", StringComparison.OrdinalIgnoreCase) || !(blur || smoothing))
                                continue;
                            if ((!Same(value, "True") && !Same(value, "False")) || !fields.Add(key))
                                throw new InvalidDataException("Unexpected " + key + " setting in " + source);
                            if (keepVelocity && Same(key, "MotionBlur")) continue;
                            output.Write(bytes, remaining, start - remaining);
                            // UE3 FString uses a big-endian negative character count for UTF-16LE.
                            writer.Write(IPAddress.HostToNetworkOrder(-6));
                            writer.Write(Encoding.Unicode.GetBytes("False\0"));
                            remaining = (int)input.Position;
                        }
                    }
                }
                if (input.Position != bytes.Length || fields.Count != (disableSmoothing ? 3 : 2))
                    throw new InvalidDataException("Could not identify the Xbox 360 graphics settings in " + source);
                output.Write(bytes, remaining, bytes.Length - remaining);
                return output.ToArray();
            }
        }

        private static bool Same(string left, string right) { return string.Equals(left, right, StringComparison.OrdinalIgnoreCase); }

        private static int ReadCount(BinaryReader reader)
        {
            int count = IPAddress.NetworkToHostOrder(reader.ReadInt32());
            if (count < 0) throw new InvalidDataException("Unexpected record count in the Xbox 360 configuration.");
            return count;
        }

        private static string ReadString(BinaryReader reader)
        {
            int length = IPAddress.NetworkToHostOrder(reader.ReadInt32());
            if (length == 0) return "";
            long size = length < 0 ? -2L * length : length;
            if (size > reader.BaseStream.Length - reader.BaseStream.Position)
                throw new EndOfStreamException("Unexpected end of the Xbox 360 configuration.");
            return (length < 0 ? Encoding.Unicode : Encoding.ASCII).GetString(reader.ReadBytes((int)size)).TrimEnd('\0');
        }
    }
}
