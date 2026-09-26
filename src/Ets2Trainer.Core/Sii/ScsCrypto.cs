using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;

namespace Ets2Trainer.Core.Sii;

/// <summary>Kind of a SII file, detected from its first four bytes.</summary>
public enum SiiFormat
{
    Unknown,
    Text,         // "SiiN"
    Encrypted,    // "ScsC" (AES-256-CBC + zlib)
    Binary,       // "BSII"
    Scrambled3nK, // "3nK" (only used by some def files; not needed for saves)
}

/// <summary>Handles the "ScsC" container used by SCS for saves and profiles.</summary>
public static class ScsCrypto
{
    public const uint SignatureText = 0x4E696953;      // "SiiN"
    public const uint SignatureEncrypted = 0x43736353; // "ScsC"
    public const uint SignatureBinary = 0x49495342;    // "BSII"
    private const uint Signature3nKMasked = 0x004B6E33; // "3nK" (low three bytes)

    private const int HeaderSize = 4 + 32 + 16 + 4; // signature, HMAC, IV, uncompressed size

    // Publicly documented key used by the SCS games for save/profile containers.
    private static readonly byte[] Key =
    {
        0x2a, 0x5f, 0xcb, 0x17, 0x91, 0xd2, 0x2f, 0xb6, 0x02, 0x45, 0xb3, 0xd8, 0x36, 0x9e, 0xd0, 0xb2,
        0xc2, 0x73, 0x71, 0x56, 0x3f, 0xbf, 0x1f, 0x3c, 0x9e, 0xdf, 0x6b, 0x11, 0x82, 0x5a, 0x5d, 0x0a,
    };

    public static SiiFormat Detect(ReadOnlySpan<byte> data)
    {
        if (data.Length < 4)
        {
            return SiiFormat.Unknown;
        }

        var sig = BinaryPrimitives.ReadUInt32LittleEndian(data);
        return sig switch
        {
            SignatureText => SiiFormat.Text,
            SignatureEncrypted => SiiFormat.Encrypted,
            SignatureBinary => SiiFormat.Binary,
            _ when (sig & 0x00FFFFFF) == Signature3nKMasked => SiiFormat.Scrambled3nK,
            _ => SiiFormat.Unknown,
        };
    }

    /// <summary>Decrypts and inflates a "ScsC" container. Returns the inner payload (text or BSII).</summary>
    public static byte[] Decrypt(ReadOnlySpan<byte> data)
    {
        if (Detect(data) != SiiFormat.Encrypted || data.Length < HeaderSize)
        {
            throw new InvalidDataException("Keine verschlüsselte SII-Datei (ScsC-Signatur fehlt).");
        }

        var iv = data.Slice(36, 16).ToArray();
        var expectedSize = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(52, 4));
        var cipherText = data[HeaderSize..];

        using var aes = Aes.Create();
        aes.Key = Key;
        var compressed = aes.DecryptCbc(cipherText, iv, PaddingMode.PKCS7);

        using var input = new MemoryStream(compressed);
        using var zlib = new ZLibStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream(checked((int)expectedSize));
        zlib.CopyTo(output);

        var result = output.ToArray();
        if (result.Length != expectedSize)
        {
            throw new InvalidDataException(
                $"Entschlüsselte Größe passt nicht ({result.Length} statt {expectedSize} Bytes).");
        }

        return result;
    }
}
