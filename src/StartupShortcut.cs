using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace TDock {
    internal static class StartupShortcut {
        internal const string FileName = "TDock.lnk";
        private const string StartupArgument = "--startup";

        private static string ShortcutPath {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Startup), FileName); }
        }

        internal static string ReadTarget(string path) {
            object shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell", true));
            try {
                dynamic shortcut = ((dynamic)shell).CreateShortcut(path);
                try {
                    return (string)shortcut.TargetPath;
                } finally {
                    Marshal.FinalReleaseComObject(shortcut);
                }
            } finally {
                Marshal.FinalReleaseComObject(shell);
            }
        }

        internal static bool Enabled {
            get { return File.Exists(ShortcutPath); }
            set {
                if (!value) {
                    File.Delete(ShortcutPath);
                    return;
                }

                string executable = Application.ExecutablePath;
                // Prefer the installed copy so enabling the option from dist also survives an MSI upgrade.
                string installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                                "TDock", "App", "TDock.exe");
                if (File.Exists(installed)) executable = installed;

                string path = ShortcutPath;
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                object shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell", true));
                try {
                    dynamic shortcut = ((dynamic)shell).CreateShortcut(path);
                    try {
                        shortcut.TargetPath = executable;
                        shortcut.Arguments = StartupArgument;
                        shortcut.WorkingDirectory = Path.GetDirectoryName(executable);
                        shortcut.Description = "TDock · Windows 任务栏动画";
                        shortcut.Save();
                    } finally {
                        Marshal.FinalReleaseComObject(shortcut);
                    }
                } finally {
                    Marshal.FinalReleaseComObject(shell);
                }
            }
        }
    }
}
