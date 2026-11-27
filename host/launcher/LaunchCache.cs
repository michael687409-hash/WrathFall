using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;

namespace AsurasWrath.Launcher
{
    internal static class LaunchCache
    {
        internal static void Materialize(string directory, IEnumerable<string> sources, string recipe,
                                         Action<string, string> create)
        {
            Directory.CreateDirectory(directory);
            string index = Path.Combine(directory, ".launch-cache.json");
            var serializer = new JavaScriptSerializer();
            var previous = ReadIndex(index, serializer);
            var current = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            bool changed = false;
            foreach (string source in sources)
            {
                string name = Path.GetFileName(source);
                string target = Path.Combine(directory, name);
                string sourceStamp = Stamp(source);
                string targetStamp = File.Exists(target) ? Stamp(target) : null;
                string key = CacheKey(recipe, source, sourceStamp, targetStamp);
                string saved;
                if (key == null || !previous.TryGetValue(name, out saved) || key != saved)
                {
                    GameLaunchInputs.AtomicWrite(target, pending => create(source, pending));
                    if (sourceStamp != null && sourceStamp != Stamp(source))
                        throw new IOException("A launch input changed while preparing it. Try Play again. " + source);
                    key = CacheKey(recipe, source, sourceStamp, Stamp(target));
                    changed = true;
                }
                if (key != null) current.Add(name, key);
            }
            if (!changed && previous.Count == current.Count) return;
            try { GameLaunchInputs.AtomicWrite(index, pending => File.WriteAllText(pending, serializer.Serialize(current))); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        private static Dictionary<string, string> ReadIndex(string path, JavaScriptSerializer serializer)
        {
            try
            {
                if (File.Exists(path))
                    return serializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(path))
                        ?? new Dictionary<string, string>();
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            catch (ArgumentException) { }
            catch (InvalidOperationException) { }
            return new Dictionary<string, string>();
        }

        private static string CacheKey(string recipe, string source, string input, string output)
        {
            return input == null || output == null ? null : string.Join("\n", recipe, source, input, output);
        }

        private static string Stamp(string path)
        {
            using (var file = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                FileBasicInfo info;
                // Check the filesystem change time as well as last write time. A copied file can preserve last write time.
                if (!GetFileInformationByHandleEx(file.SafeFileHandle, 0, out info,
                    (uint)Marshal.SizeOf(typeof(FileBasicInfo))) || info.ChangeTime <= 0) return null;
                return string.Format(CultureInfo.InvariantCulture, "{0}:{1}:{2}:{3}",
                    file.Length, info.CreationTime, info.LastWriteTime, info.ChangeTime);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct FileBasicInfo
        {
            public long CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
            public uint FileAttributes;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int informationClass,
                                                                out FileBasicInfo information, uint size);
    }
}
