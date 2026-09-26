using System.Diagnostics;
using System.Globalization;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Core.Save;

/// <summary>A save folder inside a profile.</summary>
public sealed record SaveSlot(string Directory, string Name, DateTime SavedAt, long Money, long Experience)
{
    public string FolderName => Path.GetFileName(Directory);

    public string GameFile => Path.Combine(Directory, "game.sii");

    public string InfoFile => Path.Combine(Directory, "info.sii");

    public override string ToString() => $"{Name}  ·  {SavedAt:g}";
}

public enum SaveWriteMode
{
    /// <summary>Write into a new numbered save folder — the original stays untouched (recommended).</summary>
    NewSlot,

    /// <summary>Overwrite the loaded save after backing it up.</summary>
    OverwriteWithBackup,
}

/// <summary>A loaded save: the game document plus its info.sii.</summary>
public sealed class LoadedSave
{
    public LoadedSave(SaveSlot slot, SaveGame game, SiiDocument info)
    {
        Slot = slot;
        Game = game;
        Info = info;
    }

    public SaveSlot Slot { get; }

    public SaveGame Game { get; }

    public SiiDocument Info { get; }
}

/// <summary>Lists, loads and writes save slots. All writes are plain-text SII and never touch the original in NewSlot mode.</summary>
public static class SaveSlotService
{
    public const string TrainerSaveMarker = "[Trainer]";
    private const string GameProcessName = "eurotrucks2";

    public static string BackupRoot => Path.Combine(GamePaths.DocumentsDirectory, "ets2_trainer_backups");

    public static bool IsGameRunning() => Process.GetProcessesByName(GameProcessName).Length > 0;

    public static IReadOnlyList<SaveSlot> ListSlots(ProfileInfo profile)
    {
        if (!Directory.Exists(profile.SaveDirectory))
        {
            return Array.Empty<SaveSlot>();
        }

        var slots = new List<SaveSlot>();
        foreach (var dir in Directory.GetDirectories(profile.SaveDirectory))
        {
            if (TryReadSlot(dir) is { } slot)
            {
                slots.Add(slot);
            }
        }

        return slots.OrderByDescending(s => s.SavedAt).ToList();
    }

    public static LoadedSave Load(SaveSlot slot)
    {
        var game = new SaveGame(SiiFile.LoadFile(slot.GameFile));
        var info = File.Exists(slot.InfoFile) ? SiiFile.LoadFile(slot.InfoFile) : new SiiDocument(Array.Empty<SiiUnit>());
        return new LoadedSave(slot, game, info);
    }

    /// <summary>Writes the edited save. Returns the folder that was written.</summary>
    public static string Write(LoadedSave save, SaveWriteMode mode)
    {
        UpdateInfo(save);
        var source = save.Slot.Directory;
        string target;
        if (mode == SaveWriteMode.NewSlot)
        {
            target = NextFreeSlot(Path.GetDirectoryName(source)!);
            CopyExtraFiles(source, target);
            RenameInfo(save.Info, save.Slot.Name);
        }
        else
        {
            BackupFolder(source);
            target = source;
        }

        SiiFile.SaveFile(Path.Combine(target, "game.sii"), save.Game.Document);
        SiiFile.SaveFile(Path.Combine(target, "info.sii"), save.Info);
        return target;
    }

    /// <summary>Adds a mod dependency to info.sii so the game warns if the mod is disabled.</summary>
    public static void AddModDependency(SiiDocument info, string packageName, string displayName)
    {
        if (info.FirstOfClass("save_container") is not { } container)
        {
            return;
        }

        var deps = container.GetArray("dependencies").ToList();
        if (!deps.Any(d => SiiValues.Unquote(d).StartsWith($"mod|{packageName}|", StringComparison.Ordinal)))
        {
            deps.Insert(0, SiiValues.Quote($"mod|{packageName}|{displayName}"));
            container.SetArray("dependencies", deps);
        }
    }

    public static string BackupFolder(string saveFolder)
    {
        var profileName = Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(saveFolder))) ?? "profile";
        var stamp = DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
        var target = Path.Combine(BackupRoot, profileName, $"{Path.GetFileName(saveFolder)}_{stamp}");
        Directory.CreateDirectory(target);
        foreach (var file in Directory.GetFiles(saveFolder))
        {
            File.Copy(file, Path.Combine(target, Path.GetFileName(file)), overwrite: true);
        }

        return target;
    }

    private static SaveSlot? TryReadSlot(string dir)
    {
        var gameFile = Path.Combine(dir, "game.sii");
        if (!File.Exists(gameFile))
        {
            return null;
        }

        var folder = Path.GetFileName(dir);
        var name = FriendlyFolderName(folder);
        var savedAt = File.GetLastWriteTime(gameFile);
        long money = 0, xp = 0;
        try
        {
            var info = SiiFile.LoadFile(Path.Combine(dir, "info.sii")).FirstOfClass("save_container");
            if (info is not null)
            {
                var infoName = SiiValues.Unquote(info.Get("name"));
                name = infoName.Length > 0 ? infoName : name;
                money = SiiValues.ParseLong(info.Get("info_money_account"));
                xp = SiiValues.ParseLong(info.Get("info_players_experience"));
                var fileTime = SiiValues.ParseLong(info.Get("file_time"));
                if (fileTime > 0)
                {
                    savedAt = DateTimeOffset.FromUnixTimeSeconds(fileTime).LocalDateTime;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or NotSupportedException)
        {
            // Unreadable info.sii: keep folder-based name and file date.
        }

        return new SaveSlot(dir, name, savedAt, money, xp);
    }

    private static string FriendlyFolderName(string folder) => folder switch
    {
        "autosave" => "Autosave",
        "quicksave" => "Schnellspeicherung",
        "autosave_drive" or "autosave_drive_1" => "Autosave (Fahrt)",
        "autosave_job" => "Autosave (Auftrag)",
        _ => $"Spielstand {folder}",
    };

    private static void UpdateInfo(LoadedSave save)
    {
        if (save.Info.FirstOfClass("save_container") is not { } container)
        {
            return;
        }

        container.Set("info_money_account", SiiValues.FormatLong(save.Game.Money));
        container.Set("info_players_experience", SiiValues.FormatLong(save.Game.ExperiencePoints));
        container.Set("info_visited_cities", SiiValues.FormatLong(save.Game.VisitedCityCount));
        container.Set("file_time", SiiValues.FormatLong(DateTimeOffset.UtcNow.ToUnixTimeSeconds()));
    }

    private static void RenameInfo(SiiDocument info, string originalName)
    {
        if (info.FirstOfClass("save_container") is { } container)
        {
            var newName = originalName.StartsWith(TrainerSaveMarker, StringComparison.Ordinal)
                ? originalName
                : $"{TrainerSaveMarker} {originalName}";
            container.Set("name", SiiValues.Quote(newName));
        }
    }

    private static string NextFreeSlot(string saveRoot)
    {
        var used = Directory.GetDirectories(saveRoot)
            .Select(Path.GetFileName)
            .Select(n => int.TryParse(n, NumberStyles.None, CultureInfo.InvariantCulture, out var i) ? i : 0)
            .DefaultIfEmpty(0)
            .Max();
        var target = Path.Combine(saveRoot, (used + 1).ToString(CultureInfo.InvariantCulture));
        Directory.CreateDirectory(target);
        return target;
    }

    private static void CopyExtraFiles(string source, string target)
    {
        foreach (var file in Directory.GetFiles(source))
        {
            var name = Path.GetFileName(file);
            if (name is not ("game.sii" or "info.sii"))
            {
                File.Copy(file, Path.Combine(target, name), overwrite: true);
            }
        }
    }
}
