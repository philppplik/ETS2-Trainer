using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Core.Save;

public sealed record GarageInfo(string Id, string City, int Status, int Slots, int Vehicles, int Drivers)
{
    public bool IsOwned => Status != SaveGame.GarageStatusNone;
}

public sealed record DriverInfo(string Id, string Hometown, string CurrentCity, long Experience, int Adr, int LongDistance,
    int Heavy, int Fragile, int Urgent, int Mechanical);

public sealed record TruckInfo(string Id, string Model, string EnginePath, float WorstWear, float Fuel, long Odometer,
    string LicensePlate, bool IsPlayerTruck);

/// <summary>Typed view over a decoded <c>game.sii</c> with the edits the trainer offers.</summary>
public sealed class SaveGame
{
    public const int GarageStatusNone = 0;
    public const int GarageStatusLarge = 3;
    public const int LargeGarageSlots = 5;
    public const int MaxAdr = 63;
    public const int MaxSkill = 6;

    private static readonly string[] SkillNames = { "long_dist", "heavy", "fragile", "urgent", "mechanical" };

    private static readonly string[] VehicleWearFields =
    {
        "engine_wear", "transmission_wear", "cabin_wear", "chassis_wear",
        "engine_wear_unfixable", "transmission_wear_unfixable", "cabin_wear_unfixable", "chassis_wear_unfixable",
    };

    private static readonly string[] TrailerWearFields =
    {
        "trailer_body_wear", "chassis_wear", "trailer_body_wear_unfixable", "chassis_wear_unfixable", "cargo_damage",
    };

    public SaveGame(SiiDocument document)
    {
        Document = document;
        Economy = document.FirstOfClass("economy") ?? throw new InvalidDataException("Spielstand ohne 'economy'-Unit.");
        Bank = document.Find(Economy.Get("bank")) ?? throw new InvalidDataException("Spielstand ohne Bank.");
        Player = document.Find(Economy.Get("player")) ?? throw new InvalidDataException("Spielstand ohne Spieler.");
    }

    public SiiDocument Document { get; }

    public SiiUnit Economy { get; }

    public SiiUnit Bank { get; }

    public SiiUnit Player { get; }

    public static IReadOnlyList<string> Skills => SkillNames;

    public long Money
    {
        get => SiiValues.ParseLong(Bank.Get("money_account"));
        set => Bank.Set("money_account", SiiValues.FormatLong(Math.Max(0, value)));
    }

    public long ExperiencePoints
    {
        get => SiiValues.ParseLong(Economy.Get("experience_points"));
        set => Economy.Set("experience_points", SiiValues.FormatLong(Math.Clamp(value, 0, uint.MaxValue)));
    }

    public int Adr
    {
        get => (int)SiiValues.ParseLong(Economy.Get("adr"));
        set => Economy.Set("adr", SiiValues.FormatLong(Math.Clamp(value, 0, MaxAdr)));
    }

    public int LoanCount => Bank.GetArray("loans").Count;

    public int VisitedCityCount => Economy.GetArray("visited_cities").Count;

    public int GetSkill(string name) => (int)SiiValues.ParseLong(Economy.Get(name));

    public void SetSkill(string name, int value)
    {
        if (!SkillNames.Contains(name))
        {
            throw new ArgumentException($"Unbekannter Skill '{name}'.", nameof(name));
        }

        Economy.Set(name, SiiValues.FormatLong(Math.Clamp(value, 0, MaxSkill)));
    }

    /// <summary>Removes all bank loans (and the orphaned loan units).</summary>
    public void ClearLoans()
    {
        var loanIds = Bank.GetArray("loans").ToHashSet(StringComparer.Ordinal);
        Bank.SetArray("loans", Array.Empty<string>());
        if (Document.Units.RemoveAll(u => loanIds.Contains(u.Id)) > 0)
        {
            Document.InvalidateIndex();
        }
    }

    public IReadOnlyList<GarageInfo> Garages() =>
        Economy.GetArray("garages")
            .Select(id => Document.Find(id))
            .OfType<SiiUnit>()
            .Select(g => new GarageInfo(g.Id, CityOf(g.Id), (int)SiiValues.ParseLong(g.Get("status")),
                g.GetArray("vehicles").Count, g.GetArray("vehicles").Count(IsSet), g.GetArray("drivers").Count(IsSet)))
            .ToList();

    /// <summary>Buys every garage not owned yet and upgrades owned ones to large (5 slots). Returns changed count.</summary>
    public int BuyAndUpgradeAllGarages()
    {
        var changed = 0;
        foreach (var id in Economy.GetArray("garages"))
        {
            if (Document.Find(id) is not { } garage)
            {
                continue;
            }

            var status = (int)SiiValues.ParseLong(garage.Get("status"));
            if (status == GarageStatusLarge && garage.GetArray("vehicles").Count >= LargeGarageSlots)
            {
                continue;
            }

            garage.Set("status", SiiValues.FormatLong(GarageStatusLarge));
            garage.SetArray("vehicles", PadSlots(garage.GetArray("vehicles")));
            garage.SetArray("drivers", PadSlots(garage.GetArray("drivers")));
            if (!garage.Has("trailers"))
            {
                garage.SetArray("trailers", Array.Empty<string>());
            }

            changed++;
        }

        return changed;
    }

    public IReadOnlyList<DriverInfo> HiredDrivers() =>
        HiredDriverUnits()
            .Select(d => new DriverInfo(d.Id, d.Get("hometown") ?? "", d.Get("current_city") ?? "",
                SiiValues.ParseLong(d.Get("experience_points")), Skill(d, "adr"), Skill(d, "long_dist"),
                Skill(d, "heavy"), Skill(d, "fragile"), Skill(d, "urgent"), Skill(d, "mechanical")))
            .ToList();

    /// <summary>Maxes all skills of every hired driver and raises XP to at least <paramref name="minExperience"/>.</summary>
    public int MaxAllDrivers(long minExperience)
    {
        var count = 0;
        foreach (var d in HiredDriverUnits())
        {
            d.Set("adr", SiiValues.FormatLong(MaxAdr));
            foreach (var skill in SkillNames)
            {
                d.Set(skill, SiiValues.FormatLong(MaxSkill));
            }

            if (SiiValues.ParseLong(d.Get("experience_points")) < minExperience)
            {
                d.Set("experience_points", SiiValues.FormatLong(Math.Clamp(minExperience, 0, uint.MaxValue)));
            }

            count++;
        }

        return count;
    }

    public IReadOnlyList<TruckInfo> Trucks()
    {
        var playerTruck = PlayerTruck()?.Id;
        return Player.GetArray("trucks")
            .Select(id => Document.Find(id))
            .OfType<SiiUnit>()
            .Select(v => new TruckInfo(v.Id, TruckModelOf(v) ?? "?", EnginePathOf(v), WorstWear(v),
                SiiValues.ParseFloat(v.Get("fuel_relative"), 1f), SiiValues.ParseLong(v.Get("odometer")),
                CleanPlate(SiiValues.Unquote(v.Get("license_plate"))), v.Id == playerTruck))
            .ToList();
    }

    /// <summary>Plates look like "&lt;offset hshift=-5&gt;B 78 HNO|romania": drop markup and country suffix.</summary>
    public static string CleanPlate(string rawPlate)
    {
        var text = rawPlate.Split('|')[0];
        var sb = new System.Text.StringBuilder(text.Length);
        var inTag = false;
        foreach (var c in text)
        {
            if (c == '<') { inTag = true; }
            else if (c == '>') { inTag = false; }
            else if (!inTag) { sb.Append(c); }
        }

        return sb.ToString().Trim();
    }

    /// <summary>Repairs and refuels all company trucks and trailers. Returns (trucks, trailers) touched.</summary>
    public (int Trucks, int Trailers) RepairAndRefuelFleet()
    {
        var trucks = 0;
        foreach (var v in Player.GetArray("trucks").Select(id => Document.Find(id)).OfType<SiiUnit>())
        {
            ZeroFields(v, VehicleWearFields);
            ZeroArray(v, "wheels_wear");
            ZeroArray(v, "wheels_wear_unfixable");
            v.Set("fuel_relative", "1");
            trucks++;
        }

        var trailers = 0;
        foreach (var t in Player.GetArray("trailers").Select(id => Document.Find(id)).OfType<SiiUnit>())
        {
            ZeroFields(t, TrailerWearFields);
            ZeroArray(t, "wheels_wear");
            ZeroArray(t, "wheels_wear_unfixable");
            trailers++;
        }

        return (trucks, trailers);
    }

    /// <summary>The truck the player currently drives (via player_vehicles in 1.5x+, my_truck in older saves).</summary>
    public SiiUnit? PlayerTruck()
    {
        if (Document.Find(Player.Get("assigned_vehicles")) is { } assigned &&
            Document.Find(assigned.Get("vehicle")) is { } vehicle)
        {
            return vehicle;
        }

        return Document.Find(Player.Get("assigned_truck")) ?? Document.Find(Player.Get("my_truck"));
    }

    public SiiUnit? EngineAccessory(SiiUnit vehicle) =>
        vehicle.GetArray("accessories")
            .Select(id => Document.Find(id))
            .OfType<SiiUnit>()
            .FirstOrDefault(a => a.ClassName == "vehicle_accessory" &&
                                 (a.Get("data_path") ?? "").Contains("/engine/", StringComparison.Ordinal));

    public string EnginePathOf(SiiUnit vehicle) =>
        EngineAccessory(vehicle)?.Get("data_path") is { } path ? SiiValues.Unquote(path) : "";

    /// <summary>Swaps the engine of <paramref name="vehicle"/> to the given def path (e.g. a generated trainer engine).</summary>
    public void SetEngine(SiiUnit vehicle, string engineDataPath)
    {
        var engine = EngineAccessory(vehicle) ?? throw new InvalidOperationException("Der Truck hat keinen Motor-Eintrag.");
        engine.Set("data_path", SiiValues.Quote(engineDataPath));
    }

    /// <summary>"scania.s_2016" for the vehicle's engine accessory path.</summary>
    public string? TruckModelOf(SiiUnit vehicle) => TruckModelFromPath(EnginePathOf(vehicle));

    public static string? TruckModelFromPath(string dataPath)
    {
        const string prefix = "/def/vehicle/truck/";
        if (!dataPath.StartsWith(prefix, StringComparison.Ordinal))
        {
            return null;
        }

        var rest = dataPath[prefix.Length..];
        var slash = rest.IndexOf('/');
        return slash > 0 ? rest[..slash] : null;
    }

    /// <summary>Marks every city that has a garage or company as visited. Returns number of newly visited cities.</summary>
    public int VisitAllCities()
    {
        var visited = Economy.GetArray("visited_cities").ToList();
        var counts = Economy.GetArray("visited_cities_count").ToList();
        var known = visited.ToHashSet(StringComparer.Ordinal);
        var cities = Economy.GetArray("garages").Select(CityOf)
            .Concat(Economy.GetArray("companies").Select(c => c.Split('.').LastOrDefault() ?? ""))
            .Where(c => c.Length > 0)
            .Distinct(StringComparer.Ordinal)
            .Where(c => !known.Contains(c))
            .ToList();

        foreach (var city in cities)
        {
            visited.Add(city);
            counts.Add("1");
        }

        Economy.SetArray("visited_cities", visited);
        Economy.SetArray("visited_cities_count", counts);
        return cities.Count;
    }

    private IEnumerable<SiiUnit> HiredDriverUnits() =>
        Player.GetArray("drivers").Select(id => Document.Find(id)).OfType<SiiUnit>().Where(u => u.ClassName == "driver_ai");

    private static string CityOf(string garageId) =>
        garageId.StartsWith("garage.", StringComparison.Ordinal) ? garageId[7..] : garageId;

    private static bool IsSet(string id) => id != "null" && id.Length > 0;

    private static int Skill(SiiUnit u, string name) => (int)SiiValues.ParseLong(u.Get(name));

    private static List<string> PadSlots(IReadOnlyList<string> slots)
    {
        var list = slots.ToList();
        while (list.Count < LargeGarageSlots)
        {
            list.Add("null");
        }

        return list;
    }

    private static float WorstWear(SiiUnit v)
    {
        var values = VehicleWearFields.Select(f => SiiValues.ParseFloat(v.Get(f)))
            .Concat(v.GetArray("wheels_wear").Select(w => SiiValues.ParseFloat(w)));
        return values.DefaultIfEmpty(0f).Max();
    }

    private static void ZeroFields(SiiUnit unit, IEnumerable<string> fields)
    {
        foreach (var field in fields.Where(unit.Has))
        {
            unit.Set(field, "0");
        }
    }

    private static void ZeroArray(SiiUnit unit, string name)
    {
        if (unit.Attribute(name)?.Items is { } items)
        {
            unit.SetArray(name, Enumerable.Repeat("0", items.Count));
        }
    }
}
