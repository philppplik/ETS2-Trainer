using System.ComponentModel;
using System.Windows;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.App.ViewModels;

namespace Ets2Trainer.App;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private readonly HotkeyService _hotkey = new();

    /// <param name="offline">Screenshot mode: no bridge connection and no global hotkey.</param>
    public MainWindow(bool offline = false)
    {
        InitializeComponent();
        _vm = new MainViewModel(offline);
        DataContext = _vm;
        Topmost = _vm.Settings.AlwaysOnTop;

        _vm.SettingsChanged += () => Topmost = _vm.Settings.AlwaysOnTop;
        _vm.HotkeyChanged += RegisterHotkey;
        _vm.Live.MenuHotkeyPressed += ToggleVisibility;
        _hotkey.Pressed += ToggleVisibility;
        if (offline)
        {
            return;
        }

        SourceInitialized += (_, _) =>
        {
            _hotkey.Attach(this);
            RegisterHotkey();
        };
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        _hotkey.Dispose();
        _vm.Dispose();
        base.OnClosing(e);
    }

    private void RegisterHotkey()
    {
        if (!_hotkey.Register(_vm.Settings.MenuHotkeyVk))
        {
            // Another program owns the key; the in-game plugin still reports presses while ETS2 has focus.
            _vm.Toast($"Hotkey {KeyNames.Name(_vm.Settings.MenuHotkeyVk)} ist belegt – funktioniert nur im Spiel.");
        }
    }

    /// <summary>Menu hotkey: bring the trainer to the front, or hide it again when it is already in front.</summary>
    private void ToggleVisibility()
    {
        if (IsVisible && IsActive && WindowState != WindowState.Minimized)
        {
            WindowState = WindowState.Minimized;
            return;
        }

        Show();
        if (WindowState == WindowState.Minimized)
        {
            WindowState = WindowState.Normal;
        }

        Activate();
        Topmost = true;
        Topmost = _vm.Settings.AlwaysOnTop;
        Focus();
    }

    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
