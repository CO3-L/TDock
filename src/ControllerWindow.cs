using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace TDock {
    internal sealed class ControllerWindow : NativeWindow, IDisposable {
        internal const string MutexName = @"Local\TDock.Controller.v1";
        private const string Caption = "TDock.Controller.Window";
        private static readonly IntPtr MessageOnlyWindow = new IntPtr(-3);
        private static readonly uint ControllerMessage = RegisterWindowMessage("TDock.Controller.Command");
        private const int Exit = 2;

        private readonly Action requestExit;

        internal ControllerWindow(Action requestExit) {
            this.requestExit = requestExit;
            CreateHandle(new CreateParams { Caption = Caption, Parent = MessageOnlyWindow });
        }

        protected override void WndProc(ref Message message) {
            if (message.Msg == ControllerMessage && ControllerMessage != 0 && message.WParam == new IntPtr(Exit)) {
                requestExit();
                message.Result = new IntPtr(1);
                return;
            }
            base.WndProc(ref message);
        }

        internal static bool ShutdownRunningInstance(Mutex controller) {
            for (int attempt = 0; attempt < 25; attempt++) {
                if (TryAcquire(controller, 0)) {
                    controller.ReleaseMutex();
                    return true;
                }
                PostExit();
                if (TryAcquire(controller, 200)) {
                    controller.ReleaseMutex();
                    return true;
                }
            }
            return false;
        }

        internal static bool TryAcquire(Mutex controller, int timeout) {
            try {
                return controller.WaitOne(timeout);
            } catch (AbandonedMutexException) {
                return true;
            }
        }

        internal static void PostExit(int? processId = null) {
            if (ControllerMessage == 0) return;
            IntPtr window = IntPtr.Zero;
            while ((window = FindWindowEx(MessageOnlyWindow, window, null, Caption)) != IntPtr.Zero) {
                uint owner;
                if (processId.HasValue &&
                    (GetWindowThreadProcessId(window, out owner) == 0 || owner != processId.Value)) continue;
                PostMessage(window, ControllerMessage, new IntPtr(Exit), IntPtr.Zero);
                return;
            }
        }

        public void Dispose() {
            DestroyHandle();
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern uint RegisterWindowMessage(string name);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string name, string caption);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    }
}
