using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;

namespace TDock {
    internal static class RuntimeLibrary {
        internal static string Prepare(string source) {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TDock", "Runtime");
            return Prepare(source, folder);
        }

        internal static string Prepare(string source, string folder) {
            // Explorer can retain a loaded module. The uninstaller cleans up these copies.
            byte[] contents = File.ReadAllBytes(source);
            using (var sha = SHA256.Create()) {
                string digest = HashText(sha.ComputeHash(contents));
                folder = Path.GetFullPath(folder);
                Directory.CreateDirectory(folder);
                string path = Path.Combine(folder, "TDockNative-" + digest + ".dll");
                using (OpenPublishLock(folder)) {
                    if (!Matches(path, digest, sha)) {
                        ReplaceCache(path, contents);
                        if (!Matches(path, digest, sha)) {
                            throw new IOException("TDock 的运行组件完整性校验失败。");
                        }
                    }
                    return path;
                }
            }
        }

        private static FileStream OpenPublishLock(string folder) {
            string path = Path.Combine(folder, "TDockRuntimePublish.lock");
            var elapsed = Stopwatch.StartNew();
            while (true) {
                try {
                    return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None,
                        4096, FileOptions.DeleteOnClose);
                } catch (IOException error) {
                    const int SharingViolation = 32;
                    if ((error.HResult & 0xffff) != SharingViolation) throw;
                    if (elapsed.ElapsedMilliseconds >= 3000) {
                        throw new IOException("另一份 TDock 正在更新运行组件。请稍后重试。", error);
                    }
                    Thread.Sleep(25);
                }
            }
        }

        private static void ReplaceCache(string path, byte[] contents) {
            const uint ReplaceExisting = 0x1;
            string temporary = Path.Combine(Path.GetDirectoryName(path), Guid.NewGuid().ToString("N") + ".tmp");
            try {
                File.WriteAllBytes(temporary, contents);
                if (!MoveFileEx(temporary, path, ReplaceExisting)) {
                    throw new IOException("无法替换 TDock 的运行缓存。", new Win32Exception(Marshal.GetLastWin32Error()));
                }
            } finally {
                File.Delete(temporary);
            }
        }

        private static bool Matches(string path, string digest, SHA256 sha) {
            try {
                using (var cached = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete)) {
                    return HashText(sha.ComputeHash(cached)) == digest;
                }
            } catch (FileNotFoundException) {
                return false;
            }
        }

        private static string HashText(byte[] digest) {
            return BitConverter.ToString(digest).Replace("-", "");
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool MoveFileEx(string source, string destination, uint flags);
    }
}
