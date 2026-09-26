using System.Text;

namespace Ets2Trainer.Core.HashFs;

/// <summary>
/// Virtual file system over all HashFS archives of the game installation. DLC archives are searched
/// before the base archives, mirroring how the game layers them.
/// </summary>
public sealed class GameFileSystem : IDisposable
{
    private readonly List<HashFsArchive> _archives;

    private GameFileSystem(List<HashFsArchive> archives) => _archives = archives;

    public IReadOnlyList<HashFsArchive> Archives => _archives;

    public static GameFileSystem Mount(string gameDirectory)
    {
        var files = Directory.GetFiles(gameDirectory, "*.scs", SearchOption.TopDirectoryOnly);
        var ordered = files
            .OrderByDescending(f => System.IO.Path.GetFileName(f).StartsWith("dlc_", StringComparison.OrdinalIgnoreCase))
            .ThenBy(f => f, StringComparer.OrdinalIgnoreCase);

        var archives = new List<HashFsArchive>();
        foreach (var file in ordered)
        {
            try
            {
                if (HashFsArchive.TryOpen(file) is { } archive)
                {
                    archives.Add(archive);
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
            {
                // A single unreadable archive must not break the trainer; skip it.
            }
        }

        return new GameFileSystem(archives);
    }

    public byte[]? ReadFile(string path)
    {
        foreach (var archive in _archives)
        {
            if (archive.ReadFile(path) is { } data)
            {
                return data;
            }
        }

        return null;
    }

    public string? ReadText(string path) => ReadFile(path) is { } data ? Encoding.UTF8.GetString(data) : null;

    /// <summary>Union of a directory's files across all archives (names only).</summary>
    public IReadOnlyList<string> ListFiles(string directory)
    {
        var files = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var archive in _archives)
        {
            if (archive.ListDirectory(directory) is { } listing)
            {
                files.UnionWith(listing.Files);
            }
        }

        return files.ToList();
    }

    /// <summary>Union of a directory's subdirectories across all archives (names only).</summary>
    public IReadOnlyList<string> ListDirectories(string directory)
    {
        var dirs = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var archive in _archives)
        {
            if (archive.ListDirectory(directory) is { } listing)
            {
                dirs.UnionWith(listing.Directories);
            }
        }

        return dirs.ToList();
    }

    /// <summary>Builds an <c>@include</c> resolver relative to <paramref name="baseDirectory"/>.</summary>
    public Func<string, string?> IncludeResolver(string baseDirectory) =>
        include => ReadText(include.StartsWith('/') ? include : $"{baseDirectory.TrimEnd('/')}/{include}");

    public void Dispose()
    {
        foreach (var archive in _archives)
        {
            archive.Dispose();
        }
    }
}
