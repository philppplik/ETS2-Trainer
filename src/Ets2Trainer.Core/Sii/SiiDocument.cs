namespace Ets2Trainer.Core.Sii;

/// <summary>
/// One attribute of a SII unit. Values are kept in their SII text representation
/// (e.g. <c>12345</c>, <c>&amp;3f800000</c>, <c>"text"</c>, <c>_nameless.1f5.d4c0</c>, <c>nil</c>),
/// which makes editing and lossless re-serialization trivial.
/// </summary>
public sealed class SiiAttribute
{
    public SiiAttribute(string name, string value)
    {
        Name = name;
        Value = value;
    }

    public SiiAttribute(string name, IEnumerable<string> items)
    {
        Name = name;
        Items = items.ToList();
    }

    public string Name { get; }

    /// <summary>Scalar value (null for arrays).</summary>
    public string? Value { get; internal set; }

    /// <summary>Array items (null for scalars).</summary>
    public List<string>? Items { get; internal set; }

    public bool IsArray => Items is not null;
}

/// <summary>A SII unit: <c>class_name : unit.id { ... }</c>.</summary>
/// <remarks>
/// Editing a save means touching a handful of attributes inside a ~40k unit document, so units are
/// edited in place (idiomatic C# DOM) instead of being rebuilt immutably.
/// </remarks>
public sealed class SiiUnit
{
    private readonly List<SiiAttribute> _attributes = new();
    private readonly Dictionary<string, SiiAttribute> _byName = new(StringComparer.Ordinal);

    public SiiUnit(string className, string id)
    {
        ClassName = className;
        Id = id;
    }

    public string ClassName { get; }

    public string Id { get; }

    public IReadOnlyList<SiiAttribute> Attributes => _attributes;

    public bool Has(string name) => _byName.ContainsKey(name);

    public SiiAttribute? Attribute(string name) => _byName.GetValueOrDefault(name);

    /// <summary>Scalar value or null when missing / array.</summary>
    public string? Get(string name) => _byName.TryGetValue(name, out var a) ? a.Value : null;

    /// <summary>Array items, empty when missing.</summary>
    public IReadOnlyList<string> GetArray(string name) =>
        _byName.TryGetValue(name, out var a) && a.Items is not null ? a.Items : Array.Empty<string>();

    /// <summary>Sets (or appends) a scalar attribute.</summary>
    public void Set(string name, string value)
    {
        if (_byName.TryGetValue(name, out var existing))
        {
            existing.Items = null;
            existing.Value = value;
            return;
        }

        Add(new SiiAttribute(name, value));
    }

    /// <summary>Sets (or appends) an array attribute.</summary>
    public void SetArray(string name, IEnumerable<string> items)
    {
        if (_byName.TryGetValue(name, out var existing))
        {
            existing.Value = null;
            existing.Items = items.ToList();
            return;
        }

        Add(new SiiAttribute(name, items));
    }

    /// <summary>Sets an array item in place (no-op when out of range).</summary>
    public void SetArrayItem(string name, int index, string value)
    {
        if (_byName.TryGetValue(name, out var a) && a.Items is { } items && index >= 0 && index < items.Count)
        {
            items[index] = value;
        }
    }

    internal void Add(SiiAttribute attribute)
    {
        if (_byName.TryGetValue(attribute.Name, out var existing))
        {
            // Duplicate names: last one wins, like the game's parser.
            _attributes.Remove(existing);
        }

        _attributes.Add(attribute);
        _byName[attribute.Name] = attribute;
    }

    public override string ToString() => $"{ClassName} : {Id} ({_attributes.Count} attributes)";
}

/// <summary>An in-memory SII document (list of units).</summary>
public sealed class SiiDocument
{
    private Dictionary<string, SiiUnit>? _byId;

    public SiiDocument(IEnumerable<SiiUnit> units)
    {
        Units = units.ToList();
    }

    public List<SiiUnit> Units { get; }

    public SiiUnit? Find(string? id)
    {
        if (string.IsNullOrEmpty(id) || id == "null")
        {
            return null;
        }

        _byId ??= BuildIndex();
        return _byId.GetValueOrDefault(id);
    }

    public IEnumerable<SiiUnit> OfClass(string className) =>
        Units.Where(u => string.Equals(u.ClassName, className, StringComparison.Ordinal));

    public SiiUnit? FirstOfClass(string className) => OfClass(className).FirstOrDefault();

    /// <summary>Must be called after units are added or removed so <see cref="Find"/> stays correct.</summary>
    public void InvalidateIndex() => _byId = null;

    private Dictionary<string, SiiUnit> BuildIndex()
    {
        var index = new Dictionary<string, SiiUnit>(Units.Count, StringComparer.Ordinal);
        foreach (var unit in Units)
        {
            index.TryAdd(unit.Id, unit);
        }

        return index;
    }
}
