using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Ets2Trainer.Core.HashFs;

/// <summary>
/// Read-only access to SCS "HashFS" archives (<c>.scs</c>), versions 1 and 2. Only plain files and
/// directory listings are supported (packed textures are skipped — not needed for def files).
/// </summary>
public sealed class HashFsArchive : IDisposable
{
    private const uint Magic = 0x23534353; // "SCS#"
    private const ulong V2BlockSize = 16;
    private const int V2MetadataBlockSize = 4;
    private const byte ChunkPlain = 128;
    private const byte ChunkDirectory = 129;
    private const byte CompressedFlag = 0x10;
    private const uint V1FlagDirectory = 1;
    private const uint V1FlagCompressed = 2;

    private readonly FileStream _stream;
    private readonly Dictionary<ulong, Entry> _entries = new();
    private readonly ushort _salt;
    private readonly ushort _version;

    private HashFsArchive(string path, FileStream stream, ushort version, ushort salt)
    {
        Path = path;
        _stream = stream;
        _version = version;
        _salt = salt;
    }

    public string Path { get; }

    public int EntryCount => _entries.Count;

    /// <summary>Opens an archive; returns null when the file is not a HashFS archive (e.g. zip).</summary>
    public static HashFsArchive? TryOpen(string path)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        try
        {
            var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
            if (stream.Length < 32 || reader.ReadUInt32() != Magic)
            {
                stream.Dispose();
                return null;
            }

            var version = reader.ReadUInt16();
            var salt = reader.ReadUInt16();
            var hashMethod = Encoding.ASCII.GetString(reader.ReadBytes(4));
            if (hashMethod != "CITY" || version is not (1 or 2))
            {
                stream.Dispose();
                return null;
            }

            var archive = new HashFsArchive(path, stream, version, salt);
            if (version == 2)
            {
                archive.ReadV2Tables(reader);
            }
            else
            {
                archive.ReadV1Table(reader);
            }

            return archive;
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    public ulong HashPath(string path)
    {
        var p = path.TrimStart('/');
        if (p.Length > 1 && p.EndsWith('/'))
        {
            p = p.TrimEnd('/');
        }

        if (_salt != 0)
        {
            p = _salt.ToString(System.Globalization.CultureInfo.InvariantCulture) + p;
        }

        return CityHash.Hash64(Encoding.UTF8.GetBytes(p));
    }

    public bool FileExists(string path) => _entries.TryGetValue(HashPath(path), out var e) && !e.IsDirectory;

    public bool DirectoryExists(string path) => _entries.TryGetValue(HashPath(path), out var e) && e.IsDirectory;

    public byte[]? ReadFile(string path)
    {
        if (!_entries.TryGetValue(HashPath(path), out var entry) || entry.IsDirectory)
        {
            return null;
        }

        return ReadEntry(entry);
    }

    /// <summary>Lists a directory: (subdirectories, files) as names relative to <paramref name="path"/>.</summary>
    public (List<string> Directories, List<string> Files)? ListDirectory(string path)
    {
        if (!_entries.TryGetValue(HashPath(path), out var entry) || !entry.IsDirectory)
        {
            return null;
        }

        var content = ReadEntry(entry);
        return _version == 2 ? ParseV2Listing(content) : ParseV1Listing(content);
    }

    public void Dispose() => _stream.Dispose();

    private void ReadV2Tables(BinaryReader r)
    {
        var numEntries = r.ReadUInt32();
        var entryTableLength = r.ReadUInt32();
        r.ReadUInt32(); // metadata entry count
        var metadataTableLength = r.ReadUInt32();
        var entryTableStart = r.ReadUInt64();
        var metadataTableStart = r.ReadUInt64();

        var entryTable = Inflate(ReadAt((long)entryTableStart, (int)entryTableLength));
        var metadata = Inflate(ReadAt((long)metadataTableStart, (int)metadataTableLength));

        for (var i = 0; i < numEntries; i++)
        {
            var e = entryTable.AsSpan(i * 16, 16);
            var hash = BinaryPrimitives.ReadUInt64LittleEndian(e);
            var metaIndex = BinaryPrimitives.ReadUInt32LittleEndian(e[8..]);
            var metaCount = BinaryPrimitives.ReadUInt16LittleEndian(e[12..]);
            if (TryReadV2Metadata(metadata, (int)metaIndex * V2MetadataBlockSize, metaCount, out var entry))
            {
                _entries[hash] = entry;
            }
        }
    }

    private static bool TryReadV2Metadata(byte[] metadata, int offset, int chunkCount, out Entry entry)
    {
        entry = default;
        if (chunkCount == 0 || offset + chunkCount * 4 + 16 > metadata.Length)
        {
            return false;
        }

        var chunkType = metadata[offset + 3]; // 3-byte index + 1-byte type; the first chunk decides
        if (chunkType is not (ChunkPlain or ChunkDirectory))
        {
            return false; // packed textures etc.
        }

        var m = metadata.AsSpan(offset + chunkCount * 4, 16);
        var compressedSize = m[0] | (m[1] << 8) | (m[2] << 16) | ((m[3] & 0x0F) << 24);
        var isCompressed = (m[3] & CompressedFlag) != 0;
        var size = m[4] | (m[5] << 8) | (m[6] << 16) | ((m[7] & 0x0F) << 24);
        var offsetBlock = BinaryPrimitives.ReadUInt32LittleEndian(m[12..]);
        entry = new Entry((long)(offsetBlock * V2BlockSize), compressedSize, size, isCompressed,
            chunkType == ChunkDirectory);
        return true;
    }

    private void ReadV1Table(BinaryReader r)
    {
        var numEntries = r.ReadUInt32();
        var start = (long)r.ReadUInt64();
        var table = ReadAt(start, checked((int)numEntries * 32));
        for (var i = 0; i < numEntries; i++)
        {
            var e = table.AsSpan(i * 32, 32);
            var hash = BinaryPrimitives.ReadUInt64LittleEndian(e);
            var offset = (long)BinaryPrimitives.ReadUInt64LittleEndian(e[8..]);
            var flags = BinaryPrimitives.ReadUInt32LittleEndian(e[16..]);
            var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(e[24..]);
            var compressedSize = (int)BinaryPrimitives.ReadUInt32LittleEndian(e[28..]);
            _entries[hash] = new Entry(offset, compressedSize, size, (flags & V1FlagCompressed) != 0,
                (flags & V1FlagDirectory) != 0);
        }
    }

    private byte[] ReadEntry(Entry entry) =>
        entry.IsCompressed ? Inflate(ReadAt(entry.Offset, entry.CompressedSize)) : ReadAt(entry.Offset, entry.Size);

    private byte[] ReadAt(long offset, int length)
    {
        var buffer = new byte[length];
        lock (_stream)
        {
            _stream.Position = offset;
            _stream.ReadExactly(buffer);
        }

        return buffer;
    }

    private static byte[] Inflate(byte[] data)
    {
        using var input = new MemoryStream(data);
        using var zlib = new ZLibStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream(data.Length * 4);
        zlib.CopyTo(output);
        return output.ToArray();
    }

    private static (List<string>, List<string>) ParseV2Listing(byte[] content)
    {
        var dirs = new List<string>();
        var files = new List<string>();
        var count = (int)BinaryPrimitives.ReadUInt32LittleEndian(content);
        var pos = 4 + count;
        for (var i = 0; i < count; i++)
        {
            var len = content[4 + i];
            var name = Encoding.UTF8.GetString(content, pos, len);
            pos += len;
            if (name.StartsWith('/'))
            {
                dirs.Add(name[1..]);
            }
            else
            {
                files.Add(name);
            }
        }

        return (dirs, files);
    }

    private static (List<string>, List<string>) ParseV1Listing(byte[] content)
    {
        var dirs = new List<string>();
        var files = new List<string>();
        foreach (var line in Encoding.UTF8.GetString(content).Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            if (line.StartsWith('*'))
            {
                dirs.Add(line[1..]);
            }
            else
            {
                files.Add(line);
            }
        }

        return (dirs, files);
    }

    private readonly record struct Entry(long Offset, int CompressedSize, int Size, bool IsCompressed, bool IsDirectory);
}
