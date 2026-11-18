using System;
using System.IO;
using Microsoft.Win32;

namespace TDock {
    internal static class Preferences {
        private const string SettingsKey = @"HKEY_CURRENT_USER\Software\TDock";
        internal const string ShutdownArgument = "--shutdown";

        internal static bool EffectsEnabled {
            get { return ReadInt("EffectsEnabled", 1) == 1; }
            set { WriteInt("EffectsEnabled", value ? 1 : 0); }
        }

        internal static bool CenterTaskbar {
            get { return ReadInt("CenterTaskbar", 1) == 1; }
            set { WriteInt("CenterTaskbar", value ? 1 : 0); }
        }

        internal static int Duration {
            get {
                int value = ReadInt("ClickDurationMs", NativeEffects.DefaultDuration);
                return NativeEffects.ValidDuration(value) ? value : NativeEffects.DefaultDuration;
            }
            set { WriteInt("ClickDurationMs", value); }
        }

        private static int ReadInt(string name, int fallback) {
            try {
                object value = Registry.GetValue(SettingsKey, name, null);
                return value is int ? (int)value : fallback;
            } catch (System.Security.SecurityException) {
                return fallback;
            } catch (UnauthorizedAccessException) {
                return fallback;
            } catch (IOException) {
                return fallback;
            }
        }

        private static void WriteInt(string name, int value) {
            Registry.SetValue(SettingsKey, name, value, RegistryValueKind.DWord);
        }
    }
}
