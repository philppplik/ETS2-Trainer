using System.Buffers.Binary;
using System.Text;

namespace Ets2Trainer.Core.Sii;

/// <summary>
/// Decodes the binary SII format ("BSII", versions 1-3) used by SCS saves into a <see cref="SiiDocument"/>
/// whose values are already in SII text notation.
/// </summary>
/// <remarks>Format knowledge based on the MIT-licensed Trucky/sii-decrypt-ts and TheLazyTomcat's SII_Decrypt.</remarks>
public static class BsiiDecoder
{
    private const uint MaxSupportedVersion = 3;

    public static SiiDocument Decode(byte[] data)
    {
        var reader = new BsiiReader(data);
        if (reader.U32() != ScsCrypto.SignatureBinary)
        {
            throw new InvalidDataException("Keine BSII-Datei.");
        }

        var version = reader.U32();
        if (version is 0 or > MaxSupportedVersion)
        {
            throw new NotSupportedException($"BSII-Version {version} wird nicht unterstützt.");
        }

        var structs = new Dictionary<uint, StructDef>();
        var units = new List<SiiUnit>();

        while (!reader.AtEnd)
        {
            var blockType = reader.U32();
            if (blockType == 0)
            {
                ReadStructDefinition(reader, structs);
                continue;
            }

            if (!structs.TryGetValue(blockType, out var def))
            {
                throw new InvalidDataException($"Unbekannte Struktur-ID {blockType} an Position {reader.Position}.");
            }

            units.Add(ReadUnit(reader, def, version));
        }

        return new SiiDocument(units);
    }

    private static void ReadStructDefinition(BsiiReader reader, Dictionary<uint, StructDef> structs)
    {
        var valid = reader.U8() != 0;
        if (!valid)
        {
            return;
        }

        var id = reader.U32();
        var name = reader.Str();
        var fields = new List<FieldDef>();
        while (true)
        {
            var type = reader.U32();
            if (type == 0)
            {
                break;
            }

            var fieldName = reader.Str();
            Dictionary<uint, string>? ordinals = null;
            if (type == BsiiType.OrdinalString)
            {
                var count = reader.U32();
                ordinals = new Dictionary<uint, string>((int)count);
                for (var i = 0; i < count; i++)
                {
                    var ordinal = reader.U32();
                    ordinals[ordinal] = reader.Str();
                }
            }

            fields.Add(new FieldDef(type, fieldName, ordinals));
        }

        structs.TryAdd(id, new StructDef(name, fields));
    }

    private static SiiUnit ReadUnit(BsiiReader reader, StructDef def, uint version)
    {
        var unit = new SiiUnit(def.Name, reader.Id());
        foreach (var field in def.Fields)
        {
            unit.Add(ReadField(reader, field, version));
        }

        return unit;
    }

    private static SiiAttribute ReadField(BsiiReader r, FieldDef f, uint version)
    {
        var n = f.Name;
        return f.Type switch
        {
            BsiiType.String => new(n, SiiValues.Quote(r.Str())),
            BsiiType.StringArray => new(n, r.Array(() => SiiValues.Quote(r.Str()))),
            BsiiType.Token => new(n, FormatToken(r.Token())),
            BsiiType.TokenArray => new(n, r.Array(() => FormatToken(r.Token()))),
            BsiiType.Float => new(n, SiiValues.FormatFloat(r.F32())),
            BsiiType.FloatArray => new(n, r.Array(() => SiiValues.FormatFloat(r.F32()))),
            BsiiType.Vec2 => new(n, Vec2(r)),
            BsiiType.Vec2Array => new(n, r.Array(() => Vec2(r))),
            BsiiType.Vec3 => new(n, Vec3(r)),
            BsiiType.Vec3Array => new(n, r.Array(() => Vec3(r))),
            BsiiType.Vec3I => new(n, Vec3I(r)),
            BsiiType.Vec3IArray => new(n, r.Array(() => Vec3I(r))),
            BsiiType.Vec4 => new(n, Quat(r)),
            BsiiType.Vec4Array => new(n, r.Array(() => Quat(r))),
            BsiiType.Placement => new(n, Placement(r, version)),
            BsiiType.PlacementArray => new(n, r.Array(() => Placement(r, version))),
            BsiiType.Int32 => new(n, r.I32().ToString(SiiValues.Invariant)),
            BsiiType.Int32Array => new(n, r.Array(() => r.I32().ToString(SiiValues.Invariant))),
            BsiiType.UInt32 or BsiiType.UInt32B => new(n, FormatU32(r.U32())),
            BsiiType.UInt32Array => new(n, r.Array(() => FormatU32(r.U32()))),
            BsiiType.Int16 => new(n, FormatI16(r.I16())),
            BsiiType.Int16Array => new(n, r.Array(() => FormatI16(r.I16()))),
            BsiiType.UInt16 => new(n, FormatU16(r.U16())),
            BsiiType.UInt16Array => new(n, r.Array(() => FormatU16(r.U16()))),
            BsiiType.Int64 => new(n, r.I64().ToString(SiiValues.Invariant)),
            BsiiType.Int64Array => new(n, r.Array(() => r.I64().ToString(SiiValues.Invariant))),
            BsiiType.UInt64 => new(n, r.U64().ToString(SiiValues.Invariant)),
            BsiiType.UInt64Array => new(n, r.Array(() => r.U64().ToString(SiiValues.Invariant))),
            BsiiType.Bool => new(n, r.U8() != 0 ? "true" : "false"),
            BsiiType.BoolArray => new(n, r.Array(() => r.U8() != 0 ? "true" : "false")),
            BsiiType.OrdinalString => new(n, Ordinal(r, f)),
            BsiiType.Id or BsiiType.IdB or BsiiType.IdC => new(n, r.Id()),
            BsiiType.IdArray or BsiiType.IdArrayB or BsiiType.IdArrayC => new(n, r.Array(r.Id)),
            _ => throw new NotSupportedException(
                $"Unbekannter BSII-Datentyp 0x{f.Type:x2} (Feld '{n}') an Position {r.Position}."),
        };
    }

    private static string FormatToken(string token) => token.Length == 0 ? "\"\"" : token;

    private static string FormatU32(uint v) => v == uint.MaxValue ? "nil" : v.ToString(SiiValues.Invariant);

    private static string FormatU16(ushort v) => v == ushort.MaxValue ? "nil" : v.ToString(SiiValues.Invariant);

    private static string FormatI16(short v) => v == short.MaxValue ? "nil" : v.ToString(SiiValues.Invariant);

    private static string Ordinal(BsiiReader r, FieldDef f)
    {
        var index = r.U32();
        return f.Ordinals is not null && f.Ordinals.TryGetValue(index, out var s) ? s : "\"\"";
    }

    private static string Vec2(BsiiReader r) =>
        $"({SiiValues.FormatFloat(r.F32())}, {SiiValues.FormatFloat(r.F32())})";

    private static string Vec3(BsiiReader r) =>
        $"({SiiValues.FormatFloat(r.F32())}, {SiiValues.FormatFloat(r.F32())}, {SiiValues.FormatFloat(r.F32())})";

    private static string Vec3I(BsiiReader r) =>
        string.Create(SiiValues.Invariant, $"({r.I32()}, {r.I32()}, {r.I32()})");

    private static string Quat(BsiiReader r) =>
        $"({SiiValues.FormatFloat(r.F32())}; {SiiValues.FormatFloat(r.F32())}, " +
        $"{SiiValues.FormatFloat(r.F32())}, {SiiValues.FormatFloat(r.F32())})";

    private static string Placement(BsiiReader r, uint version)
    {
        float x = r.F32(), y = r.F32(), z = r.F32();
        if (version == 1)
        {
            // v1: position + quaternion, 7 floats, no bias.
            return $"({F(x)}, {F(y)}, {F(z)}) ({F(r.F32())}; {F(r.F32())}, {F(r.F32())}, {F(r.F32())})";
        }

        // v2+: the 4th float carries two 12-bit biases that extend x and z beyond float precision.
        var bias = (int)r.F32();
        var biasX = ((bias & 0xFFF) - 2048) << 9;
        var biasZ = (((bias >> 12) & 0xFFF) - 2048) << 9;
        var px = (float)(x + (double)biasX);
        var pz = (float)(z + (double)biasZ);
        return $"({F(px)}, {F(y)}, {F(pz)}) ({F(r.F32())}; {F(r.F32())}, {F(r.F32())}, {F(r.F32())})";

        static string F(float v) => SiiValues.FormatFloat(v);
    }

    private sealed record StructDef(string Name, List<FieldDef> Fields);

    private sealed record FieldDef(uint Type, string Name, Dictionary<uint, string>? Ordinals);

    private static class BsiiType
    {
        public const uint String = 0x01, StringArray = 0x02, Token = 0x03, TokenArray = 0x04;
        public const uint Float = 0x05, FloatArray = 0x06, Vec2 = 0x07, Vec2Array = 0x08;
        public const uint Vec3 = 0x09, Vec3Array = 0x0A, Vec3I = 0x11, Vec3IArray = 0x12;
        public const uint Vec4 = 0x17, Vec4Array = 0x18, Placement = 0x19, PlacementArray = 0x1A;
        public const uint Int32 = 0x25, Int32Array = 0x26, UInt32 = 0x27, UInt32Array = 0x28;
        public const uint Int16 = 0x29, Int16Array = 0x2A, UInt16 = 0x2B, UInt16Array = 0x2C;
        public const uint UInt32B = 0x2F, Int64 = 0x31, Int64Array = 0x32, UInt64 = 0x33, UInt64Array = 0x34;
        public const uint Bool = 0x35, BoolArray = 0x36, OrdinalString = 0x37;
        public const uint Id = 0x39, IdArray = 0x3A, IdB = 0x3B, IdArrayB = 0x3C, IdC = 0x3D, IdArrayC = 0x3E;
    }

    /// <summary>Little-endian cursor over the payload.</summary>
    private sealed class BsiiReader
    {
        private const string TokenAlphabet = "0123456789abcdefghijklmnopqrstuvwxyz_";
        private const int TokenBase = 38;
        private const byte NamelessMarker = 0xFF;

        private readonly byte[] _data;

        public BsiiReader(byte[] data) => _data = data;

        public int Position { get; private set; }

        public bool AtEnd => Position >= _data.Length;

        public byte U8() => _data[Take(1)];

        public short I16() => BinaryPrimitives.ReadInt16LittleEndian(_data.AsSpan(Take(2)));

        public ushort U16() => BinaryPrimitives.ReadUInt16LittleEndian(_data.AsSpan(Take(2)));

        public int I32() => BinaryPrimitives.ReadInt32LittleEndian(_data.AsSpan(Take(4)));

        public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(_data.AsSpan(Take(4)));

        public long I64() => BinaryPrimitives.ReadInt64LittleEndian(_data.AsSpan(Take(8)));

        public ulong U64() => BinaryPrimitives.ReadUInt64LittleEndian(_data.AsSpan(Take(8)));

        public float F32() => BinaryPrimitives.ReadSingleLittleEndian(_data.AsSpan(Take(4)));

        public string Str()
        {
            var length = (int)U32();
            var start = Take(length);
            return Encoding.UTF8.GetString(_data, start, length);
        }

        public string Token()
        {
            var value = U64();
            var sb = new StringBuilder(12);
            while (value != 0)
            {
                var index = (int)(value % TokenBase) - 1;
                value /= TokenBase;
                if (index >= 0 && index < TokenAlphabet.Length)
                {
                    sb.Append(TokenAlphabet[index]);
                }
            }

            return sb.ToString();
        }

        public string Id()
        {
            var parts = U8();
            if (parts == NamelessMarker)
            {
                return FormatNameless(U64());
            }

            if (parts == 0)
            {
                return "null";
            }

            var sb = new StringBuilder();
            for (var i = 0; i < parts; i++)
            {
                if (i > 0)
                {
                    sb.Append('.');
                }

                sb.Append(Token());
            }

            return sb.ToString();
        }

        public List<string> Array(Func<string> readItem)
        {
            var count = (int)U32();
            var items = new List<string>(count);
            for (var i = 0; i < count; i++)
            {
                items.Add(readItem());
            }

            return items;
        }

        /// <summary>"_nameless." + hex digits grouped by four from the right, e.g. _nameless.1f5.d4c0.5a10.</summary>
        private static string FormatNameless(ulong value)
        {
            var hex = value.ToString("x", SiiValues.Invariant);
            var groups = new List<string>();
            for (var end = hex.Length; end > 0; end -= 4)
            {
                var start = Math.Max(0, end - 4);
                groups.Insert(0, hex[start..end]);
            }

            return "_nameless." + string.Join('.', groups);
        }

        private int Take(int count)
        {
            if (count < 0 || Position + count > _data.Length)
            {
                throw new InvalidDataException($"BSII-Daten abgeschnitten (Position {Position}, {count} Bytes).");
            }

            var start = Position;
            Position += count;
            return start;
        }
    }
}
