using System.IO;
using System.Text.Json;

namespace Ets2Trainer.App.Infrastructure;

/// <summary>User preferences, stored as JSON in %AppData%\ETS2Trainer\settings.json.</summary>
public sealed record AppSettings
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public uint MenuHotkeyVk { get; init; } = 0x77; // F8

    public uint NitroVk { get; init; } = 0x10; // Shift

    public float PowerFactor { get; init; } = 2f;

    public float NitroAccel { get; init; } = 6f;

    public float SpeedCapKmh { get; init; } = 130f;

    public bool AlwaysOnTop { get; init; } = true;

    public uint JumpVk { get; init; } = 0x61;   // Num 1

    public uint RocketVk { get; init; } = 0x62; // Num 2

    public uint RollVk { get; init; } = 0x63;   // Num 3

    public uint HoverVk { get; init; } = 0x60;  // Num 0 (hold)

    public uint UnflipVk { get; init; } = 0x65; // Num 5

    public bool PrepareMotion { get; init; }

    public float MoonGravity { get; init; } = 0.7f;

    public float SpinSpeed { get; init; } = 1f;

    public string? LastProfileDirectory { get; init; }

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ETS2Trainer", "settings.json");

    public static AppSettings Load()
    {
        try
        {
            return File.Exists(FilePath)
                ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath)) ?? new AppSettings()
                : new AppSettings();
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return new AppSettings(); // corrupt or locked settings must never block the trainer
        }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, JsonOptions));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Trace.TraceWarning($"Einstellungen konnten nicht gespeichert werden: {ex.Message}");
        }
    }
}
