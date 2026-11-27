using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace AsurasWrath.Launcher
{
    internal sealed class BackgroundJob : IDisposable
    {
        private readonly SafeWaitHandle process;
        public readonly string Title, OutputFile, ErrorFile, InputFile;
        public readonly DateTime Started = DateTime.UtcNow;
        public readonly bool IsGame;

        private BackgroundJob(SafeWaitHandle process, string title, string output, string error,
                              string input, bool isGame)
        {
            this.process = process;
            Title = title;
            OutputFile = output;
            ErrorFile = error;
            InputFile = input;
            IsGame = isGame;
        }

        public static BackgroundJob StartGame(Project project, GraphicsSettings settings, long click)
        {
            return StartWorker(project, "Running game",
                new[] { GameLaunch.WorkerCommand, GameLaunch.Serialize(settings), click.ToString(CultureInfo.InvariantCulture) },
                null, true);
        }

        public static BackgroundJob StartShaderPreparation(Project project, GraphicsSettings settings, bool temporal)
        {
            var request = new ShaderPreparationRequest { Renderer = settings.RuntimeRenderer, Upscaler = settings.ActiveUpscaler(temporal) };
            return StartWorker(project, "Preparing recorded shaders",
                new[] { GameLaunch.ShaderWorkerCommand, GameLaunch.SerializePreparation(request) });
        }

        public static BackgroundJob StartImport(Project project, string title, string kind, string input)
        {
            return StartWorker(project, title, new[] { GameImport.WorkerCommand, kind, input }, input);
        }

        internal static BackgroundJob StartWorker(Project project, string title, IEnumerable<string> arguments,
                                                  string input = null, bool isGame = false)
        {
            string logDirectory = project.Logs;
            Directory.CreateDirectory(logDirectory);
            string stem = "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + "-" +
                Guid.NewGuid().ToString("N").Substring(0, 6);
            string output = Path.Combine(logDirectory, stem + ".log");
            string error = Path.Combine(logDirectory, stem + ".err.log");
            return StartProcess(project.Root, title, typeof(Program).Assembly.Location, arguments, output, error, input, isGame);
        }

        internal static BackgroundJob StartCollector(string root, string executable, IEnumerable<string> arguments,
                                                     string output, string error)
        {
            return StartProcess(root, "gameplay collector", executable, arguments, output, error, null, false);
        }

        private static BackgroundJob StartProcess(string root, string title, string executable,
            IEnumerable<string> arguments, string output, string error, string input, bool isGame)
        {
            using (var stdin = OpenHandle("NUL", 0x80000000, 3))
            using (var stdout = OpenHandle(output, 0x40000000, 1))
            using (var stderr = OpenHandle(error, 0x40000000, 1))
            {
                var startup = new StartupInfo
                {
                    Size = Marshal.SizeOf(typeof(StartupInfo)),
                    Flags = 0x100, // STARTF_USESTDHANDLES
                    Input = stdin.DangerousGetHandle(),
                    Output = stdout.DangerousGetHandle(),
                    Error = stderr.DangerousGetHandle()
                };
                ProcessInformation created;
                const uint CreateNoWindow = 0x08000000;
                string command = Quote(executable) + " " + string.Join(" ", arguments.Select(Quote));
                if (!CreateProcess(executable, new StringBuilder(command), IntPtr.Zero, IntPtr.Zero, true,
                                   CreateNoWindow, IntPtr.Zero, root, ref startup, out created))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Unable to start " + title + ".");
                CloseHandle(created.Thread);
                return new BackgroundJob(new SafeWaitHandle(created.Process, true), title, output, error, input, isGame);
            }
        }

        public bool TryGetExitCode(out uint code)
        {
            code = 0;
            uint result = WaitForSingleObject(process, 0);
            if (result == 258) return false;
            if (result != 0 || !GetExitCodeProcess(process, out code))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return true;
        }

        internal void WaitForExit(uint milliseconds)
        {
            uint result = WaitForSingleObject(process, milliseconds);
            if (result != 0 && result != 258) throw new Win32Exception(Marshal.GetLastWin32Error());
        }

        public void Dispose() { process.Dispose(); }

        // CreateProcess receives a Windows command line. Double the backslashes
        // that precede a quote, including the closing quote after a trailing slash.
        internal static string Quote(string argument)
        {
            if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '"' }) < 0)
                return argument;
            var text = new StringBuilder("\"");
            int slashes = 0;
            foreach (char ch in argument)
            {
                if (ch == '\\') { ++slashes; continue; }
                text.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes);
                text.Append(ch);
                slashes = 0;
            }
            return text.Append('\\', slashes * 2).Append('"').ToString();
        }

        private static SafeFileHandle OpenHandle(string path, uint access, uint disposition)
        {
            var security = new SecurityAttributes { Size = Marshal.SizeOf(typeof(SecurityAttributes)), Inherit = 1 };
            var handle = CreateFile(path, access, 7, ref security, disposition, 0x80, IntPtr.Zero);
            if (!handle.IsInvalid) return handle;
            int code = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new Win32Exception(code, "Unable to open " + path);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes { public int Size; public IntPtr Descriptor; public int Inherit; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int Size;
            public string Reserved, Desktop, Title;
            public int X, Y, Width, Height, Columns, Rows, Fill, Flags;
            public short ShowWindow, ReservedBytes;
            public IntPtr ReservedPointer, Input, Output, Error;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }

        [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
            ref SecurityAttributes security, uint disposition, uint attributes, IntPtr template);

        [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcess(string application, StringBuilder command, IntPtr processSecurity,
            IntPtr threadSecurity, bool inherit, uint flags, IntPtr environment, string directory,
            ref StartupInfo startup, out ProcessInformation process);

        [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(SafeWaitHandle handle, uint ms);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetExitCodeProcess(SafeWaitHandle handle, out uint code);
    }
}
