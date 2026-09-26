using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Ets2Trainer.Core.Game;

/// <summary>A player profile (local or Steam Cloud) with its folder.</summary>
public sealed record ProfileInfo(string Directory, string DisplayName, bool IsSteamCloud)
{
    public string SaveDirectory => Path.Combine(Directory, "save");

    public string ProfileFile => Path.Combine(Directory, "profile.sii");

    public override string ToString() => IsSteamCloud ? $"{DisplayName}  (Steam Cloud)" : DisplayName;
}

/// <summary>Locates the ETS2 installation, the documents folder and all profiles.</summary>
public static partial class GamePaths
{
    public const string SteamAppId = "227300";
    private const string GameFolderName = "Euro Truck Simulator 2";
    private const string GameExe = @"bin\win_x64\eurotrucks2.exe";

    public static string DocumentsDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), GameFolderName);

    public static string ModDirectory => Path.Combine(DocumentsDirectory, "mod");

    public static string ConfigFile => Path.Combine(DocumentsDirectory, "config.cfg");

    public static string? SteamDirectory =>
        (Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string)
            ?.Replace('/', Path.DirectorySeparatorChar);

    /// <summary>Finds the game directory via Steam library folders; null when not installed.</summary>
    public static string? FindGameDirectory()
    {
        foreach (var library in SteamLibraries())
        {
            var candidate = Path.Combine(library, "steamapps", "common", GameFolderName);
            if (File.Exists(Path.Combine(candidate, GameExe)))
            {
                return candidate;
            }
        }

        return null;
    }

    public static string PluginDirectory(string gameDirectory) => Path.Combine(gameDirectory, "bin", "win_x64", "plugins");

    /// <summary>All profiles: local ones and Steam Cloud ones (saves live in Steam's userdata folder).</summary>
    public static IReadOnlyList<ProfileInfo> FindProfiles()
    {
        var result = new List<ProfileInfo>();
        AddProfiles(result, Path.Combine(DocumentsDirectory, "profiles"), isCloud: false);
        AddProfiles(result, Path.Combine(DocumentsDirectory, "steam_profiles"), isCloud: false);

        if (SteamDirectory is { } steam && Directory.Exists(Path.Combine(steam, "userdata")))
        {
            foreach (var user in Directory.GetDirectories(Path.Combine(steam, "userdata")))
            {
                AddProfiles(result, Path.Combine(user, SteamAppId, "remote", "profiles"), isCloud: true);
            }
        }

        return result
            .Where(p => File.Exists(p.ProfileFile) || Directory.Exists(p.SaveDirectory))
            .GroupBy(p => p.Directory, StringComparer.OrdinalIgnoreCase)
            .Select(g => g.First())
            .ToList();
    }

    /// <summary>Profile folder names are the hex-encoded UTF-8 profile name.</summary>
    public static string DecodeProfileName(string folderName)
    {
        if (folderName.Length % 2 != 0 || !HexRegex().IsMatch(folderName))
        {
            return folderName;
        }

        try
        {
            return Encoding.UTF8.GetString(Convert.FromHexString(folderName));
        }
        catch (FormatException)
        {
            return folderName;
        }
    }

    private static void AddProfiles(List<ProfileInfo> result, string root, bool isCloud)
    {
        if (!Directory.Exists(root))
        {
            return;
        }

        foreach (var dir in Directory.GetDirectories(root))
        {
            var name = DecodeProfileName(Path.GetFileName(dir));
            result.Add(new ProfileInfo(dir, name, isCloud));
        }
    }

    private static IEnumerable<string> SteamLibraries()
    {
        if (SteamDirectory is not { } steam)
        {
            yield break;
        }

        yield return steam;
        var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(vdf))
        {
            yield break;
        }

        foreach (Match m in LibraryPathRegex().Matches(File.ReadAllText(vdf)))
        {
            yield return m.Groups[1].Value.Replace(@"\\", @"\");
        }
    }

    [GeneratedRegex("^[0-9A-Fa-f]+$")]
    private static partial Regex HexRegex();

    [GeneratedRegex("\"path\"\\s+\"([^\"]+)\"")]
    private static partial Regex LibraryPathRegex();
}
