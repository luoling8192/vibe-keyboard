using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;

namespace VibeKeyboardApp.Input
{
    /// <summary>
    /// Keyboard injection using Win32 SendInput API.
    /// Replaces macOS CGEventCreateKeyboardEvent and WinForms SendKeys.
    /// </summary>
    internal static class KeyboardInjector
    {
        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint Type;
            public INPUTUNION U;
        }

        [StructLayout(LayoutKind.Explicit)]
        private struct INPUTUNION
        {
            [FieldOffset(0)] public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        private const uint INPUT_KEYBOARD = 1;
        private const uint KEYEVENTF_KEYDOWN = 0x0000;
        private const uint KEYEVENTF_KEYUP = 0x0002;
        private const uint KEYEVENTF_UNICODE = 0x0004;

        private const ushort VK_CONTROL = 0x11;
        private const ushort VK_SHIFT = 0x10;
        private const ushort VK_MENU = 0x12; // Alt
        private const ushort VK_LWIN = 0x5B;

        /// <summary>
        /// Send a modifier+key combination (e.g., Ctrl+C, Alt+Tab).
        /// </summary>
        public static void SendShortcut(string modifiers, string key)
        {
            var inputs = new System.Collections.Generic.List<INPUT>();

            // Press modifiers
            foreach (var mod in modifiers)
            {
                ushort vk = mod switch
                {
                    '^' => VK_CONTROL,
                    '+' => VK_SHIFT,
                    '%' => VK_MENU,
                    _ => 0
                };
                if (vk != 0)
                    inputs.Add(MakeKeyInput(vk, KEYEVENTF_KEYDOWN));
            }

            // Press and release key
            var vkKey = VkFromKey(key);
            if (vkKey != 0)
            {
                inputs.Add(MakeKeyInput(vkKey, KEYEVENTF_KEYDOWN));
                inputs.Add(MakeKeyInput(vkKey, KEYEVENTF_KEYUP));
            }

            // Release modifiers (reverse order)
            for (int i = modifiers.Length - 1; i >= 0; i--)
            {
                ushort vk = modifiers[i] switch
                {
                    '^' => VK_CONTROL,
                    '+' => VK_SHIFT,
                    '%' => VK_MENU,
                    _ => (ushort)0
                };
                if (vk != 0)
                    inputs.Add(MakeKeyInput(vk, KEYEVENTF_KEYUP));
            }

            if (inputs.Count > 0)
                SendInput((uint)inputs.Count, inputs.ToArray(), Marshal.SizeOf<INPUT>());
        }

        /// <summary>
        /// Send Enter key.
        /// </summary>
        public static void SendEnter()
        {
            var inputs = new[]
            {
                MakeKeyInput(0x0D, KEYEVENTF_KEYDOWN),  // VK_RETURN
                MakeKeyInput(0x0D, KEYEVENTF_KEYUP)
            };
            SendInput(2, inputs, Marshal.SizeOf<INPUT>());
        }

        /// <summary>
        /// Paste text from clipboard (Ctrl+V).
        /// </summary>
        public static void PasteFromClipboard(string text)
        {
            var thread = new Thread(() =>
            {
                Clipboard.SetText(text);
                SendShortcut("^", "v");
            });
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start();
            thread.Join();
        }

        /// <summary>
        /// Copy selection (Ctrl+C).
        /// </summary>
        public static void CopySelection() => SendShortcut("^", "c");

        /// <summary>
        /// Send Ctrl+C for interrupt.
        /// </summary>
        public static void InterruptControlC() => SendShortcut("^", "c");

        private static INPUT MakeKeyInput(ushort vk, uint flags)
        {
            return new INPUT
            {
                Type = INPUT_KEYBOARD,
                U = new INPUTUNION
                {
                    ki = new KEYBDINPUT
                    {
                        wVk = vk,
                        wScan = 0,
                        dwFlags = flags,
                        time = 0,
                        dwExtraInfo = IntPtr.Zero
                    }
                }
            };
        }

        private static ushort VkFromKey(string key)
        {
            if (key.Length == 1)
            {
                char c = char.ToUpper(key[0]);
                return (ushort)c; // ASCII for A-Z, 0-9
            }
            return key.ToLower() switch
            {
                "enter" or "return" => 0x0D,
                "tab" => 0x09,
                "escape" or "esc" => 0x1B,
                "space" => 0x20,
                "backspace" => 0x08,
                "delete" or "del" => 0x2E,
                "home" => 0x24,
                "end" => 0x23,
                "pageup" => 0x21,
                "pagedown" => 0x22,
                "left" => 0x25,
                "up" => 0x26,
                "right" => 0x27,
                "down" => 0x28,
                _ => 0
            };
        }
    }
}
