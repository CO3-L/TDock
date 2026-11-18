using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace TDock {
    internal enum EffectState {
        Off, On, Waiting, Unsupported, Conflict, Error
    }

    internal enum CenterState {
        Off, Waiting, Centered, NoSpace, Unavailable
    }

    internal sealed class EffectResult {
        internal readonly EffectState State;
        internal readonly string Message;
        internal readonly CenterState Center;
        internal readonly bool AnimationFailed;

        internal EffectResult(EffectState state, string message, CenterState center = CenterState.Off, bool animationFailed = false) {
            State = state;
            Message = message;
            Center = center;
            AnimationFailed = animationFailed;
        }
    }

    internal sealed class NativeEffects {
        internal const int MinimumDuration = 300;
        internal const int MaximumDuration = 1200;
        internal const int DurationStep = 10;
        internal const int DefaultDuration = 720;

        private const uint EnableCommand = 1;
        private const uint QueryCommand = 2;
        private const uint DisableCommand = 3;
        private const uint ConfigureCommand = 4;
        private const uint QueryCenterCommand = 5;
        private const int AnimateFlag = 0x10000;
        private const int CenterFlag = 0x20000;
        private const ulong NativeActive = 0x54440001;
        private const ulong BackendVersion = 0x5444010F;
        private const ulong NativeStopped = 0x54440002;
        private const ulong NativeUnsupported = 0x54440003;
        private const ulong NativeStartFailed = 0x54440004;
        private const ulong NativeBusy = 0x54440005;
        private const ulong NativeStopFailed = 0x54440006;
        private const uint SendFlags = 0x0001 | 0x0002 | 0x0020;
        private const int CallWndProc = 4;
        private const uint LoadLibrarySearchDllLoadDir = 0x00000100;
        private const uint LoadLibrarySearchSystem32 = 0x00000800;
        private const string StartErrorProperty = "TDock.Native.StartError.v2";

        private readonly uint legacyControlMessage = RegisterWindowMessage("TDock.Native.Control.v1");
        private readonly uint controlMessage = RegisterWindowMessage("TDock.Native.Control.v2");
        private IntPtr nativeLibrary;
        private IntPtr hookCallback;
        private int appliedSettings;
        private IntPtr appliedTaskList;
        private uint failedStopProcess;

        internal static bool ValidDuration(int duration) {
            return duration >= MinimumDuration && duration <= MaximumDuration && duration % DurationStep == 0;
        }

        internal static bool IsSupportedWindows() {
            Version version = Environment.OSVersion.Version;
            return Environment.Is64BitProcess && version.Major == 10 && version.Build == 19045;
        }

        internal EffectResult Sync(bool animate, bool center, int duration) {
            bool enabled = animate || center;
            int settings = duration | (animate ? AnimateFlag : 0) | (center ? CenterFlag : 0);
            if (!IsSupportedWindows()) {
                return new EffectResult(EffectState.Unsupported, "此版本支持 Windows 10 22H2 x64。");
            }
            if (controlMessage == 0) {
                return new EffectResult(EffectState.Error, "无法初始化 TDock 与任务栏的通信。");
            }

            IntPtr taskList = FindTaskList();
            if (taskList == IntPtr.Zero) {
                return enabled
                    ? new EffectResult(EffectState.Waiting, "正在等待主屏原生任务栏。")
                    : new EffectResult(EffectState.Off, "已停用");
            }

            ulong state;
            if (!Send(taskList, controlMessage, QueryCommand, 0, out state)) {
                return new EffectResult(EffectState.Waiting, "任务栏暂时未响应。");
            }

            if (state != BackendVersion) {
                // 0.1.0 can still be active if its controller just exited and Explorer
                // has not yet processed the watchdog timer.
                ulong legacyState;
                if (legacyControlMessage == 0 ||
                    !Send(taskList, legacyControlMessage, QueryCommand, 0, out legacyState)) {
                    return new EffectResult(EffectState.Waiting, "正在确认旧版 TDock 已停用。");
                }
                if (legacyState == NativeActive) {
                    EffectResult legacyStop = Stop(taskList, legacyControlMessage);
                    if (legacyStop.State != EffectState.Off) return legacyStop;
                }
            }

            if (state != 0 && (state != BackendVersion || !enabled)) {
                EffectResult stop = Stop(taskList, controlMessage);
                if (!enabled || stop.State != EffectState.Off) return stop;
                state = 0;
            }

            if (!enabled) return new EffectResult(EffectState.Off, "已停用");

            if (state != BackendVersion) {
                appliedSettings = 0;
                appliedTaskList = IntPtr.Zero;
                EffectResult start = Start(taskList, settings);
                if (start.State != EffectState.On) return start;
                if (!Send(taskList, controlMessage, QueryCommand, 0, out state) || state != BackendVersion) {
                    return new EffectResult(EffectState.Waiting, "正在等待当前版本的 TDock 后端。");
                }
                appliedSettings = settings;
                appliedTaskList = taskList;
            }

            CenterState layout = CenterState.Off;
            if (center) {
                ulong result;
                layout = Send(taskList, controlMessage, QueryCenterCommand, 0, out result) && result <= (ulong)CenterState.Unavailable
                    ? (CenterState)result : CenterState.Waiting;
                if (layout == CenterState.Off) layout = CenterState.Waiting;
            }

            if (appliedTaskList != taskList || appliedSettings != settings) {
                ulong applied;
                if (!Send(taskList, controlMessage, ConfigureCommand, settings, out applied) || applied == NativeBusy) {
                    return new EffectResult(EffectState.Waiting, "正在等待任务栏更新。", layout);
                }
                if (applied == NativeUnsupported || applied == NativeStartFailed) {
                    return Failure(applied, center, animate, layout);
                }
                if (applied == NativeStopFailed) {
                    GetWindowThreadProcessId(taskList, out failedStopProcess);
                    return new EffectResult(EffectState.Error, "部分绘制连接将在 Explorer 重启后清除。", layout, center);
                }
                if (applied != NativeActive) return new EffectResult(EffectState.Waiting, "正在等待任务栏更新。", layout);
                appliedSettings = settings;
                appliedTaskList = taskList;
            }
            return new EffectResult(animate ? EffectState.On : EffectState.Off,
                animate ? "已启用 · Windows 原生任务栏" : "动画已停用", layout);
        }

        private EffectResult Start(IntPtr taskList, int settings) {
            uint pid;
            uint thread = GetWindowThreadProcessId(taskList, out pid);
            if (thread == 0 || pid == 0) return new EffectResult(EffectState.Waiting, "正在等待任务栏重建。");
            if (pid == failedStopProcess) {
                return new EffectResult(EffectState.Error, "请先重启资源管理器，再启用 TDock。");
            }

            try {
                using (var shell = Process.GetProcessById((int)pid)) {
                    string systemExplorer = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe");
                    if (!String.Equals(shell.MainModule.FileName, systemExplorer, StringComparison.OrdinalIgnoreCase)) {
                        return new EffectResult(EffectState.Unsupported, "当前任务栏由其他程序提供。");
                    }
                    foreach (ProcessModule module in shell.Modules) {
                        if (module.ModuleName.IndexOf("taskdock-win10-native", StringComparison.OrdinalIgnoreCase) >= 0) {
                            return new EffectResult(EffectState.Conflict, "请先在 Windhawk 中停用旧版 TaskDock。");
                        }
                    }
                }

                EnsureLibrary();
                IntPtr hook = SetWindowsHookEx(CallWndProc, hookCallback, nativeLibrary, thread);
                if (hook == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());

                bool delivered;
                ulong result;
                try {
                    delivered = Send(taskList, controlMessage, EnableCommand, settings, out result);
                } finally {
                    UnhookWindowsHookEx(hook);
                }

                if (!delivered) return new EffectResult(EffectState.Waiting, "任务栏尚未确认启用，正在重试。");
                ulong error = unchecked((ulong)GetProp(taskList, StartErrorProperty).ToInt64());
                if (error == NativeStopFailed) {
                    failedStopProcess = pid;
                    return new EffectResult(EffectState.Error, "部分绘制连接将在 Explorer 重启后清除。");
                }
                if (error == NativeUnsupported || error == NativeStartFailed) {
                    ulong layout;
                    bool keepCenter = (settings & CenterFlag) != 0 &&
                        Send(taskList, controlMessage, QueryCenterCommand, 0, out layout) && layout != 0;
                    return Failure(error, keepCenter, (settings & AnimateFlag) != 0, CenterState.Waiting);
                }
                if (result == NativeActive) return new EffectResult(EffectState.On, "已启用");
                if (error == NativeBusy || !IsWindow(taskList)) {
                    return new EffectResult(EffectState.Waiting, "正在等待任务栏重建。");
                }
                return new EffectResult(EffectState.Error, "任务栏未能启动 TDock。");
            } catch (ArgumentException) {
                return new EffectResult(EffectState.Waiting, "正在等待任务栏重建。");
            } catch (InvalidOperationException) {
                return new EffectResult(EffectState.Waiting, "正在等待资源管理器就绪。");
            } catch (Win32Exception error) {
                return new EffectResult(EffectState.Error, "无法接入任务栏（Windows 错误 " + error.NativeErrorCode + "）。");
            } catch (IOException error) {
                return new EffectResult(EffectState.Error, "无法读取 TDock 组件。 " + error.Message);
            } catch (UnauthorizedAccessException) {
                return new EffectResult(EffectState.Error, "无法读取 TDock 组件。请检查安装目录权限。");
            }
        }

        private static EffectResult Failure(ulong error, bool center, bool animate, CenterState layout) {
            string message = error == NativeUnsupported
                ? "此 Explorer 版本或任务栏位置尚未适配。"
                : "任务栏未能应用 TDock 设置。";
            return new EffectResult(error == NativeUnsupported ? EffectState.Unsupported : EffectState.Error,
                                    message, center ? layout : CenterState.Off, center && animate);
        }

        private EffectResult Stop(IntPtr taskList, uint message) {
            appliedTaskList = IntPtr.Zero;
            appliedSettings = 0;
            ulong state;
            for (int attempt = 0; attempt < 3; attempt++) {
                if (!IsWindow(taskList)) return new EffectResult(EffectState.Off, "已停用");
                if (!Send(taskList, message, DisableCommand, 0, out state)) {
                    return new EffectResult(EffectState.Waiting, "正在等待任务栏恢复原生绘制。");
                }
                if (state == NativeStopped || state == 0) return new EffectResult(EffectState.Off, "已停用");
                if (state == NativeStopFailed) {
                    GetWindowThreadProcessId(taskList, out failedStopProcess);
                    return new EffectResult(EffectState.Error, "动画已停止。部分绘制连接将在 Explorer 重启后清除。");
                }
                if (state != NativeBusy) break;
                Thread.Sleep(30);
            }
            return new EffectResult(EffectState.Waiting, "正在等待任务栏完成当前绘制。");
        }

        private void EnsureLibrary() {
            if (hookCallback != IntPtr.Zero) return;
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "TDockNative.dll");
            if (!File.Exists(path)) throw new FileNotFoundException("请将 TDockNative.dll 与 TDock.exe 放在一起。");
            path = RuntimeLibrary.Prepare(path);
            nativeLibrary = LoadLibraryEx(path, IntPtr.Zero, LoadLibrarySearchDllLoadDir | LoadLibrarySearchSystem32);
            if (nativeLibrary == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());

            try {
                VerifyBackend(nativeLibrary);
                hookCallback = GetProcAddress(nativeLibrary, "TDockTaskbarHook");
                if (hookCallback == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            } catch {
                FreeLibrary(nativeLibrary);
                nativeLibrary = IntPtr.Zero;
                throw;
            }
        }

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate uint ReadBackendVersion();

        internal static void VerifyBackend(IntPtr module) {
            IntPtr address = GetProcAddress(module, "TDockBackendVersion");
            if (address != IntPtr.Zero) {
                var version = (ReadBackendVersion)Marshal.GetDelegateForFunctionPointer(address, typeof(ReadBackendVersion));
                if (version() == BackendVersion) return;
            }
            throw new IOException("TDock.exe 与 TDockNative.dll 的版本不匹配。请从同一个 ZIP 完整解压。");
        }

        private static IntPtr FindTaskList() {
            IntPtr window = FindWindow("Shell_TrayWnd", null);
            foreach (string name in new[] { "ReBarWindow32", "MSTaskSwWClass", "MSTaskListWClass" }) {
                if (window == IntPtr.Zero) return IntPtr.Zero;
                window = FindWindowEx(window, IntPtr.Zero, name, null);
            }
            return window;
        }

        private static bool Send(IntPtr window, uint message, uint command, int parameter, out ulong result) {
            UIntPtr reply;
            bool sent = SendMessageTimeout(window, message, new UIntPtr(command), new IntPtr(parameter),
                                           SendFlags, 1200, out reply) != IntPtr.Zero;
            result = reply.ToUInt64();
            return sent;
        }


        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern uint RegisterWindowMessage(string name);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindow(string className, string title);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool IsWindow(IntPtr window);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SetWindowsHookEx(int type, IntPtr callback, IntPtr module, uint threadId);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UnhookWindowsHookEx(IntPtr hook);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam,
                                                        uint flags, uint timeout, out UIntPtr result);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetProp(IntPtr window, string name);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr reserved, uint flags);

        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, [MarshalAs(UnmanagedType.LPStr)] string name);

        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr module);
    }
}
