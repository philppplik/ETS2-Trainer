using System.Globalization;
using System.Text.Json;

namespace Ets2Trainer.Core.Game;

public enum PlaceKind
{
    Bookmark,
    Company,
}

/// <summary>A teleport target: manual bookmark or a company learned from job start/delivery.</summary>
public sealed record Place(string Name, PlaceKind Kind, string CompanyId, string CityId, double X, double Y, double Z,
    float Heading, DateTime CreatedUtc)
{
    public string Key => Kind == PlaceKind.Company ? $"{CompanyId}.{CityId}" : $"bm:{Name}";

    public override string ToString() => Name;
}

/// <summary>Teleport targets persisted as JSON in %AppData%\ETS2Trainer\places.json.</summary>
public sealed class PlacesStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly string _path;
    private List<Place> _places;

    public PlacesStore(string? path = null)
    {
        _path = path ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ETS2Trainer", "places.json");
        _places = Load(_path);
    }

    public IReadOnlyList<Place> All => _places;

    public Place? FindCompany(string companyId, string cityId) =>
        _places.FirstOrDefault(p => p.Kind == PlaceKind.Company && p.CompanyId == companyId && p.CityId == cityId);

    /// <summary>Adds or replaces a place (same key) and saves.</summary>
    public void Upsert(Place place)
    {
        _places = _places.Where(p => p.Key != place.Key).Append(place)
            .OrderBy(p => p.Kind).ThenBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        Save();
    }

    public void Remove(Place place)
    {
        _places = _places.Where(p => p.Key != place.Key).ToList();
        Save();
    }

    /// <summary>Human readable company name from ids like "tree_et" + "felixstowe".</summary>
    public static string CompanyLabel(string companyId, string cityId)
    {
        var text = CultureInfo.CurrentCulture.TextInfo;
        string Pretty(string id) => text.ToTitleCase(id.Replace('_', ' '));
        return $"{Pretty(companyId)} · {Pretty(cityId)}";
    }

    private void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temp = _path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(_places, JsonOptions));
        File.Move(temp, _path, overwrite: true);
    }

    private static List<Place> Load(string path)
    {
        try
        {
            return File.Exists(path)
                ? JsonSerializer.Deserialize<List<Place>>(File.ReadAllText(path)) ?? new List<Place>()
                : new List<Place>();
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return new List<Place>();  // a damaged file must not break the trainer
        }
    }
}

/// <summary>Share codes for "teleport to player": ETS2T:x;y;z;heading (invariant culture).</summary>
public static class PositionCode
{
    private const string Prefix = "ETS2T:";

    public static string Encode(double x, double y, double z, float heading) =>
        string.Create(CultureInfo.InvariantCulture, $"{Prefix}{x:0.00};{y:0.00};{z:0.00};{heading:0.0000}");

    public static bool TryDecode(string? text, out double x, out double y, out double z, out float heading)
    {
        x = y = z = 0;
        heading = 0;
        var t = text?.Trim() ?? "";
        if (!t.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var parts = t[Prefix.Length..].Split(';');
        const NumberStyles style = NumberStyles.Float;
        var inv = CultureInfo.InvariantCulture;
        return parts.Length == 4 &&
               double.TryParse(parts[0], style, inv, out x) && double.TryParse(parts[1], style, inv, out y) &&
               double.TryParse(parts[2], style, inv, out z) && float.TryParse(parts[3], style, inv, out heading) &&
               double.IsFinite(x) && double.IsFinite(y) && double.IsFinite(z) && Math.Abs(x) < 1e7 && Math.Abs(z) < 1e7;
    }
}
