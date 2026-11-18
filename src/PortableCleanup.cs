using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace TDock {
    internal sealed class PortableCleanup {
        internal const string UninstallerName = "TDock-Uninstall.exe";
        internal const string SettingsKey = @"Software\TDock";

        private const string AppName = "TDock.exe";
        private static readonly string[] PackageFiles = { "README.md", "MinHook-LICENSE.txt", "TDockNative.dll", AppName };
        private static readonly string[] SettingNames = { "EffectsEnabled", "CenterTaskbar", "ClickDurationMs" };
        private static readonly Regex CacheFile = new Regex(
            @"\A(?:TDockNative-[0-9A-F]{64}\.dll|[0-9A-F]{32}\.tmp)\z",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        private readonly string packageDirectory;
        private readonly string shortcut;
        private readonly string defaultFolder;
        private readonly Dictionary<string, string> digests = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        internal PortableCleanup(string packageDirectory, string startupDirectory) {
            this.packageDirectory = new DirectoryInfo(packageDirectory).FullName;
            shortcut = String.IsNullOrEmpty(startupDirectory) ? null : Path.Combine(startupDirectory, StartupShortcut.FileName);

            using (Stream resource = typeof(PortableCleanup).Assembly.GetManifestResourceStream("TDock.PortableFiles"))
            using (var reader = new StreamReader(resource)) {
                defaultFolder = reader.ReadLine();
                foreach (string file in PackageFiles) digests.Add(file, reader.ReadLine());
            }
        }

        internal string AppPath { get { return Path.Combine(packageDirectory, AppName); } }

        internal bool IsDefaultFolder {
            get { return SamePath(new DirectoryInfo(packageDirectory).Name, defaultFolder); }
        }

        internal void VerifyPackage() {
            if (IsLink(packageDirectory) ||
                !PackageFiles.Where(name => name != "MinHook-LICENSE.txt").Any(MatchesPackageFile)) {
                throw new IOException("请先完整解压 TDock 的 ZIP，然后在解压目录中打开 TDock-Uninstall.exe。");
            }
        }

        internal bool StopSelectedCopy() {
            bool otherCopyRunning = false;
            using (Process current = Process.GetCurrentProcess()) {
                foreach (Process dock in Process.GetProcessesByName("TDock")) {
                    using (dock) {
                        try {
                            if (dock.SessionId != current.SessionId || dock.HasExited) continue;
                            if (!SamePath(dock.MainModule.FileName, AppPath)) {
                                otherCopyRunning = true;
                                continue;
                            }
                            Stop(dock);
                        } catch (Win32Exception) {
                            if (!dock.HasExited) {
                                throw new IOException("无法检查正在运行的 TDock。请先从托盘退出 TDock，然后重试。");
                            }
                        } catch (InvalidOperationException) {
                            if (!dock.HasExited) throw;
                        }
                    }
                }
            }
            return otherCopyRunning;
        }

        private static void Stop(Process dock) {
            for (int attempt = 0; attempt < 25; attempt++) {
                if (dock.HasExited) return;
                ControllerWindow.PostExit(dock.Id);
                if (dock.WaitForExit(200)) return;
            }
            throw new IOException("TDock 尚未退出。请在托盘中退出 TDock，然后重试。");
        }

        internal void RemoveStartupShortcut() {
            if (File.Exists(shortcut) && !IsLink(shortcut) &&
                SamePath(StartupShortcut.ReadTarget(shortcut), AppPath)) {
                File.Delete(shortcut);
            }
        }

        internal static string[] RemoveRuntime(string appData, IDictionary<string, string> recorded = null) {
            string dataDirectory = Path.Combine(appData, "TDock");
            string runtime = Path.Combine(dataDirectory, "Runtime");
            if (!Directory.Exists(runtime)) return new string[0];
            if (IsLink(dataDirectory) || IsLink(runtime)) {
                throw new IOException("运行缓存目录指向其他位置，请确认目录后手动清理：" + Environment.NewLine + runtime);
            }

            var retained = new List<string>();
            foreach (string file in Directory.EnumerateFiles(runtime)) {
                string name = Path.GetFileName(file);
                string expected = null;
                if (!CacheFile.IsMatch(name) || (recorded != null && !recorded.TryGetValue(name, out expected))) continue;
                try {
                    if (IsLink(file) || (recorded != null && CacheStamp(file) != expected)) continue;
                    File.Delete(file);
                } catch (FileNotFoundException) {
                } catch (DirectoryNotFoundException) {
                } catch (IOException) {
                    retained.Add(file);
                } catch (UnauthorizedAccessException) {
                    retained.Add(file);
                }
            }
            RemoveEmptyDirectory(runtime);
            RemoveEmptyDirectory(dataDirectory);
            return retained.ToArray();
        }

        internal static string CacheStamp(string file) {
            var info = new FileInfo(file);
            return String.Format(CultureInfo.InvariantCulture, "{0}/{1}/{2}",
                info.Length, info.CreationTimeUtc.Ticks, info.LastWriteTimeUtc.Ticks);
        }

        internal static void RemoveSettings(RegistryKey root, string path) {
            bool empty;
            using (RegistryKey settings = root.OpenSubKey(path, true)) {
                if (settings == null) return;
                foreach (string name in SettingNames) settings.DeleteValue(name, false);
                empty = settings.ValueCount == 0 && settings.SubKeyCount == 0;
            }
            if (empty) root.DeleteSubKey(path, false);
        }

        internal void RemovePackageFiles() {
            if (IsLink(packageDirectory)) throw new IOException("解压目录指向其他位置，请手动清理。");
            foreach (string name in PackageFiles) {
                if (MatchesPackageFile(name)) File.Delete(Path.Combine(packageDirectory, name));
            }
        }

        private bool MatchesPackageFile(string name) {
            string file = Path.Combine(packageDirectory, name);
            return File.Exists(file) && !IsLink(file) && SamePath(digests[name], Digest(file));
        }

        internal static string Digest(string file) {
            using (Stream stream = File.OpenRead(file))
            using (SHA256 sha = SHA256.Create()) {
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
            }
        }

        internal static bool IsLink(string path) {
            return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
        }

        private static bool SamePath(string left, string right) {
            return String.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }

        private static void RemoveEmptyDirectory(string path) {
            if (Directory.Exists(path) && !IsLink(path) && !Directory.EnumerateFileSystemEntries(path).Any()) {
                Directory.Delete(path);
            }
        }

        // The last step runs in Windows PowerShell after this executable has closed.
        // Paths are passed as data and checked again before deletion.
        private const string FinishCommand = @"
$ErrorActionPreference = 'Stop'
$file = $env:TDOCK_UNINSTALL_FILE
$folder = [IO.Path]::GetDirectoryName($file)
$result = $env:TDOCK_UNINSTALL_RESULT
$icon = 'Information'
$exitCode = 0
try {
    $owner = Get-Process -Id ([int]$env:TDOCK_UNINSTALL_PID) -ErrorAction SilentlyContinue
    if ($owner -and $owner.StartTime.ToFileTimeUtc() -eq [long]$env:TDOCK_UNINSTALL_START) {
        if (-not $owner.WaitForExit(20000)) { throw '卸载程序尚未退出。' }
    }
    if ((Get-Item -LiteralPath $folder -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw '解压目录指向其他位置。'
    }
    if (Test-Path -LiteralPath $file) {
        if ((Get-Item -LiteralPath $file -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw '卸载器文件指向其他位置。'
        }
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $stream = [IO.File]::OpenRead($file)
            try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
        } finally { $sha.Dispose() }
        if ($hash -ne $env:TDOCK_UNINSTALL_DIGEST) {
            throw '卸载器文件已变更。'
        }
        [IO.File]::Delete($file)
    }
    if ($env:TDOCK_UNINSTALL_REMOVE_FOLDER -eq '1' -and
        -not (Get-ChildItem -LiteralPath $folder -Force | Select-Object -First 1)) {
        [IO.Directory]::Delete($folder)
    }
} catch {
    $exitCode = 1
    $icon = 'Warning'
    $result = '其他清理已完成。请手动检查解压目录是否仍有卸载器或空文件夹。' +
        [Environment]::NewLine + $folder + [Environment]::NewLine + $_.Exception.Message
}
if ($exitCode -ne 0 -or $result) {
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($result, '卸载 TDock', 'OK', $icon)
}
exit $exitCode
";

        internal static Process DeleteSelfAfterExit(string executable, bool removeFolder, string result) {
            string powershell = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
            var start = new ProcessStartInfo(powershell, "-NoLogo -NoProfile -NonInteractive -STA -Command \"" + FinishCommand + "\"") {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Environment.SystemDirectory
            };
            using (Process current = Process.GetCurrentProcess()) {
                start.EnvironmentVariables["TDOCK_UNINSTALL_PID"] = current.Id.ToString(CultureInfo.InvariantCulture);
                start.EnvironmentVariables["TDOCK_UNINSTALL_START"] = current.StartTime.ToFileTimeUtc().ToString(CultureInfo.InvariantCulture);
            }
            start.EnvironmentVariables["TDOCK_UNINSTALL_FILE"] = executable;
            start.EnvironmentVariables["TDOCK_UNINSTALL_DIGEST"] = Digest(executable);
            start.EnvironmentVariables["TDOCK_UNINSTALL_REMOVE_FOLDER"] = removeFolder ? "1" : "0";
            start.EnvironmentVariables["TDOCK_UNINSTALL_RESULT"] = result ?? "";
            return Process.Start(start);
        }
    }
}
