using System.Text;

namespace Ets2Trainer.Core.Sii;

/// <summary>Loads SII files in any SCS container format and writes them back as plain text.</summary>
public static class SiiFile
{
    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false);

    public static SiiDocument Load(byte[] data)
    {
        return ScsCrypto.Detect(data) switch
        {
            SiiFormat.Encrypted => Load(ScsCrypto.Decrypt(data)),
            SiiFormat.Binary => BsiiDecoder.Decode(data),
            SiiFormat.Text => SiiTextParser.Parse(Utf8NoBom.GetString(data)),
            SiiFormat.Scrambled3nK => throw new NotSupportedException("3nK-kodierte SII-Dateien werden nicht unterstützt."),
            _ => throw new InvalidDataException("Unbekanntes SII-Format."),
        };
    }

    public static SiiDocument LoadFile(string path) => Load(File.ReadAllBytes(path));

    public static byte[] ToBytes(SiiDocument document, SiiArrayStyle style = SiiArrayStyle.Indexed) =>
        Utf8NoBom.GetBytes(SiiTextWriter.Write(document, style));

    /// <summary>Writes the document as plain SII text, atomically (temp file + replace).</summary>
    public static void SaveFile(string path, SiiDocument document)
    {
        var directory = Path.GetDirectoryName(path) ?? throw new ArgumentException("Ungültiger Pfad.", nameof(path));
        Directory.CreateDirectory(directory);
        var temp = Path.Combine(directory, $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        File.WriteAllBytes(temp, ToBytes(document));
        File.Move(temp, path, overwrite: true);
    }
}
