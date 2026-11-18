using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("TDock 免安装版卸载器")]
[assembly: System.Reflection.AssemblyProduct("TDock")]

namespace TDock {
    internal static class PortableUninstall {
        private const string Title = "卸载 TDock";

        [STAThread]
        private static int Main(string[] args) {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            try {
                string executable = Application.ExecutablePath;
                if (!String.Equals(Path.GetFileName(executable), PortableCleanup.UninstallerName, StringComparison.OrdinalIgnoreCase) ||
                    (File.GetAttributes(executable) & FileAttributes.ReparsePoint) != 0) {
                    throw new IOException("请在解压目录中运行 TDock-Uninstall.exe。");
                }
                string folder = Path.GetDirectoryName(executable);
                string appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (String.IsNullOrEmpty(appData)) throw new IOException("无法找到当前用户的本地数据目录。");
                var pending = PendingCacheCleanup.Open(executable, appData);
                bool fromLogon = args.Length == 1 && args[0] == PendingCacheCleanup.LogonArgument;
                if (pending != null && (fromLogon || args.Length == 0)) {
                    return WithUninstallLock(() => ContinueCacheCleanup(pending, fromLogon));
                }
                if (args.Length != 0) throw new IOException("找不到对应的缓存清理任务。");

                var cleanup = new PortableCleanup(folder, Environment.GetFolderPath(Environment.SpecialFolder.Startup));
                cleanup.VerifyPackage();

                using (var dialog = new UninstallDialog(folder)) {
                    if (dialog.ShowDialog() != DialogResult.OK) return 0;
                    return WithUninstallLock(() => Uninstall(executable, cleanup, appData, dialog.ClearSettings));
                }
            } catch (Exception error) {
                MessageBox.Show("卸载未完成。" + Environment.NewLine + Environment.NewLine + error.Message, Title,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return 1;
            }
        }

        private static int WithUninstallLock(Func<int> work) {
            using (var mutex = new Mutex(false, @"Local\TDock.PortableUninstall")) {
                if (!ControllerWindow.TryAcquire(mutex, 0)) throw new IOException("另一个 TDock 卸载窗口正在清理。");
                try {
                    return work();
                } finally {
                    mutex.ReleaseMutex();
                }
            }
        }

        private static int Uninstall(string executable, PortableCleanup cleanup, string appData, bool clearSettings) {
            cleanup.VerifyPackage();
            bool anotherCopy = cleanup.StopSelectedCopy();
            PendingCacheCleanup pending = null;

            using (var controller = new Mutex(false, ControllerWindow.MutexName)) {
                bool acquired = !anotherCopy && ControllerWindow.TryAcquire(controller, 0);
                anotherCopy = !acquired;
                try {
                    cleanup.RemoveStartupShortcut();
                    if (acquired) {
                        if (clearSettings) PortableCleanup.RemoveSettings(Registry.CurrentUser, PortableCleanup.SettingsKey);
                        string[] blocked = PortableCleanup.RemoveRuntime(appData);
                        if (blocked.Length != 0) pending = PendingCacheCleanup.Create(executable, appData, blocked);
                    }
                    try {
                        cleanup.RemovePackageFiles();
                    } catch {
                        if (pending != null) pending.DiscardCopy();
                        throw;
                    }
                } finally {
                    if (acquired) controller.ReleaseMutex();
                }
            }

            bool programRemoved = !File.Exists(cleanup.AppPath);
            string result = programRemoved ? "程序已移除。" : "压缩包中的匹配文件已清理，改动过的程序文件仍保留。";
            result += anotherCopy ? "另一份 TDock 正在运行，已保留共用的设置与缓存。"
                : clearSettings ? "保存的设置已清除。" : "保存的设置已保留。";
            if (pending != null) {
                using (var dialog = new CacheCleanupDialog(pending, programRemoved)) {
                    DialogResult choice = dialog.ShowDialog();
                    if (choice == DialogResult.OK) {
                        pending.DiscardCopy();
                        result += Environment.NewLine + "运行缓存已清理。";
                    } else if (choice == DialogResult.Yes) {
                        result += Environment.NewLine + "已安排下次登录 Windows 时自动清理剩余缓存。";
                    } else {
                        result += Environment.NewLine + "运行缓存待清理。资源管理器退出后，可以双击此清理工具继续：" +
                            Environment.NewLine + pending.Executable;
                    }
                }
            }

            DeleteUninstaller(executable, cleanup.IsDefaultFolder, result);
            return 0;
        }

        private static void DeleteUninstaller(string executable, bool removeFolder, string result) {
            using (var worker = PortableCleanup.DeleteSelfAfterExit(executable, removeFolder, result)) {
                if (worker == null) throw new IOException("请关闭此窗口后手动删除 TDock-Uninstall.exe。");
            }
        }

        private static int ContinueCacheCleanup(PendingCacheCleanup pending, bool fromLogon) {
            string[] blocked = RetryCache(pending, false);
            if (blocked == null && !fromLogon) {
                throw new IOException("TDock 正在使用运行缓存。退出 TDock 后可再次打开清理工具。");
            }
            if (blocked != null && blocked.Length != 0) {
                if (fromLogon) throw new IOException("运行缓存仍被占用。可再次打开此清理工具：" +
                    Environment.NewLine + pending.Executable);
                using (var dialog = new CacheCleanupDialog(pending, false)) {
                    if (dialog.ShowDialog() != DialogResult.OK) return 0;
                }
            }

            if (!fromLogon) pending.CancelSchedule(Registry.CurrentUser, PendingCacheCleanup.RunOncePath);
            pending.RemovePlan();
            DeleteUninstaller(pending.Executable, true, fromLogon ? null : "运行缓存已清理。");
            return 0;
        }

        private static string[] RetryCache(PendingCacheCleanup pending, bool restartExplorer) {
            using (var controller = new Mutex(false, ControllerWindow.MutexName)) {
                if (!ControllerWindow.TryAcquire(controller, 0)) return null;
                try {
                    string[] blocked = pending.RemoveFiles();
                    if (restartExplorer && blocked.Length != 0) {
                        ExplorerRestart.Restart(blocked);
                        blocked = pending.RemoveFiles();
                    }
                    return blocked;
                } finally {
                    controller.ReleaseMutex();
                }
            }
        }

        private sealed class CacheCleanupDialog : Form {
            private readonly PendingCacheCleanup pending;
            private readonly Label status;
            private readonly Button restart;
            private readonly Button later;
            private bool working;

            internal CacheCleanupDialog(PendingCacheCleanup pending, bool programRemoved) {
                this.pending = pending;
                Text = Title;
                StartPosition = FormStartPosition.CenterScreen;
                FormBorderStyle = FormBorderStyle.FixedDialog;
                MaximizeBox = false;
                MinimizeBox = false;
                Font = SystemFonts.MessageBoxFont;
                AutoScaleMode = AutoScaleMode.Dpi;
                AutoSize = true;
                AutoSizeMode = AutoSizeMode.GrowAndShrink;
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);

                var layout = new TableLayoutPanel {
                    Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, Padding = new Padding(20)
                };
                layout.Controls.Add(new Label {
                    Text = programRemoved ? "程序已移除，缓存待清理" : "缓存待清理",
                    AutoSize = true, Margin = new Padding(0, 0, 0, 10)
                });
                layout.Controls.Add(new Label {
                    Text = "运行缓存仍被占用。可以现在重启资源管理器，或下次登录 Windows 时自动清理。",
                    AutoSize = true, MaximumSize = new Size(430, 0), Margin = new Padding(0, 0, 0, 12)
                });
                status = new Label {
                    AutoSize = true, MaximumSize = new Size(430, 0), Margin = new Padding(0, 0, 0, 12), Visible = false
                };
                layout.Controls.Add(status);

                var buttons = new FlowLayoutPanel {
                    AutoSize = true, FlowDirection = FlowDirection.RightToLeft, WrapContents = false,
                    Dock = DockStyle.Fill, Margin = Padding.Empty
                };
                restart = new Button { Text = "重启资源管理器并完成", AutoSize = true };
                later = new Button { Text = "下次登录自动清理", AutoSize = true };
                restart.Click += RestartClick;
                later.Click += (sender, args) => {
                    try {
                        pending.Schedule(Registry.CurrentUser, PendingCacheCleanup.RunOncePath);
                        DialogResult = DialogResult.Yes;
                    } catch (Exception error) {
                        SetStatus("无法安排下次登录清理。" + Environment.NewLine + error.Message);
                    }
                };
                buttons.Controls.Add(restart);
                buttons.Controls.Add(later);
                layout.Controls.Add(buttons);
                Controls.Add(layout);
                AcceptButton = restart;
                ActiveControl = restart;
                FormClosing += (sender, args) => { if (working) args.Cancel = true; };
            }

            private void SetStatus(string text) {
                status.Text = text;
                status.Visible = text.Length != 0;
            }

            private async void RestartClick(object sender, EventArgs args) {
                if (MessageBox.Show(this, "资源管理器和任务栏会短暂关闭。请先完成正在进行的文件操作。现在重启资源管理器？",
                    Title, MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.Yes) return;

                working = true;
                restart.Enabled = later.Enabled = false;
                SetStatus("正在重启资源管理器并清理缓存…");
                try {
                    string[] blocked = await Task.Run(() => RetryCache(pending, true));
                    if (blocked == null) {
                        SetStatus("另一份 TDock 正在使用缓存。可以退出它后重试，或选择下次登录自动清理。");
                        return;
                    }
                    if (blocked.Length != 0) {
                        SetStatus("仍有缓存被占用。可以选择下次登录自动清理。" + Environment.NewLine + blocked[0]);
                        return;
                    }
                } catch (Exception error) {
                    SetStatus(error.Message);
                    return;
                } finally {
                    working = false;
                    restart.Enabled = later.Enabled = true;
                }
                DialogResult = DialogResult.OK;
            }
        }

        private sealed class UninstallDialog : Form {
            private readonly CheckBox clearSettings;

            internal bool ClearSettings { get { return clearSettings.Checked; } }

            internal UninstallDialog(string folder) {
                Text = Title;
                StartPosition = FormStartPosition.CenterScreen;
                FormBorderStyle = FormBorderStyle.FixedDialog;
                MaximizeBox = false;
                MinimizeBox = false;
                Font = SystemFonts.MessageBoxFont;
                AutoScaleMode = AutoScaleMode.Dpi;
                AutoSize = true;
                AutoSizeMode = AutoSizeMode.GrowAndShrink;
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);

                var layout = new TableLayoutPanel {
                    Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, Padding = new Padding(20)
                };
                layout.Controls.Add(new Label {
                    Text = "关闭 TDock，删除这份免安装版的程序文件、登录自启快捷方式和运行缓存。",
                    AutoSize = true, MaximumSize = new Size(420, 0), Margin = new Padding(0, 0, 0, 12)
                });
                layout.Controls.Add(new TextBox {
                    Text = folder, ReadOnly = true, Width = 420, Margin = new Padding(0, 0, 0, 10)
                });
                clearSettings = new CheckBox {
                    Text = "同时清除保存的设置", Checked = true, AutoSize = true, Margin = new Padding(0, 0, 0, 16)
                };
                layout.Controls.Add(clearSettings);

                var buttons = new FlowLayoutPanel {
                    AutoSize = true, FlowDirection = FlowDirection.RightToLeft, WrapContents = false,
                    Dock = DockStyle.Fill, Margin = new Padding(0)
                };
                var uninstall = new Button { Text = "卸载", DialogResult = DialogResult.OK, AutoSize = true };
                var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, AutoSize = true };
                buttons.Controls.Add(uninstall);
                buttons.Controls.Add(cancel);
                layout.Controls.Add(buttons);
                Controls.Add(layout);
                AcceptButton = uninstall;
                CancelButton = cancel;
                ActiveControl = cancel;
            }
        }
    }
}
