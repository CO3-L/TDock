using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace TDock {
    internal sealed class TrayPanel : Form {
        private const int DwmCloak = 13;
        private const uint InvalidateAndPaintChildren = 0x0001 | 0x0004 | 0x0080 | 0x0100;
        private const string OverflowClass = "NotifyIconOverflowWindow";
        private const uint RootWindow = 2;
        private const int WindowActivation = 0x0006;
        private const int KeyDownMessage = 0x0100;

        internal readonly TraySwitch AnimationSwitch;
        internal readonly TraySwitch CenterSwitch;
        internal readonly TraySwitch StartupSwitch;
        internal readonly DurationSlider Duration;
        internal readonly Button ExitButton;

        private readonly TableLayoutPanel layout;
        private readonly RoundedTrayCard durationCard;
        private readonly TrayPanelEdge edge = new TrayPanelEdge();
        private readonly ToolTip centerDetails = new ToolTip();
        private readonly Label readout;
        private readonly Label minimum;
        private readonly Label maximum;
        private readonly Label status;
        private readonly Label saveStatus;
        private readonly Font titleFont;
        private TrayMenuTheme theme;
        private Point popupAnchor;
        private IntPtr overflow;

        internal TrayPanel(int duration, bool supported) {
            Text = "TDock";
            AccessibleName = "TDock 托盘菜单";
            Font = SystemFonts.MenuFont;
            AutoScaleDimensions = new SizeF(96, 96);
            AutoScaleMode = AutoScaleMode.Dpi;
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            TopMost = true;
            StartPosition = FormStartPosition.Manual;
            Padding = new Padding(14, 12, 14, 10);
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;

            titleFont = new Font(Font.FontFamily, 11, FontStyle.Bold);
            var title = new Label {
                Text = "TDock", AutoSize = true, Font = titleFont, Margin = new Padding(8, 1, 8, 9)
            };
            AnimationSwitch = new TraySwitch("动画") { Enabled = supported };
            CenterSwitch = new TraySwitch("居中") { Enabled = supported };
            StartupSwitch = new TraySwitch("开机自启") { Enabled = supported, Checked = StartupShortcut.Enabled };
            Duration = new DurationSlider(duration) { Enabled = supported };
            readout = CreateLabel("", ContentAlignment.MiddleRight);
            minimum = CreateLabel(Seconds(NativeEffects.MinimumDuration), ContentAlignment.MiddleLeft);
            maximum = CreateLabel(Seconds(NativeEffects.MaximumDuration), ContentAlignment.MiddleRight);
            Duration.ValueChanged += (sender, args) => UpdateDurationReadout();
            UpdateDurationReadout();

            var durationLayout = new TableLayoutPanel {
                ColumnCount = 2, RowCount = 3, Dock = DockStyle.Fill, Margin = Padding.Empty
            };
            durationLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 60));
            durationLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));
            durationLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
            durationLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            durationLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 20));
            durationLayout.Controls.Add(CreateLabel("弹跳时长", ContentAlignment.MiddleLeft), 0, 0);
            durationLayout.Controls.Add(readout, 1, 0);
            durationLayout.Controls.Add(Duration, 0, 1);
            durationLayout.SetColumnSpan(Duration, 2);
            durationLayout.Controls.Add(minimum, 0, 2);
            durationLayout.Controls.Add(maximum, 1, 2);
            durationCard = new RoundedTrayCard {
                Height = 90, MinimumSize = new Size(0, 90), Dock = DockStyle.Fill,
                Margin = new Padding(0, 5, 0, 6), Padding = new Padding(10, 8, 10, 7)
            };
            durationCard.Controls.Add(durationLayout);

            ExitButton = new Button {
                Text = "退出 TDock", TextAlign = ContentAlignment.MiddleLeft, Dock = DockStyle.Fill,
                Height = 35, MinimumSize = new Size(0, 35), Margin = Padding.Empty,
                Padding = new Padding(6, 0, 0, 0), FlatStyle = FlatStyle.Flat
            };
            ExitButton.FlatAppearance.BorderSize = 0;

            status = CreateNote();
            saveStatus = CreateNote();
            layout = new TableLayoutPanel {
                Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 8, Margin = Padding.Empty,
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink
            };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 292));
            for (int row = 0; row < layout.RowCount; row++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.Controls.Add(title, 0, 0);
            layout.Controls.Add(AnimationSwitch, 0, 1);
            layout.Controls.Add(CenterSwitch, 0, 2);
            layout.Controls.Add(durationCard, 0, 3);
            layout.Controls.Add(StartupSwitch, 0, 4);
            layout.Controls.Add(status, 0, 5);
            layout.Controls.Add(saveStatus, 0, 6);
            layout.Controls.Add(ExitButton, 0, 7);
            Controls.Add(layout);
            UpdateTheme();
        }

        internal void ShowAt(Point anchor) {
            popupAnchor = anchor;
            IntPtr flyout = FindWindow(OverflowClass, null);
            overflow = flyout != IntPtr.Zero && GetAncestor(WindowFromPoint(anchor), RootWindow) == flyout
                ? flyout : IntPtr.Zero;
            UpdateTheme();
            // Lay out wrapped status text before anchoring the popup to the cursor.
            PerformLayout();
            Location = PopupLocation(anchor, GetPreferredSize(Size.Empty));

            IntPtr window = Handle;
            bool staged = !Visible && SetCloaked(window, true);
            try {
                Show();
                Location = PopupLocation(anchor, Size);
                if (staged && Visible) RedrawWindow(window, IntPtr.Zero, IntPtr.Zero, InvalidateAndPaintChildren);
            } finally {
                // The owned corner layer inherits the cloak. Reveal it with the painted controls.
                if (staged) SetCloaked(window, false);
            }
            Activate();
        }

        internal void SetStatus(string message) {
            SetNote(status, message);
        }

        internal void SetCenterStatus(string text, string details) {
            CenterSwitch.SetStatus(text);
            if (CenterSwitch.AccessibleDescription == details) return;
            CenterSwitch.AccessibleDescription = details;
            centerDetails.SetToolTip(CenterSwitch, details);
        }

        internal void SetSaveStatus(string message) {
            SetNote(saveStatus, message);
        }

        private static Label CreateLabel(string text, ContentAlignment alignment) {
            return new Label { Text = text, TextAlign = alignment, Dock = DockStyle.Fill, Margin = Padding.Empty };
        }

        private static Label CreateNote() {
            return new Label {
                AutoSize = true, MaximumSize = new Size(276, 0), Margin = new Padding(8, 3, 8, 4),
                Visible = false
            };
        }

        private static void SetNote(Label note, string text) {
            note.Text = text;
            note.Visible = text.Length != 0;
        }

        private static string Seconds(int milliseconds) {
            return (milliseconds / 1000m).ToString("0.00", CultureInfo.InvariantCulture) + " 秒";
        }

        private void UpdateDurationReadout() {
            readout.Text = Seconds(Duration.Milliseconds);
        }

        private Point PopupLocation(Point anchor, Size popupSize) {
            Rectangle work = Screen.FromPoint(anchor).WorkingArea;
            int gap = TrayDrawing.Scale(this, 8);
            int x = anchor.X + gap - popupSize.Width;
            int y = anchor.Y - gap - popupSize.Height;
            if (y < work.Top) y = anchor.Y + gap;
            return new Point(
                Math.Max(work.Left, Math.Min(x, work.Right - popupSize.Width)),
                Math.Max(work.Top, Math.Min(y, work.Bottom - popupSize.Height)));
        }

        private void UpdateTheme() {
            TrayMenuTheme current = TrayMenuTheme.Current();
            if (current == theme) return;
            theme = current;
            BackColor = current.Surface;
            ForeColor = current.Text;
            durationCard.BackColor = current.Field;
            foreach (Label label in new[] { minimum, maximum, status, saveStatus }) label.ForeColor = current.Muted;
            AnimationSwitch.ApplyTheme(current);
            CenterSwitch.ApplyTheme(current);
            StartupSwitch.ApplyTheme(current);
            Duration.ApplyTheme(current);
            ExitButton.BackColor = current.Surface;
            ExitButton.ForeColor = current.Text;
            ExitButton.FlatAppearance.MouseOverBackColor = current.Hover;
            ExitButton.FlatAppearance.MouseDownBackColor = current.Hover;
            UpdateFrame();
        }

        private void UpdateFrame() {
            if (theme == null || !Visible || !IsHandleCreated) return;

            int radius = TrayDrawing.Scale(this, 13);
            int inset = TrayDrawing.Scale(this, 5);
            Region region = null;
            bool smooth = false;
            if (!theme.HighContrast && Width > 2 * (inset + 1) && Height > 2 * (inset + 1)) {
                smooth = edge.Show(Handle, Bounds, radius, inset, theme.Surface, theme.Border);
                if (smooth) {
                    region = new Region(Rectangle.Inflate(ClientRectangle, -inset, -inset));
                } else {
                    using (var outline = TrayDrawing.Rounded(ClientRectangle, radius)) region = new Region(outline);
                }
            }
            if (!smooth) edge.Hide();
            var previous = Region;
            Region = region;
            if (previous != null) previous.Dispose();
            Invalidate();
        }

        protected override bool ShowWithoutActivation {
            get { return true; }
        }

        protected override CreateParams CreateParams {
            get {
                const int toolWindow = 0x00000080;
                const int appWindow = 0x00040000;
                CreateParams parameters = base.CreateParams;
                parameters.ExStyle = (parameters.ExStyle | toolWindow) & ~appWindow;
                return parameters;
            }
        }

        protected override void OnSizeChanged(EventArgs args) {
            base.OnSizeChanged(args);
            if (Visible) Location = PopupLocation(popupAnchor, Size);
            UpdateFrame();
        }

        protected override void OnLocationChanged(EventArgs args) {
            base.OnLocationChanged(args);
            if (edge != null && edge.IsShown && !edge.Move(Location)) UpdateFrame();
        }

        protected override void OnVisibleChanged(EventArgs args) {
            base.OnVisibleChanged(args);
            if (edge == null) return;
            if (Visible) UpdateFrame();
            else {
                edge.Hide();
                DismissOverflow();
            }
        }

        protected override void OnHandleDestroyed(EventArgs args) {
            if (edge != null) edge.Dispose();
            DismissOverflow();
            base.OnHandleDestroyed(args);
        }

        protected override void OnPaint(PaintEventArgs args) {
            base.OnPaint(args);
            if (theme == null || edge.IsShown || ClientSize.Width < 3 || ClientSize.Height < 3) return;
            var bounds = new RectangleF(0.5f, 0.5f, ClientSize.Width - 1f, ClientSize.Height - 1f);
            using (var outline = TrayDrawing.Rounded(bounds, theme.HighContrast ? 0 : TrayDrawing.Scale(this, 13)))
            using (var border = new Pen(theme.Border)) {
                args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                args.Graphics.DrawPath(border, outline);
            }
        }

        protected override void OnDeactivate(EventArgs args) {
            base.OnDeactivate(args);
            Hide();
        }

        protected override void WndProc(ref Message message) {
            if (overflow != IntPtr.Zero && message.Msg == WindowActivation && (message.WParam.ToInt64() & 0xffff) == 0 &&
                (message.LParam == overflow || message.LParam == FindWindow("Shell_TrayWnd", null))) {
                // The shell handles clicks back into its own notification area.
                overflow = IntPtr.Zero;
            }
            base.WndProc(ref message);
        }

        protected override void OnFormClosing(FormClosingEventArgs args) {
            if (args.CloseReason == CloseReason.UserClosing) {
                args.Cancel = true;
                Hide();
            }
            base.OnFormClosing(args);
        }

        protected override bool ProcessCmdKey(ref Message message, Keys keyData) {
            if (keyData == Keys.Escape) {
                Hide();
                return true;
            }
            return base.ProcessCmdKey(ref message, keyData);
        }

        protected override void Dispose(bool disposing) {
            if (disposing && centerDetails != null) centerDetails.Dispose();
            base.Dispose(disposing);
            if (disposing && titleFont != null) titleFont.Dispose();
        }

        private void DismissOverflow() {
            IntPtr flyout = overflow;
            overflow = IntPtr.Zero;
            if (flyout != IntPtr.Zero && IsWindowVisible(flyout) && flyout == FindWindow(OverflowClass, null)) {
                // Escape takes the shell's own dismissal path and resets its chevron state.
                PostMessage(flyout, KeyDownMessage, new IntPtr((int)Keys.Escape), new IntPtr(1));
            }
        }

        private static bool SetCloaked(IntPtr window, bool enabled) {
            int value = enabled ? 1 : 0;
            return DwmSetWindowAttribute(window, DwmCloak, ref value, sizeof(int)) >= 0;
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

        [DllImport("user32.dll")]
        private static extern bool RedrawWindow(IntPtr window, IntPtr update, IntPtr region, uint flags);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindow(string className, string windowName);

        [DllImport("user32.dll")]
        private static extern IntPtr WindowFromPoint(Point point);

        [DllImport("user32.dll")]
        private static extern IntPtr GetAncestor(IntPtr window, uint flags);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr window);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool PostMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam);

        private sealed class RoundedTrayCard : Panel {
            internal RoundedTrayCard() {
                SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            }

            protected override void OnPaintBackground(PaintEventArgs args) {
                if (Parent == null || SystemInformation.HighContrast) {
                    base.OnPaintBackground(args);
                    return;
                }
                args.Graphics.Clear(Parent.BackColor);
                args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                using (var path = TrayDrawing.Rounded(ClientRectangle, TrayDrawing.Scale(this, 8)))
                using (var fill = new SolidBrush(BackColor)) args.Graphics.FillPath(fill, path);
            }
        }
    }
}
