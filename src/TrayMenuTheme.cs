using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

namespace TDock {
    internal sealed class TrayMenuTheme {
        private static readonly TrayMenuTheme Light = new TrayMenuTheme(
            "#FCFAF5", "#292822", "#665F53", "#F3EEE5", "#DED7C9", "#F0EBE0", "#746E62");
        private static readonly TrayMenuTheme Dark = new TrayMenuTheme(
            "#262624", "#F2EEE3", "#BEB7A8", "#37352F", "#4B4740", "#34322D", "#A19888");

        internal readonly Color Surface;
        internal readonly Color Text;
        internal readonly Color Muted;
        internal readonly Color Hover;
        internal readonly Color Border;
        internal readonly Color Field;
        internal readonly Color SwitchOff;
        internal readonly bool HighContrast;

        private TrayMenuTheme(string surface, string text, string muted, string hover, string border, string field, string switchOff) {
            Surface = ColorTranslator.FromHtml(surface);
            Text = ColorTranslator.FromHtml(text);
            Muted = ColorTranslator.FromHtml(muted);
            Hover = ColorTranslator.FromHtml(hover);
            Border = ColorTranslator.FromHtml(border);
            Field = ColorTranslator.FromHtml(field);
            SwitchOff = ColorTranslator.FromHtml(switchOff);
        }

        private TrayMenuTheme() {
            HighContrast = true;
            Surface = Field = SystemColors.Window;
            Text = SystemColors.WindowText;
            Muted = SystemColors.WindowText;
            Hover = SystemColors.Window;
            Border = SystemColors.WindowText;
            SwitchOff = SystemColors.WindowText;
        }

        internal static TrayMenuTheme Current() {
            if (SystemInformation.HighContrast) return new TrayMenuTheme();
            try {
                object setting = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
                    "AppsUseLightTheme", 1);
                return Equals(setting, 0) ? Dark : Light;
            } catch (UnauthorizedAccessException) {
                return Light;
            } catch (System.Security.SecurityException) {
                return Light;
            }
        }
    }

    internal static class TrayDrawing {
        internal static int Scale(Control control, int value) {
            return (value * control.DeviceDpi + 48) / 96;
        }

        internal static GraphicsPath Rounded(RectangleF bounds, float radius) {
            float diameter = Math.Min(2 * radius, Math.Min(bounds.Width, bounds.Height));
            var path = new GraphicsPath();
            if (diameter <= 0) {
                path.AddRectangle(bounds);
                return path;
            }
            path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180, 90);
            path.AddArc(bounds.Right - diameter, bounds.Top, diameter, diameter, 270, 90);
            path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0, 90);
            path.AddArc(bounds.Left, bounds.Bottom - diameter, diameter, diameter, 90, 90);
            path.CloseFigure();
            return path;
        }
    }

    internal sealed class TraySwitch : CheckBox {
        private const int StatusWidth = 80;
        private TrayMenuTheme theme;
        private bool hovered;

        internal string StatusText { get; private set; }

        internal TraySwitch(string label) {
            StatusText = "";
            Text = label;
            FlatStyle = FlatStyle.Flat;
            AutoSize = false;
            Height = 38;
            MinimumSize = new Size(0, 38);
            Dock = DockStyle.Fill;
            Margin = Padding.Empty;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        internal void ApplyTheme(TrayMenuTheme current) {
            theme = current;
            Invalidate();
        }

        internal void SetStatus(string text) {
            if (StatusText == text) return;
            StatusText = text;
            Invalidate();
        }

        protected override void OnMouseEnter(EventArgs args) {
            base.OnMouseEnter(args);
            hovered = true;
            Invalidate();
        }

        protected override void OnMouseLeave(EventArgs args) {
            base.OnMouseLeave(args);
            hovered = false;
            Invalidate();
        }

        protected override void OnCheckedChanged(EventArgs args) {
            base.OnCheckedChanged(args);
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs args) {
            if (theme == null || theme.HighContrast) {
                base.OnPaint(args);
                if (theme != null) DrawStatus(args.Graphics, Width - TrayDrawing.Scale(this, 8));
                return;
            }

            var graphics = args.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.Clear(theme.Surface);
            if (hovered && Enabled) {
                using (var shape = TrayDrawing.Rounded(ClientRectangle, TrayDrawing.Scale(this, 7)))
                using (var fill = new SolidBrush(theme.Hover)) graphics.FillPath(fill, shape);
            }

            int inset = TrayDrawing.Scale(this, 8);
            int width = TrayDrawing.Scale(this, 36);
            int height = TrayDrawing.Scale(this, 21);
            var track = new RectangleF(Width - inset - width, (Height - height) / 2f, width, height - 1);
            int statusRight = (int)track.X - TrayDrawing.Scale(this, 12);
            int labelRight = StatusText.Length == 0 ? (int)track.X - inset : statusRight - TrayDrawing.Scale(this, StatusWidth + 4);
            var label = new Rectangle(inset, 0, Math.Max(0, labelRight - inset), Height);
            var textColor = Enabled ? theme.Text : theme.Muted;
            TextRenderer.DrawText(graphics, Text, Font, label, textColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
            DrawStatus(graphics, statusRight);

            Color switchColor = Checked && Enabled ? theme.Text : theme.SwitchOff;
            using (var outline = TrayDrawing.Rounded(track, height / 2f))
            using (var pen = new Pen(switchColor))
            using (var fill = new SolidBrush(switchColor)) {
                if (Checked) graphics.FillPath(fill, outline);
                else graphics.DrawPath(pen, outline);
            }
            float insetThumb = TrayDrawing.Scale(this, 4);
            float diameter = track.Height - 2 * insetThumb + 1;
            float left = Checked ? track.Right - insetThumb - diameter : track.Left + insetThumb;
            using (var thumb = new SolidBrush(Checked ? theme.Surface : switchColor)) {
                graphics.FillEllipse(thumb, left, track.Y + insetThumb - 0.5f, diameter, diameter);
            }

            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(graphics, Rectangle.Inflate(ClientRectangle, -2, -2));
        }

        private void DrawStatus(Graphics graphics, int right) {
            if (StatusText.Length == 0) return;
            int width = TrayDrawing.Scale(this, StatusWidth);
            var bounds = new Rectangle(right - width, 0, width, Height);
            using (var font = new Font(Font.FontFamily, Font.SizeInPoints * 0.9f, Font.Style)) {
                TextRenderer.DrawText(graphics, StatusText, font, bounds, theme.Muted,
                    TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
            }
        }
    }

    internal sealed class DurationSlider : TrackBar {
        private const int GetThumbRect = 0x0419;
        private const int GetChannelRect = 0x041A;
        private const int BeginDrag = 0x0201;
        private const int EndDrag = 0x0202;
        private const int BeginDoubleClick = 0x0203;
        private const int CaptureChanged = 0x0215;
        private const int CancelMode = 0x001F;

        private TrayMenuTheme theme;
        private bool dragging;
        internal event EventHandler Committed;

        internal int Milliseconds {
            get { return Value * NativeEffects.DurationStep; }
        }

        internal DurationSlider(int milliseconds) {
            AccessibleName = "弹跳时长";
            AccessibleDescription = "单位为秒，方向键每次调整 0.01 秒。";
            AutoSize = false;
            Dock = DockStyle.Fill;
            Margin = Padding.Empty;
            TickStyle = TickStyle.None;
            Minimum = NativeEffects.MinimumDuration / NativeEffects.DurationStep;
            Maximum = NativeEffects.MaximumDuration / NativeEffects.DurationStep;
            SmallChange = 1;
            LargeChange = 5;
            Value = milliseconds / NativeEffects.DurationStep;
            SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        internal void ApplyTheme(TrayMenuTheme current) {
            theme = current;
            BackColor = current.Field;
            Invalidate();
        }

        protected override void WndProc(ref Message message) {
            if (message.Msg == BeginDrag || message.Msg == BeginDoubleClick) dragging = true;
            base.WndProc(ref message);
            if (dragging && (message.Msg == EndDrag || message.Msg == CaptureChanged || message.Msg == CancelMode)) {
                dragging = false;
                RaiseCommitted();
            }
        }

        protected override void OnValueChanged(EventArgs args) {
            base.OnValueChanged(args);
            Invalidate();
            if (!dragging) RaiseCommitted();
        }

        protected override void OnGotFocus(EventArgs args) {
            base.OnGotFocus(args);
            Invalidate();
        }

        protected override void OnLostFocus(EventArgs args) {
            base.OnLostFocus(args);
            Invalidate();
        }

        private void RaiseCommitted() {
            var handler = Committed;
            if (handler != null) handler(this, EventArgs.Empty);
        }

        protected override void OnPaint(PaintEventArgs args) {
            if (theme == null || !IsHandleCreated) return;
            NativeRect thumb;
            NativeRect channel;
            SendMessage(Handle, GetThumbRect, IntPtr.Zero, out thumb);
            SendMessage(Handle, GetChannelRect, IntPtr.Zero, out channel);
            var graphics = args.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.Clear(theme.Field);

            float x = (thumb.Left + thumb.Right) / 2f;
            float y = (thumb.Top + thumb.Bottom) / 2f;
            float left = channel.Left + (thumb.Right - thumb.Left) / 2f;
            float right = channel.Right - (thumb.Right - thumb.Left) / 2f - 1;
            if (right <= left || thumb.Right <= thumb.Left) return;
            left = Math.Min(left, x);
            right = Math.Max(right, x);
            var accent = Enabled ? theme.Text : SystemColors.GrayText;
            using (var rail = new Pen(theme.Border, TrayDrawing.Scale(this, 4)) { StartCap = LineCap.Round, EndCap = LineCap.Round })
                graphics.DrawLine(rail, left, y, right, y);
            using (var fill = new Pen(accent, TrayDrawing.Scale(this, 4)) { StartCap = LineCap.Round, EndCap = LineCap.Round })
                if (x > left) graphics.DrawLine(fill, left, y, x, y);

            float radius = TrayDrawing.Scale(this, 7);
            using (var ring = new Pen(theme.Field, TrayDrawing.Scale(this, 2)))
            using (var dot = new SolidBrush(accent)) {
                var circle = new RectangleF(x - radius, y - radius, 2 * radius, 2 * radius);
                graphics.DrawEllipse(ring, circle);
                graphics.FillEllipse(dot, circle);
            }
            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(graphics, Rectangle.Inflate(ClientRectangle, -1, -1));
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeRect {
            internal int Left;
            internal int Top;
            internal int Right;
            internal int Bottom;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr SendMessage(IntPtr window, int message, IntPtr wParam, out NativeRect rect);
    }
}
