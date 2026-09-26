using System.Text;
using System.Text.RegularExpressions;

namespace Ets2Trainer.Core.Sii;

public enum SiiArrayStyle
{
    /// <summary><c>name: 2</c>, <c>name[0]: a</c>, <c>name[1]: b</c> — used by saves.</summary>
    Indexed,

    /// <summary><c>name[]: a</c>, <c>name[]: b</c> — used by def files.</summary>
    Appended,
}

/// <summary>Serializes a <see cref="SiiDocument"/> to SII text ("SiiNunit").</summary>
public static class SiiTextWriter
{
    public static string Write(SiiDocument document, SiiArrayStyle style = SiiArrayStyle.Indexed)
    {
        var sb = new StringBuilder(1 << 20);
        sb.Append("SiiNunit\n{\n");
        foreach (var unit in document.Units)
        {
            WriteUnit(sb, unit, style);
        }

        sb.Append("}\n");
        return sb.ToString();
    }

    private static void WriteUnit(StringBuilder sb, SiiUnit unit, SiiArrayStyle style)
    {
        sb.Append(unit.ClassName).Append(" : ").Append(unit.Id).Append(" {\n");
        foreach (var attribute in unit.Attributes)
        {
            if (attribute.Items is { } items)
            {
                WriteArray(sb, attribute.Name, items, style);
            }
            else
            {
                sb.Append(' ').Append(attribute.Name).Append(": ").Append(attribute.Value).Append('\n');
            }
        }

        sb.Append("}\n\n");
    }

    private static void WriteArray(StringBuilder sb, string name, List<string> items, SiiArrayStyle style)
    {
        if (style == SiiArrayStyle.Indexed)
        {
            sb.Append(' ').Append(name).Append(": ").Append(items.Count).Append('\n');
            for (var i = 0; i < items.Count; i++)
            {
                sb.Append(' ').Append(name).Append('[').Append(i).Append("]: ").Append(items[i]).Append('\n');
            }

            return;
        }

        foreach (var item in items)
        {
            sb.Append(' ').Append(name).Append("[]: ").Append(item).Append('\n');
        }
    }
}

/// <summary>
/// Line-based parser for SII text (saves and def files). Supports comments (<c>#</c>, <c>//</c>, <c>/* */</c>),
/// <c>@include</c> via a resolver, indexed (<c>a[0]:</c>) and appended (<c>a[]:</c>) arrays.
/// </summary>
public static partial class SiiTextParser
{
    private const int MaxIncludeDepth = 8;
    private const char ByteOrderMark = '﻿';

    public static SiiDocument Parse(string text, Func<string, string?>? includeResolver = null)
    {
        var lines = new List<string>();
        Flatten(text, includeResolver, lines, 0);
        return BuildDocument(lines);
    }

    private static void Flatten(string text, Func<string, string?>? resolver, List<string> output, int depth)
    {
        foreach (var raw in StripComments(text).Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0)
            {
                continue;
            }

            var include = IncludeRegex().Match(line);
            if (include.Success)
            {
                if (resolver is not null && depth < MaxIncludeDepth &&
                    resolver(include.Groups[1].Value) is { } included)
                {
                    Flatten(included, resolver, output, depth + 1);
                }

                continue;
            }

            output.Add(line);
        }
    }

    private static SiiDocument BuildDocument(List<string> lines)
    {
        var units = new List<SiiUnit>();
        UnitBuilder? current = null;
        var awaitingBrace = false;

        foreach (var line in lines)
        {
            if (current is null)
            {
                var header = UnitHeaderRegex().Match(line);
                if (header.Success)
                {
                    current = new UnitBuilder(header.Groups[1].Value, header.Groups[2].Value);
                    awaitingBrace = !header.Groups[3].Success;
                }

                continue; // "SiiNunit", outer braces, stray lines
            }

            if (awaitingBrace)
            {
                awaitingBrace = false;
                if (line == "{")
                {
                    continue;
                }
            }

            if (line == "}")
            {
                units.Add(current.Build());
                current = null;
                continue;
            }

            var attr = AttributeRegex().Match(line);
            if (attr.Success)
            {
                current.Add(attr.Groups[1].Value, attr.Groups[2].Success ? attr.Groups[3].Value : null,
                    attr.Groups[4].Value.Trim());
            }
        }

        return new SiiDocument(units);
    }

    /// <summary>Removes comments outside of string literals; keeps line structure.</summary>
    private static string StripComments(string text)
    {
        var sb = new StringBuilder(text.Length);
        var inString = false;
        var inBlock = false;
        var inLine = false;
        for (var i = 0; i < text.Length; i++)
        {
            var c = text[i];
            var next = i + 1 < text.Length ? text[i + 1] : '\0';
            if (inBlock)
            {
                if (c == '*' && next == '/') { inBlock = false; i++; }
                else if (c == '\n') { sb.Append('\n'); }
                continue;
            }

            if (inLine)
            {
                if (c == '\n') { inLine = false; sb.Append('\n'); }
                continue;
            }

            if (inString)
            {
                sb.Append(c);
                if (c == '\\' && next != '\0') { sb.Append(next); i++; }
                else if (c == '"' || c == '\n') { inString = false; }
                continue;
            }

            if (c == '"') { inString = true; sb.Append(c); }
            else if (c == '#' || (c == '/' && next == '/')) { inLine = true; }
            else if (c == '/' && next == '*') { inBlock = true; i++; }
            else if (c != '\r' && c != ByteOrderMark) { sb.Append(c); }
        }

        return sb.ToString();
    }

    [GeneratedRegex(@"^@include\s+""([^""]+)""")]
    private static partial Regex IncludeRegex();

    [GeneratedRegex(@"^([A-Za-z0-9_]+)\s*:\s*([^\s{]+)\s*(\{)?$")]
    private static partial Regex UnitHeaderRegex();

    [GeneratedRegex(@"^([A-Za-z0-9_]+)(\[(\d*)\])?\s*:\s*(.*)$")]
    private static partial Regex AttributeRegex();

    private sealed class UnitBuilder
    {
        private readonly string _className;
        private readonly string _id;
        private readonly List<string> _order = new();
        private readonly Dictionary<string, string> _scalars = new(StringComparer.Ordinal);
        private readonly Dictionary<string, SortedDictionary<int, string>> _indexed = new(StringComparer.Ordinal);
        private readonly Dictionary<string, List<string>> _appended = new(StringComparer.Ordinal);

        public UnitBuilder(string className, string id)
        {
            _className = className;
            _id = id;
        }

        public void Add(string name, string? index, string value)
        {
            if (!_scalars.ContainsKey(name) && !_indexed.ContainsKey(name) && !_appended.ContainsKey(name))
            {
                _order.Add(name);
            }

            if (index is null)
            {
                _scalars[name] = value;
            }
            else if (index.Length == 0)
            {
                GetOrAdd(_appended, name).Add(value);
            }
            else
            {
                GetOrAdd(_indexed, name)[int.Parse(index, SiiValues.Invariant)] = value;
            }
        }

        public SiiUnit Build()
        {
            var unit = new SiiUnit(_className, _id);
            foreach (var name in _order)
            {
                var isArray = _indexed.ContainsKey(name) || _appended.ContainsKey(name);
                if (!isArray)
                {
                    unit.Add(new SiiAttribute(name, _scalars[name]));
                    continue;
                }

                var items = new List<string>();
                if (_indexed.TryGetValue(name, out var byIndex))
                {
                    items.AddRange(byIndex.Values);
                }

                if (_appended.TryGetValue(name, out var appended))
                {
                    items.AddRange(appended);
                }

                unit.Add(new SiiAttribute(name, items));
            }

            return unit;
        }

        private static TValue GetOrAdd<TValue>(Dictionary<string, TValue> map, string key)
            where TValue : new()
        {
            if (!map.TryGetValue(key, out var value))
            {
                value = new TValue();
                map[key] = value;
            }

            return value;
        }
    }
}
