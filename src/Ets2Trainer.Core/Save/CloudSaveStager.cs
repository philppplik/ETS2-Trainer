using System.Text;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Core.Save;

/// <summary>
/// Prepares a save for Steam-Cloud profiles. ETS2 reads those through Steam Remote Storage, so the
/// files are staged locally and the in-game plugin writes them through the game's own Steam API
/// (bridge command WriteSaveFiles). Layout expected by the plugin (src/plugin/cloud_storage.cpp):
/// %LOCALAPPDATA%\ETS2Trainer\stage\&lt;slot&gt;\{game,info}.sii plus cloud_request.txt with
/// "profiles/&lt;hex&gt;/save/&lt;slot&gt;/&lt;file&gt;&lt;TAB&gt;&lt;local path&gt;" lines.
/// </summary>
public static class CloudSaveStager
{
    private static readonly UTF8Encoding Utf8NoBom = new(false);

    public static string Root => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ETS2Trainer");

    public static string StageRoot => Path.Combine(Root, "stage");

    public static string RequestFile => Path.Combine(Root, "cloud_request.txt");

    /// <summary>"profiles/&lt;hex&gt;/save/&lt;slot&gt;/&lt;file&gt;" for a Steam-Cloud profile folder.</summary>
    public static string RemoteName(ProfileInfo profile, string slotFolder, string fileName) =>
        $"profiles/{Path.GetFileName(profile.Directory)}/save/{slotFolder}/{fileName}";

    /// <summary>Writes the staged files and the request; returns the remote names.</summary>
    public static IReadOnlyList<string> Stage(LoadedSave save, ProfileInfo profile, string slotFolder)
    {
        if (!profile.IsSteamCloud)
        {
            throw new InvalidOperationException("Nur Steam-Cloud-Profile werden über das Plugin gespeichert.");
        }

        if (slotFolder.Length == 0 || slotFolder.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || slotFolder.Contains(".."))
        {
            throw new ArgumentException("Ungültiger Spielstand-Ordner.", nameof(slotFolder));
        }

        var stageDir = Path.Combine(StageRoot, slotFolder);
        Directory.CreateDirectory(stageDir);
        var files = new (string Name, SiiDocument Document)[] { ("game.sii", save.Game.Document), ("info.sii", save.Info) };
        var request = new StringBuilder();
        var remotes = new List<string>();
        foreach (var (name, document) in files)
        {
            var local = Path.Combine(stageDir, name);
            File.WriteAllBytes(local, SiiFile.ToBytes(document));
            var remote = RemoteName(profile, slotFolder, name);
            request.Append(remote).Append('\t').Append(local).Append('\n');
            remotes.Add(remote);
        }

        File.WriteAllText(RequestFile, request.ToString(), Utf8NoBom);
        return remotes;
    }

    /// <summary>Removes staged copies after a successful write (they are only needed once).</summary>
    public static void CleanUp()
    {
        try
        {
            if (Directory.Exists(StageRoot))
            {
                Directory.Delete(StageRoot, recursive: true);
            }

            File.Delete(RequestFile);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Stale staging files are harmless; the next save overwrites them.
        }
    }
}
