using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace Ets2Trainer.App.Infrastructure;

/// <summary>System-wide hotkey via RegisterHotKey (works while the game has focus).</summary>
public sealed class HotkeyService : IDisposable
{
    private const int WmHotkey = 0x0312;
    private const int HotkeyId = 0xE275;
    private const uint ModNoRepeat = 0x4000;

    private HwndSource? _source;
    private bool _registered;

    public event Action? Pressed;

    public void Attach(Window window)
    {
        var handle = new WindowInteropHelper(window).EnsureHandle();
        _source = HwndSource.FromHwnd(handle);
        _source?.AddHook(WndProc);
    }

    /// <summary>Registers <paramref name="virtualKey"/> (no modifiers). Returns false if another app owns it.</summary>
    public bool Register(uint virtualKey)
    {
        Unregister();
        if (_source is null || virtualKey == 0)
        {
            return false;
        }

        _registered = RegisterHotKey(_source.Handle, HotkeyId, ModNoRepeat, virtualKey);
        return _registered;
    }

    public void Unregister()
    {
        if (_registered && _source is not null)
        {
            UnregisterHotKey(_source.Handle, HotkeyId);
        }

        _registered = false;
    }

    public void Dispose()
    {
        Unregister();
        _source?.RemoveHook(WndProc);
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmHotkey && wParam.ToInt32() == HotkeyId)
        {
            Pressed?.Invoke();
            handled = true;
        }

        return IntPtr.Zero;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
