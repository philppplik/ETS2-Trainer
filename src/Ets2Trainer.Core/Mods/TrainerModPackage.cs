using System.Globalization;
using System.IO.Compression;
using System.Text;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Save;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Core.Mods;

/// <summary>
/// The trainer's own mod (<c>mod/ets2_trainer_engines.scs</c>, a zip): holds all generated engines.
/// Existing engines are kept when new ones are added.
/// </summary>
public static class TrainerModPackage
{
    public const string PackageName = "ets2_trainer_engines";
    public const string DisplayName = "ETS2 Trainer - Motoren";
    private const string ManifestEntry = "manifest.sii";
    private const string DescriptionEntry = "description.txt";

    private static readonly UTF8Encoding Utf8NoBom = new(false);

    public static string PackagePath => Path.Combine(GamePaths.ModDirectory, PackageName + ".scs");

    public static string ActiveModEntry => $"{PackageName}|{DisplayName}";

    /// <summary>Adds or replaces an engine in the package (atomic rewrite).</summary>
    public static void AddOrReplace(GeneratedEngine engine)
    {
        var entries = ReadExistingDefs();
        entries[engine.DataPath.TrimStart('/')] = engine.DefinitionText;
        WritePackage(entries);
    }

    /// <summary>Generated engine def paths currently in the package.</summary>
    public static IReadOnlyList<string> ListEngines() =>
        ReadExistingDefs().Keys.Select(k => "/" + k).OrderBy(k => k, StringComparer.Ordinal).ToList();

    /// <summary>Adds the trainer mod to the profile's active mod list (game must be closed). Returns true when changed.</summary>
    public static bool ActivateInProfile(ProfileInfo profile)
    {
        if (SaveSlotService.IsGameRunning())
        {
            throw new InvalidOperationException("Bitte ETS2 schließen – das Spiel überschreibt sonst die Mod-Liste.");
        }

        var document = SiiFile.LoadFile(profile.ProfileFile);
        var user = document.FirstOfClass("user_profile") ?? throw new InvalidDataException("profile.sii ohne user_profile.");
        var mods = user.GetArray("active_mods").ToList();
        if (mods.Any(IsTrainerEntry))
        {
            return false;
        }

        var backupDir = Path.Combine(SaveSlotService.BackupRoot, Path.GetFileName(profile.Directory));
        Directory.CreateDirectory(backupDir);
        var stamp = DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
        File.Copy(profile.ProfileFile, Path.Combine(backupDir, $"profile_{stamp}.sii"), overwrite: true);

        mods.Insert(0, SiiValues.Quote(ActiveModEntry));
        user.SetArray("active_mods", mods);
        SiiFile.SaveFile(profile.ProfileFile, document);
        return true;
    }

    public static bool IsActiveInProfile(ProfileInfo profile)
    {
        try
        {
            var user = SiiFile.LoadFile(profile.ProfileFile).FirstOfClass("user_profile");
            return user?.GetArray("active_mods").Any(IsTrainerEntry) == true;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or NotSupportedException)
        {
            return false;
        }
    }

    private static bool IsTrainerEntry(string entry) =>
        SiiValues.Unquote(entry).StartsWith(PackageName + "|", StringComparison.Ordinal);

    private static Dictionary<string, string> ReadExistingDefs()
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!File.Exists(PackagePath))
        {
            return result;
        }

        using var zip = ZipFile.OpenRead(PackagePath);
        foreach (var entry in zip.Entries.Where(e => e.FullName.StartsWith("def/", StringComparison.Ordinal)))
        {
            using var reader = new StreamReader(entry.Open(), Utf8NoBom);
            result[entry.FullName] = reader.ReadToEnd();
        }

        return result;
    }

    private static void WritePackage(Dictionary<string, string> defs)
    {
        Directory.CreateDirectory(GamePaths.ModDirectory);
        var temp = PackagePath + ".tmp";
        using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write))
        using (var zip = new ZipArchive(stream, ZipArchiveMode.Create))
        {
            AddText(zip, ManifestEntry, Manifest());
            AddText(zip, DescriptionEntry, Description(defs.Keys));
            foreach (var (path, text) in defs.OrderBy(d => d.Key, StringComparer.Ordinal))
            {
                AddText(zip, path, text);
            }
        }

        File.Move(temp, PackagePath, overwrite: true);
    }

    private static void AddText(ZipArchive zip, string path, string text)
    {
        var entry = zip.CreateEntry(path, CompressionLevel.Optimal);
        using var writer = new StreamWriter(entry.Open(), Utf8NoBom);
        writer.Write(text.Replace("\r\n", "\n"));
    }

    private static string Manifest() => $$"""
        SiiNunit
        {
        mod_package : .package_name
        {
        	package_version: "1.0"
        	display_name: "{{DisplayName}}"
        	author: "ETS2 Trainer"
        	category[]: "truck"
        	description_file: "{{DescriptionEntry}}"
        }
        }

        """;

    private static string Description(IEnumerable<string> defs) =>
        "Vom ETS2 Trainer erzeugte Motoren (nur Singleplayer / eigene Konvois).\n\n" +
        string.Join("\n", defs.Select(d => "- " + d)) + "\n";
}
