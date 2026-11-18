using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace TDock {
    internal sealed class TrayPanelEdge : NativeWindow, IDisposable {
        private const int Popup = unchecked((int)0x80000000);
        private const int Layered = 0x00080000;
        private const int ToolWindow = 0x00000080;
        private const int NoActivate = 0x08000000;
        private const int Transparent = 0x00000020;
        private const int UseAlpha = 2;
        private const int HideWindow = 0;
        private const int ShowNoActivate = 4;
        private const int KeepSize = 0x0001;
        private const int KeepZOrder = 0x0004;
        private const int KeepActivation = 0x0010;
        private const int KeepOwnerZOrder = 0x0200;

        internal bool IsShown { get; private set; }

        internal bool Show(IntPtr owner, Rectangle bounds, int radius, int inset, Color surface, Color border) {
            if (Handle == IntPtr.Zero) {
                try {
                    CreateHandle(new CreateParams {
                        Style = Popup, ExStyle = Layered | ToolWindow | NoActivate | Transparent, Parent = owner
                    });
                } catch (Win32Exception) {
                    return false;
                }
            }

            if (!PaintEdge(bounds, radius, inset, surface, border)) return false;
            if (!IsShown) ShowWindow(Handle, ShowNoActivate);
            IsShown = true;
            return true;
        }

        internal bool Move(Point location) {
            return SetWindowPos(Handle, IntPtr.Zero, location.X, location.Y, 0, 0,
                KeepSize | KeepZOrder | KeepActivation | KeepOwnerZOrder);
        }

        internal void Hide() {
            if (IsShown && Handle != IntPtr.Zero) ShowWindow(Handle, HideWindow);
            IsShown = false;
        }

        public void Dispose() {
            IsShown = false;
            if (Handle != IntPtr.Zero) DestroyHandle();
        }

        private bool PaintEdge(Rectangle bounds, int radius, int inset, Color surface, Color border) {
            IntPtr dc = CreateCompatibleDC(IntPtr.Zero);
            if (dc == IntPtr.Zero) return false;

            IntPtr dib = IntPtr.Zero;
            IntPtr previous = IntPtr.Zero;
            try {
                var info = new BitmapInfoHeader {
                    HeaderSize = (uint)Marshal.SizeOf(typeof(BitmapInfoHeader)),
                    Width = bounds.Width, Height = -bounds.Height, Planes = 1, BitsPerPixel = 32
                };
                IntPtr pixels;
                dib = CreateDIBSection(dc, ref info, 0, out pixels, IntPtr.Zero, 0);
                if (dib == IntPtr.Zero || pixels == IntPtr.Zero) return false;

                previous = SelectObject(dc, dib);
                if (previous == IntPtr.Zero || previous == new IntPtr(-1)) return false;

                // Leave a one-pixel overlap at the seam with the form's opaque center.
                var hole = Rectangle.Inflate(new Rectangle(Point.Empty, bounds.Size), -(inset + 1), -(inset + 1));
                using (var bitmap = new Bitmap(bounds.Width, bounds.Height, bounds.Width * 4, PixelFormat.Format32bppPArgb, pixels))
                using (var graphics = Graphics.FromImage(bitmap))
                using (var outline = TrayDrawing.Rounded(new Rectangle(Point.Empty, bounds.Size), radius))
                using (var inside = TrayDrawing.Rounded(new Rectangle(1, 1, bounds.Width - 2, bounds.Height - 2), radius - 1))
                using (var borderBrush = new SolidBrush(border))
                using (var surfaceBrush = new SolidBrush(surface)) {
                    graphics.Clear(Color.Transparent);
                    graphics.ExcludeClip(hole);
                    graphics.SmoothingMode = SmoothingMode.AntiAlias;
                    graphics.FillPath(borderBrush, outline);
                    graphics.FillPath(surfaceBrush, inside);
                }

                Point destination = bounds.Location;
                Size size = bounds.Size;
                Point source = Point.Empty;
                var blend = new BlendFunction { SourceConstantAlpha = 255, AlphaFormat = 1 };
                return UpdateLayeredWindow(Handle, IntPtr.Zero, ref destination, ref size, dc, ref source, 0, ref blend, UseAlpha);
            } finally {
                if (previous != IntPtr.Zero && previous != new IntPtr(-1)) SelectObject(dc, previous);
                if (dib != IntPtr.Zero) DeleteObject(dib);
                DeleteDC(dc);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BitmapInfoHeader {
            internal uint HeaderSize;
            internal int Width;
            internal int Height;
            internal ushort Planes;
            internal ushort BitsPerPixel;
            internal uint Compression;
            internal uint ImageSize;
            internal int HorizontalPixelsPerMeter;
            internal int VerticalPixelsPerMeter;
            internal uint ColorsUsed;
            internal uint ImportantColors;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BlendFunction {
            internal byte BlendOp;
            internal byte BlendFlags;
            internal byte SourceConstantAlpha;
            internal byte AlphaFormat;
        }

        [DllImport("user32.dll")]
        private static extern bool UpdateLayeredWindow(IntPtr window, IntPtr destinationDc, ref Point destination,
            ref Size size, IntPtr sourceDc, ref Point source, int colorKey, ref BlendFunction blend, int flags);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr window, int command);

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, int flags);

        [DllImport("gdi32.dll")]
        private static extern IntPtr CreateCompatibleDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        private static extern IntPtr CreateDIBSection(IntPtr dc, ref BitmapInfoHeader info, uint usage, out IntPtr bits,
            IntPtr section, uint offset);

        [DllImport("gdi32.dll")]
        private static extern IntPtr SelectObject(IntPtr dc, IntPtr value);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteObject(IntPtr value);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteDC(IntPtr dc);
    }
}
