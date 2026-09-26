using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.App.ViewModels;

/// <summary>Installation of the in-game plugin, hotkeys and housekeeping.</summary>
public sealed class SetupViewModel : ObservableObject
{
    private readonly MainViewModel _main;

    public SetupViewModel(MainViewModel main)
    {
        _main = main;
        InstallCommand = new RelayCommand(Install, () => _main.GameDirectory is not null);
        UninstallCommand = new RelayCommand(Uninstall, () => _main.GameDirectory is not null);
        OpenBackupsCommand = new RelayCommand(() => OpenFolder(SaveSlotService.BackupRoot));
        OpenLogCommand = new RelayCommand(OpenPluginLog);
        OpenModFolderCommand = new RelayCommand(() => OpenFolder(GamePaths.ModDirectory));
    }

    public ICommand InstallCommand { get; }

    public ICommand UninstallCommand { get; }

    public ICommand OpenBackupsCommand { get; }

    public ICommand OpenLogCommand { get; }

    public ICommand OpenModFolderCommand { get; }

    public IReadOnlyList<KeyOption> MenuKeys => KeyNames.MenuKeys;

    public IReadOnlyList<KeyOption> NitroKeys => KeyNames.NitroKeys;

    public string GameDirectoryText => _main.GameDirectory ?? "ETS2 nicht gefunden (Steam-Bibliothek prüfen)";

    public PluginInstallState PluginState => PluginInstaller.GetState(_main.GameDirectory);

    public string PluginStateText => PluginState switch
    {
        PluginInstallState.Installed => "Installiert – wird beim Spielstart geladen",
        PluginInstallState.Outdated => "Veraltet – bitte neu installieren",
        PluginInstallState.NotInstalled => "Nicht installiert",
        PluginInstallState.SourceMissing => "Plugin-DLL fehlt im Trainer-Ordner (build.cmd ausführen)",
        _ => "Spiel nicht gefunden",
    };

    public bool PluginOk => PluginState == PluginInstallState.Installed;

    public KeyOption? MenuKey
    {
        get => KeyNames.MenuKeys.FirstOrDefault(k => k.VirtualKey == _main.Settings.MenuHotkeyVk);
        set
        {
            if (value is not null)
            {
                _main.UpdateSettings(s => s with { MenuHotkeyVk = value.VirtualKey });
                _main.ReRegisterHotkey();
                Raise();
            }
        }
    }

    public KeyOption? NitroKey
    {
        get => KeyNames.NitroKeys.FirstOrDefault(k => k.VirtualKey == _main.Settings.NitroVk);
        set
        {
            if (value is not null)
            {
                _main.UpdateSettings(s => s with { NitroVk = value.VirtualKey });
                Raise();
            }
        }
    }

    public bool AlwaysOnTop
    {
        get => _main.Settings.AlwaysOnTop;
        set
        {
            _main.UpdateSettings(s => s with { AlwaysOnTop = value });
            Raise();
        }
    }

    public void Refresh() => RaiseAll();

    private void Install()
    {
        PluginInstaller.Install(_main.GameDirectory!);
        RaiseAll();
        _main.Toast("Plugin installiert. ETS2 starten und den SDK-Hinweis mit OK bestätigen.");
    }

    private void Uninstall()
    {
        PluginInstaller.Uninstall(_main.GameDirectory!);
        RaiseAll();
        _main.Toast("Plugin entfernt.");
    }

    private static void OpenPluginLog()
    {
        var log = Path.Combine(GamePaths.DocumentsDirectory, "ets2_trainer_plugin.log");
        if (File.Exists(log))
        {
            Process.Start(new ProcessStartInfo(log) { UseShellExecute = true });
        }
        else
        {
            OpenFolder(GamePaths.DocumentsDirectory);
        }
    }

    private static void OpenFolder(string path)
    {
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
    }
}
