using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("TDock")]
[assembly: System.Reflection.AssemblyProduct("TDock")]
[assembly: System.Reflection.AssemblyDescription("Windows 10 原生任务栏动画与居中")]
[assembly: System.Reflection.AssemblyVersion("0.1.21.0")]
[assembly: System.Reflection.AssemblyFileVersion("0.1.21.0")]

namespace TDock {
    internal sealed class TrayApplication : ApplicationContext {
        private const int RefreshInterval = 1000;
        private const int FirstCenterCheck = 200;
        private const int CenterStatusDelay = 350;
        private const string WaitingForLayout = "正在等待任务栏排版…";

        private readonly NativeEffects effects = new NativeEffects();
        private readonly bool supported = NativeEffects.IsSupportedWindows();
        private TrayPanel panel;
        private readonly NotifyIcon tray;
        private readonly System.Windows.Forms.Timer refresh = new System.Windows.Forms.Timer { Interval = RefreshInterval };
        private readonly System.Windows.Forms.Timer centerWait = new System.Windows.Forms.Timer { Interval = CenterStatusDelay };

        private bool requested;
        private bool centerRequested;
        private bool refreshing;
        private bool refreshQueued;
        private bool closing;
        private int duration;
        private int centerChange;
        private bool centerChangePending;
        private string pendingCenterMessage;
        private string centerSaveIssue = "";
        private string panelStatus = "";
        private string centerStatusText = "";
        private string centerStatusDetails = "";

        internal TrayApplication(Icon icon) {
            requested = supported && Preferences.EffectsEnabled;
            centerRequested = supported && Preferences.CenterTaskbar;
            duration = Preferences.Duration;

            tray = new NotifyIcon { Icon = icon };
            tray.MouseClick += (sender, args) => {
                if (!closing && (args.Button == MouseButtons.Left || args.Button == MouseButtons.Right)) {
                    EnsurePanel().ShowAt(Cursor.Position);
                }
            };
            refresh.Tick += (sender, args) => {
                if (refreshing) return;
                refresh.Interval = RefreshInterval;
                RefreshEffects();
            };
            centerWait.Tick += (sender, args) => {
                centerWait.Stop();
                if (!closing && pendingCenterMessage != null) {
                    DisplayCenterStatus("等待排版", pendingCenterMessage);
                }
            };
            ShowResult(supported
                ? new EffectResult(requested || centerRequested ? EffectState.Waiting : EffectState.Off, "正在连接任务栏…",
                    centerRequested ? CenterState.Waiting : CenterState.Off)
                : new EffectResult(EffectState.Unsupported, "此版本支持 Windows 10 22H2 x64。"), centerRequested);
        }

        internal void Start() {
            tray.Visible = true;
            Application.Idle += StartOnIdle;
        }

        private void StartOnIdle(object sender, EventArgs args) {
            Application.Idle -= StartOnIdle;
            if (closing) return;
            if (requested || centerRequested) RefreshEffects();
            EnsurePanel();
        }

        private TrayPanel EnsurePanel() {
            if (panel != null) return panel;

            panel = new TrayPanel(duration, supported);
            panel.AnimationSwitch.Click += (sender, args) => {
                requested = panel.AnimationSwitch.Checked;
                SaveSetting(() => Preferences.EffectsEnabled = requested, "动画开关仅在本次运行生效。");
                RefreshEffects();
            };
            panel.CenterSwitch.Click += (sender, args) => {
                centerRequested = panel.CenterSwitch.Checked;
                ++centerChange;
                centerChangePending = true;
                centerSaveIssue = TrySaveSetting(() => Preferences.CenterTaskbar = centerRequested, "居中开关仅在本次运行生效。");
                if (centerRequested) {
                    WaitForCenter(WaitingForLayout);
                    refresh.Interval = FirstCenterCheck;
                } else {
                    FinishCenterStatus("", "");
                }
                RefreshEffects();
            };
            panel.StartupSwitch.Click += (sender, args) => {
                SaveSetting(() => StartupShortcut.Enabled = panel.StartupSwitch.Checked, "无法保存登录自启设置。");
                panel.StartupSwitch.Checked = StartupShortcut.Enabled;
            };
            panel.Duration.Committed += (sender, args) => SetDuration(panel.Duration.Milliseconds);
            panel.ExitButton.Click += (sender, args) => RequestExit();
            UpdateChecks();
            panel.SetStatus(panelStatus);
            panel.SetCenterStatus(centerStatusText, centerStatusDetails);
            return panel;
        }

        private void SetDuration(int value) {
            if (duration == value) return;
            duration = value;
            SaveSetting(() => Preferences.Duration = value, "时长仅在本次运行生效。");
            if (requested) RefreshEffects();
        }

        private void UpdateChecks() {
            if (panel == null) return;
            panel.AnimationSwitch.Checked = requested;
            panel.CenterSwitch.Checked = centerRequested;
        }

        private void ShowResult(EffectResult result, bool attemptedCenter) {
            string issue = "";
            string centerStatus = "";
            bool waitingForCenter = false;
            if (result.State == EffectState.Unsupported || result.State == EffectState.Conflict ||
                result.State == EffectState.Error) {
                issue = result.Message;
                if ((attemptedCenter || centerChangePending) && !result.AnimationFailed) {
                    centerStatus = result.State == EffectState.Unsupported ? "布局不支持" :
                        result.State == EffectState.Conflict ? "存在冲突" :
                        attemptedCenter ? "启用失败" : "关闭失败";
                }
            } else if (result.Center == CenterState.NoSpace) {
                issue = "按钮空间不足，当前使用系统排版。";
                centerStatus = "空间不足";
            } else if (result.Center == CenterState.Unavailable) {
                issue = "当前布局不可居中，请使用主屏底部经典任务栏。";
                centerStatus = "布局不支持";
            } else if (result.State == EffectState.Waiting) {
                issue = result.Message;
                waitingForCenter = centerRequested ? result.Center != CenterState.Centered : centerChangePending;
            } else if (centerRequested && result.Center != CenterState.Centered) {
                issue = WaitingForLayout;
                waitingForCenter = true;
            }

            if (waitingForCenter) {
                WaitForCenter(issue);
            } else {
                centerChangePending = false;
                FinishCenterStatus(centerStatus, centerStatus.Length == 0 ? "" : issue);
            }
            panelStatus = waitingForCenter || centerStatus.Length != 0 ? "" : issue;
            if (panel != null) panel.SetStatus(panelStatus);

            if (issue.Length != 0) {
                tray.Text = "TDock · 打开托盘菜单查看状态";
            } else if (result.Center == CenterState.Centered) {
                tray.Text = result.State == EffectState.On ? "TDock · 动画与居中已启用" : "TDock · 任务栏已居中";
            } else {
                tray.Text = result.State == EffectState.On ? "TDock · 动画已启用" : "TDock · 动画已停用";
            }
        }

        private void WaitForCenter(string details) {
            bool starting = pendingCenterMessage == null;
            pendingCenterMessage = string.IsNullOrEmpty(details) ? WaitingForLayout : details;
            if (starting) {
                DisplayCenterStatus("", "");
                centerWait.Start();
            } else if (!centerWait.Enabled) {
                DisplayCenterStatus("等待排版", pendingCenterMessage);
            }
        }

        private void FinishCenterStatus(string text, string details) {
            centerWait.Stop();
            pendingCenterMessage = null;
            refresh.Interval = RefreshInterval;
            DisplayCenterStatus(text, details);
        }

        private void DisplayCenterStatus(string text, string details) {
            if (centerSaveIssue.Length != 0) {
                if (text.Length == 0) text = "未保存";
                details = details.Length == 0 ? centerSaveIssue : details + " " + centerSaveIssue;
            }
            centerStatusText = text;
            centerStatusDetails = details;
            if (panel != null) panel.SetCenterStatus(text, details);
        }

        private void SaveSetting(Action save, string failure) {
            panel.SetSaveStatus(TrySaveSetting(save, failure));
        }

        private static string TrySaveSetting(Action save, string failure) {
            try {
                save();
                return "";
            } catch (Exception error) {
                if (!(error is IOException || error is UnauthorizedAccessException ||
                    error is System.Security.SecurityException || error is COMException)) throw;
                return failure;
            }
        }

        private async void RefreshEffects() {
            if (closing || !supported) return;
            refreshQueued = true;
            if (refreshing) return;
            refreshing = true;
            try {
                while (refreshQueued && !closing) {
                    refreshQueued = false;
                    bool enable = requested;
                    bool center = centerRequested;
                    int centerRequest = centerChange;
                    int durationToApply = duration;
                    EffectResult result;
                    try {
                        result = await Task.Run(() => effects.Sync(enable, center, durationToApply));
                    } catch (Exception error) {
                        System.Diagnostics.Trace.WriteLine(error);
                        result = new EffectResult(EffectState.Error, "TDock 无法更新任务栏状态。");
                    }
                    if (closing) return;
                    if (enable != requested || center != centerRequested || centerRequest != centerChange ||
                        (enable && durationToApply != duration)) {
                        refreshQueued = true;
                        continue;
                    }
                    if (result.State == EffectState.Unsupported || result.State == EffectState.Conflict ||
                        result.State == EffectState.Error) {
                        requested = false;
                        if (!result.AnimationFailed) centerRequested = false;
                        UpdateChecks();
                        if (result.AnimationFailed) refreshQueued = true;
                    }
                    refresh.Enabled = requested || centerRequested || result.State == EffectState.Waiting;
                    ShowResult(result, center);
                }
            } catch (Exception error) {
                if (!closing) {
                    System.Diagnostics.Trace.WriteLine(error);
                    bool attemptedCenter = centerRequested;
                    requested = false;
                    centerRequested = false;
                    UpdateChecks();
                    refresh.Stop();
                    ShowResult(new EffectResult(EffectState.Error, "TDock 无法更新任务栏状态。"), attemptedCenter);
                }
            } finally {
                refreshing = false;
            }
        }

        internal async void RequestExit() {
            if (closing) return;
            closing = true;
            Application.Idle -= StartOnIdle;
            refresh.Stop();
            centerWait.Stop();
            if (panel != null) panel.Hide();
            while (refreshing) await Task.Delay(30);
            try {
                await Task.Run(() => effects.Sync(false, false, duration));
            } catch (Exception error) {
                System.Diagnostics.Trace.WriteLine(error);
                // Explorer also restores drawing when the controller mutex is released.
            } finally {
                ExitThread();
            }
        }

        protected override void Dispose(bool disposing) {
            if (disposing) {
                Application.Idle -= StartOnIdle;
                tray.Dispose();
                if (panel != null) panel.Dispose();
                refresh.Dispose();
                centerWait.Dispose();
            }
            base.Dispose(disposing);
        }

        private static Icon LoadTrayIcon() {
            const int applicationIcon = 32512; // Resource group emitted by csc /win32icon.
            const uint iconType = 1;
            Size size = SystemInformation.SmallIconSize;
            IntPtr handle = LoadImage(GetModuleHandle(null), new IntPtr(applicationIcon), iconType, size.Width, size.Height, 0);
            if (handle == IntPtr.Zero) return (Icon)SystemIcons.Application.Clone();
            try {
                using (var icon = Icon.FromHandle(handle)) return (Icon)icon.Clone();
            } finally {
                DestroyIcon(handle);
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandle(string name);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr LoadImage(IntPtr module, IntPtr name, uint type, int width, int height, uint flags);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DestroyIcon(IntPtr icon);

        [STAThread]
        private static int Main(string[] args) {
            bool shutdown = args.Length == 1 && args[0] == Preferences.ShutdownArgument;
            using (var controller = new Mutex(false, ControllerWindow.MutexName)) {
                if (shutdown) return ControllerWindow.ShutdownRunningInstance(controller) ? 0 : 1;
                if (!ControllerWindow.TryAcquire(controller, 0)) return 0;
                try {
                    Application.EnableVisualStyles();
                    Application.SetCompatibleTextRenderingDefault(false);
                    using (var icon = LoadTrayIcon())
                    using (var context = new TrayApplication(icon))
                    using (var window = new ControllerWindow(context.RequestExit)) {
                        context.Start();
                        Application.Run(context);
                    }
                    return 0;
                } finally {
                    controller.ReleaseMutex();
                }
            }
        }
    }
}
