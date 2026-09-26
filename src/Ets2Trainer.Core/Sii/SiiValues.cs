using System.Globalization;
using System.Text;

namespace Ets2Trainer.Core.Sii;

/// <summary>Conversions between SII text notation and .NET values.</summary>
public static class SiiValues
{
    public static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

    private const float IntegerFormatLimit = 1e7f;

    /// <summary>Integral floats print as integers, everything else as exact hex bits (<c>&amp;3f800000</c>).</summary>
    public static string FormatFloat(float value)
    {
        if (float.IsFinite(value) && MathF.Abs(value) < IntegerFormatLimit && value == MathF.Truncate(value))
        {
            return ((long)value).ToString(Invariant);
        }

        var bits = BitConverter.SingleToUInt32Bits(value);
        return "&" + bits.ToString("x8", Invariant);
    }

    public static bool TryParseFloat(string? text, out float value)
    {
        value = 0;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        var t = text.Trim();
        if (t.StartsWith('&'))
        {
            if (uint.TryParse(t.AsSpan(1), NumberStyles.HexNumber, Invariant, out var bits))
            {
                value = BitConverter.UInt32BitsToSingle(bits);
                return true;
            }

            return false;
        }

        return float.TryParse(t, NumberStyles.Float, Invariant, out value);
    }

    public static float ParseFloat(string? text, float fallback = 0) =>
        TryParseFloat(text, out var v) ? v : fallback;

    public static long ParseLong(string? text, long fallback = 0)
    {
        if (string.IsNullOrWhiteSpace(text) || text == "nil")
        {
            return fallback;
        }

        if (long.TryParse(text.Trim(), NumberStyles.Integer, Invariant, out var v))
        {
            return v;
        }

        return TryParseFloat(text, out var f) ? (long)f : fallback;
    }

    public static string FormatLong(long value) => value.ToString(Invariant);

    public static bool ParseBool(string? text) => string.Equals(text?.Trim(), "true", StringComparison.Ordinal);

    public static string FormatBool(bool value) => value ? "true" : "false";

    /// <summary>Quotes a string for SII text: escapes quotes/backslashes and non-ASCII bytes as \xHH.</summary>
    public static string Quote(string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        var sb = new StringBuilder(bytes.Length + 2);
        sb.Append('"');
        foreach (var b in bytes)
        {
            switch (b)
            {
                case (byte)'"':
                    sb.Append("\\\"");
                    break;
                case (byte)'\\':
                    sb.Append("\\\\");
                    break;
                case < 0x20 or >= 0x7F:
                    sb.Append("\\x").Append(b.ToString("x2", Invariant));
                    break;
                default:
                    sb.Append((char)b);
                    break;
            }
        }

        sb.Append('"');
        return sb.ToString();
    }

    /// <summary>Reverses <see cref="Quote"/>; returns unquoted tokens unchanged.</summary>
    public static string Unquote(string? value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return string.Empty;
        }

        var t = value.Trim();
        if (t.Length < 2 || t[0] != '"' || t[^1] != '"')
        {
            return t;
        }

        var bytes = new List<byte>(t.Length);
        var end = t.Length - 1;
        for (var i = 1; i < end; i++)
        {
            var c = t[i];
            if (c == '\\' && i + 1 < end)
            {
                var next = t[i + 1];
                if (next == 'x' && i + 3 < end &&
                    byte.TryParse(t.AsSpan(i + 2, 2), NumberStyles.HexNumber, Invariant, out var hex))
                {
                    bytes.Add(hex);
                    i += 3;
                    continue;
                }

                bytes.AddRange(Encoding.UTF8.GetBytes(next.ToString()));
                i++;
                continue;
            }

            bytes.AddRange(Encoding.UTF8.GetBytes(c.ToString()));
        }

        return Encoding.UTF8.GetString(bytes.ToArray());
    }
}
