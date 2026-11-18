using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using FILETIME = System.Runtime.InteropServices.ComTypes.FILETIME;

[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]

namespace TDock {
    internal static class ExplorerRestart {
        internal static void Restart(string[] lockedFiles) {
            int pid = ShellProcessId();
            string explorerPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe");
            if (pid == 0) throw new IOException("找不到当前桌面的 Windows 资源管理器。");

            using (var shell = Process.GetProcessById(pid))
            using (var current = Process.GetCurrentProcess()) {
                if (shell.SessionId != current.SessionId ||
                    !String.Equals(shell.MainModule.FileName, explorerPath, StringComparison.OrdinalIgnoreCase)) {
                    throw new IOException("无法确认占用缓存的是当前会话的 Windows 资源管理器。");
                }

                var paths = new HashSet<string>(lockedFiles, StringComparer.OrdinalIgnoreCase);
                bool holdsCache = false;
                foreach (ProcessModule module in shell.Modules) {
                    if (paths.Contains(module.FileName)) {
                        holdsCache = true;
                        break;
                    }
                }
                if (!holdsCache) throw new IOException("资源管理器未加载待清理的缓存，可以选择下次登录自动清理。");

                long started = shell.StartTime.ToFileTimeUtc();
                int result = RestartRegisteredProcess(new UniqueProcess {
                    ProcessId = (uint)pid,
                    Started = new FILETIME { dwLowDateTime = unchecked((int)started), dwHighDateTime = (int)(started >> 32) }
                });
                if (!shell.WaitForExit(2000)) {
                    throw new IOException("Windows 资源管理器未能正常退出。" +
                        (result != 0 ? " " + new Win32Exception(result).Message : ""));
                }
            }

            if (WaitForShell()) return;
            if (ShellProcessId() == 0) {
                var start = new ProcessStartInfo(explorerPath) {
                    WorkingDirectory = Path.GetDirectoryName(explorerPath), UseShellExecute = true
                };
                using (Process restarted = Process.Start(start)) { }
            }
            if (!WaitForShell()) throw new IOException("Windows 资源管理器尚未恢复。");
        }

        private static int ShellProcessId() {
            uint pid;
            return GetWindowThreadProcessId(GetShellWindow(), out pid) != 0 ? (int)pid : 0;
        }

        private static bool WaitForShell() {
            for (int attempt = 0; attempt < 20; attempt++) {
                if (ShellProcessId() != 0) return true;
                Thread.Sleep(150);
            }
            return ShellProcessId() != 0;
        }

        // Register the exact shell process, rather than every process that may touch a cache file.
        private static int RestartRegisteredProcess(UniqueProcess process) {
            uint session;
            int status = RmStartSession(out session, 0, new StringBuilder(33));
            if (status != 0) return status;
            try {
                status = RmRegisterResources(session, 0, IntPtr.Zero, 1, new[] { process }, 0, IntPtr.Zero);
                if (status != 0) return status;
                int restarted;
                try {
                    status = RmShutdown(session, 0, IntPtr.Zero);
                } finally {
                    restarted = RmRestart(session, 0, IntPtr.Zero);
                }
                return status != 0 ? status : restarted;
            } finally {
                RmEndSession(session);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct UniqueProcess {
            internal uint ProcessId;
            internal FILETIME Started;
        }

        [DllImport("user32.dll")]
        private static extern IntPtr GetShellWindow();

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int RmStartSession(out uint session, int flags, StringBuilder sessionKey);

        [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
        private static extern int RmRegisterResources(uint session, uint files, IntPtr fileNames, uint applications,
            [In] UniqueProcess[] processes, uint services, IntPtr serviceNames);

        [DllImport("rstrtmgr.dll")]
        private static extern int RmShutdown(uint session, uint flags, IntPtr progress);

        [DllImport("rstrtmgr.dll")]
        private static extern int RmRestart(uint session, uint flags, IntPtr progress);

        [DllImport("rstrtmgr.dll")]
        private static extern int RmEndSession(uint session);
    }
}
