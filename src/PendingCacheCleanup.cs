using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Win32;

namespace TDock {
    internal sealed class PendingCacheCleanup {
        internal const string LogonArgument = "--finish-cache-cleanup";
        internal const string RunOncePath = @"Software\Microsoft\Windows\CurrentVersion\RunOnce";

        private const string FolderPrefix = "TDock-Cleanup-";
        private const string PlanName = "CacheFiles.txt";
        private readonly string appData;
        private readonly string folder;
        private readonly string plan;

        private PendingCacheCleanup(string appData, string folder) {
            this.appData = appData;
            this.folder = folder;
            plan = Path.Combine(folder, PlanName);
        }

        internal string Executable { get { return Path.Combine(folder, PortableCleanup.UninstallerName); } }
        private string EntryName { get { return Path.GetFileName(folder); } }
        private string Command { get { return "\"" + Executable + "\" " + LogonArgument; } }

        internal static PendingCacheCleanup Create(string uninstaller, string appData, string[] blocked) {
            var entries = new List<string>();
            foreach (string file in blocked) {
                try {
                    if (!PortableCleanup.IsLink(file)) {
                        entries.Add(Path.GetFileName(file) + "\t" + PortableCleanup.CacheStamp(file));
                    }
                } catch (FileNotFoundException) {
                } catch (DirectoryNotFoundException) {
                }
            }
            if (entries.Count == 0) return null;

            string folder = Path.Combine(appData, FolderPrefix + Guid.NewGuid().ToString("N"));
            var job = new PendingCacheCleanup(appData, folder);
            Directory.CreateDirectory(folder);
            try {
                File.Copy(uninstaller, job.Executable);
                File.WriteAllLines(job.plan, entries);
                return job;
            } catch {
                job.DiscardCopy();
                throw;
            }
        }

        internal static PendingCacheCleanup Open(string executable, string appData) {
            string folder = Path.GetDirectoryName(executable);
            string name = Path.GetFileName(folder);
            Guid id;
            if (!String.Equals(Path.GetDirectoryName(folder), appData, StringComparison.OrdinalIgnoreCase) ||
                !name.StartsWith(FolderPrefix, StringComparison.OrdinalIgnoreCase) ||
                !Guid.TryParseExact(name.Substring(FolderPrefix.Length), "N", out id)) return null;

            var job = new PendingCacheCleanup(appData, folder);
            if (!File.Exists(job.plan)) return null;
            if (PortableCleanup.IsLink(folder) || PortableCleanup.IsLink(job.plan)) {
                throw new IOException("清理任务目录指向了其他位置。");
            }
            return job;
        }

        internal string[] RemoveFiles() {
            if (PortableCleanup.IsLink(folder) || PortableCleanup.IsLink(plan)) {
                throw new IOException("清理任务目录指向了其他位置。");
            }
            var recorded = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string line in File.ReadLines(plan)) {
                int split = line.IndexOf('\t');
                if (split <= 0 || split == line.Length - 1) throw new IOException("清理任务记录已损坏。");
                recorded.Add(line.Substring(0, split), line.Substring(split + 1));
            }
            return PortableCleanup.RemoveRuntime(appData, recorded);
        }

        internal void Schedule(RegistryKey root, string keyPath) {
            string command = Command;
            if (command.Length > 260) throw new IOException("清理任务路径过长，无法安排下次登录清理。");
            using (RegistryKey key = root.CreateSubKey(keyPath)) {
                if (key == null) throw new IOException("无法打开一次性登录任务设置。");
                key.SetValue(EntryName, command, RegistryValueKind.String);
            }
        }

        internal void CancelSchedule(RegistryKey root, string keyPath) {
            using (RegistryKey key = root.OpenSubKey(keyPath, true)) {
                if (key != null && String.Equals(key.GetValue(EntryName) as string, Command, StringComparison.Ordinal)) {
                    key.DeleteValue(EntryName, false);
                }
            }
        }

        internal void RemovePlan() {
            if (PortableCleanup.IsLink(folder) || PortableCleanup.IsLink(plan)) {
                throw new IOException("清理任务目录指向了其他位置。");
            }
            File.Delete(plan);
        }

        internal void DiscardCopy() {
            if (PortableCleanup.IsLink(folder)) return;
            File.Delete(plan);
            File.Delete(Executable);
            if (Directory.GetFileSystemEntries(folder).Length == 0) Directory.Delete(folder);
        }
    }
}
