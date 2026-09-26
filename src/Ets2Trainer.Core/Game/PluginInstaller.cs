using System.Security.Cryptography;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.Core.Game;

public enum PluginInstallState
{
    GameNotFound,
    SourceMissing,
    NotInstalled,
    Outdated,
    Installed,
}

/// <summary>Copies the trainer plugin DLL into <c>bin/win_x64/plugins</c> (the game loads it on start).</summary>
public static class PluginInstaller
{
    public const string DllName = "ets2_trainer.dll";

    /// <summary>The plugin shipped next to the trainer executable.</summary>
    public static string SourcePath => Path.Combine(AppContext.BaseDirectory, "plugin", DllName);

    public static string? TargetPath(string? gameDirectory) =>
        gameDirectory is null ? null : Path.Combine(GamePaths.PluginDirectory(gameDirectory), DllName);

    public static PluginInstallState GetState(string? gameDirectory)
    {
        var target = TargetPath(gameDirectory);
        if (target is null)
        {
            return PluginInstallState.GameNotFound;
        }

        if (!File.Exists(SourcePath))
        {
            return File.Exists(target) ? PluginInstallState.Installed : PluginInstallState.SourceMissing;
        }

        if (!File.Exists(target))
        {
            return PluginInstallState.NotInstalled;
        }

        return SameContent(SourcePath, target) ? PluginInstallState.Installed : PluginInstallState.Outdated;
    }

    public static void Install(string gameDirectory)
    {
        EnsureGameClosed();
        if (!File.Exists(SourcePath))
        {
            throw new FileNotFoundException("Plugin-DLL fehlt neben dem Trainer.", SourcePath);
        }

        var dir = GamePaths.PluginDirectory(gameDirectory);
        Directory.CreateDirectory(dir);
        File.Copy(SourcePath, Path.Combine(dir, DllName), overwrite: true);
    }

    public static void Uninstall(string gameDirectory)
    {
        EnsureGameClosed();
        var target = TargetPath(gameDirectory)!;
        if (File.Exists(target))
        {
            File.Delete(target);
        }
    }

    private static void EnsureGameClosed()
    {
        if (SaveSlotService.IsGameRunning())
        {
            throw new InvalidOperationException("Bitte ETS2 schließen – die Plugin-DLL ist sonst gesperrt.");
        }
    }

    private static bool SameContent(string a, string b)
    {
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        return SHA256.HashData(fa).AsSpan().SequenceEqual(SHA256.HashData(fb));
    }
}
