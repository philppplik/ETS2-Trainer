using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.App.ViewModels;

public sealed record DriverRow(string Name, string Hometown, string Xp, string Skills);

public sealed record TruckRow(string Model, string Plate, string Wear, string Fuel, string Odometer, string Engine, bool IsPlayer);

/// <summary>Company, garages, drivers and fleet — all edits go into the loaded save and are written on "Speichern".</summary>
public sealed class SaveEditorViewModel : ObservableObject
{
    private const long MaxDriverXp = 5_000_000;
    private static readonly TimeSpan CloudWriteTimeout = TimeSpan.FromSeconds(20);

    private static readonly CultureInfo German = CultureInfo.GetCultureInfo("de-DE");
    private static readonly string[] AdrLabels =
        { "Explosiv", "Gase", "Entzündl. Flüssigkeiten", "Entzündl. Feststoffe", "Giftig", "Ätzend" };
    private static readonly string[] SkillLabels =
        { "Langstrecke", "Wertvolle Fracht", "Zerbrechliche Fracht", "Pünktlichkeit", "Sparsames Fahren" };

    private readonly MainViewModel _main;
    private ProfileInfo? _selectedProfile;
    private SaveSlot? _selectedSlot;
    private LoadedSave? _loaded;
    private long _money;
    private long _experience;
    private bool _isDirty;
    private bool _overwrite;

    public SaveEditorViewModel(MainViewModel main)
    {
        _main = main;
        Adr = AdrLabels.Select((label, bit) => new FlagItem(label, bit, MarkDirty)).ToList();
        Skills = SaveGame.Skills.Select((name, i) => new SkillItem(name, SkillLabels[i], MarkDirty)).ToList();

        RefreshCommand = new RelayCommand(RefreshProfiles);
        LoadCommand = new AsyncCommand(LoadAsync, main.ShowError, () => SelectedSlot is not null);
        SaveCommand = new AsyncCommand(SaveAsync, main.ShowError, () => IsLoaded);
        AddMoneyCommand = new RelayCommand(p => Money += ParseAmount(p), _ => IsLoaded);
        SetMoneyCommand = new RelayCommand(p => Money = ParseAmount(p), _ => IsLoaded);
        AddXpCommand = new RelayCommand(p => Experience += ParseAmount(p), _ => IsLoaded);
        SetXpCommand = new RelayCommand(p => Experience = ParseAmount(p), _ => IsLoaded);
        MaxSkillsCommand = new RelayCommand(MaxSkills, () => IsLoaded);
        ClearLoansCommand = new RelayCommand(() => Edit(ClearLoans), () => IsLoaded);
        VisitCitiesCommand = new RelayCommand(() => Edit(g => $"{g.VisitAllCities()} Städte neu entdeckt"), () => IsLoaded);
        BuyGaragesCommand = new RelayCommand(() => Edit(g => $"{g.BuyAndUpgradeAllGarages()} Garagen gekauft/ausgebaut"), () => IsLoaded);
        MaxDriversCommand = new RelayCommand(() => Edit(g => $"{g.MaxAllDrivers(MaxDriverXp)} Fahrer maximiert"), () => IsLoaded);
        RepairFleetCommand = new RelayCommand(() => Edit(RepairFleet), () => IsLoaded);

        RefreshProfiles();
    }

    public ObservableCollection<ProfileInfo> Profiles { get; } = new();

    public ObservableCollection<SaveSlot> Slots { get; } = new();

    public ObservableCollection<DriverRow> Drivers { get; } = new();

    public ObservableCollection<TruckRow> Trucks { get; } = new();

    public IReadOnlyList<FlagItem> Adr { get; }

    public IReadOnlyList<SkillItem> Skills { get; }

    public ICommand RefreshCommand { get; }

    public ICommand LoadCommand { get; }

    public ICommand SaveCommand { get; }

    public ICommand AddMoneyCommand { get; }

    public ICommand SetMoneyCommand { get; }

    public ICommand AddXpCommand { get; }

    public ICommand SetXpCommand { get; }

    public ICommand MaxSkillsCommand { get; }

    public ICommand ClearLoansCommand { get; }

    public ICommand VisitCitiesCommand { get; }

    public ICommand BuyGaragesCommand { get; }

    public ICommand MaxDriversCommand { get; }

    public ICommand RepairFleetCommand { get; }

    public LoadedSave? Loaded => _loaded;

    public bool IsLoaded => _loaded is not null;

    public string LoadedTitle => _loaded is { } l ? $"{l.Slot.Name}  ·  {l.Slot.SavedAt:g}" : "Kein Spielstand geladen";

    public ProfileInfo? SelectedProfile
    {
        get => _selectedProfile;
        set
        {
            if (Set(ref _selectedProfile, value))
            {
                RefreshSlots();
                _main.UpdateSettings(s => s with { LastProfileDirectory = value?.Directory });
            }
        }
    }

    public SaveSlot? SelectedSlot { get => _selectedSlot; set => Set(ref _selectedSlot, value); }

    public long Money
    {
        get => _money;
        set
        {
            if (Set(ref _money, Math.Clamp(value, 0, long.MaxValue / 2)))
            {
                Raise(nameof(MoneyText));
                MarkDirty();
            }
        }
    }

    public string MoneyText => _money.ToString("N0", German) + " €";

    public long Experience
    {
        get => _experience;
        set
        {
            if (Set(ref _experience, Math.Clamp(value, 0, uint.MaxValue)))
            {
                Raise(nameof(ExperienceText));
                MarkDirty();
            }
        }
    }

    public string ExperienceText => _experience.ToString("N0", German) + " XP";

    public string GarageSummary => _loaded is { } l
        ? $"{l.Game.Garages().Count(g => g.IsOwned)} von {l.Game.Garages().Count} Garagen gehören dir"
        : "–";

    public string FleetSummary => _loaded is { } l
        ? $"{l.Game.Trucks().Count} Trucks · {l.Game.HiredDrivers().Count} Fahrer · {l.Game.LoanCount} Kredite · {l.Game.VisitedCityCount} Städte besucht"
        : "–";

    public bool IsDirty { get => _isDirty; private set => Set(ref _isDirty, value); }

    /// <summary>false = new slot (recommended), true = overwrite with backup.</summary>
    public bool Overwrite { get => _overwrite; set => Set(ref _overwrite, value); }

    public void MarkDirty() => IsDirty = IsLoaded;

    /// <summary>Called by the workshop after it changed the loaded save (engine swap).</summary>
    public void NotifyExternalEdit(string message)
    {
        RefreshLists();
        IsDirty = true;
        _main.Toast(message + " – zum Übernehmen „Speichern“ klicken.");
    }

    private void RefreshProfiles()
    {
        Profiles.Clear();
        foreach (var p in GamePaths.FindProfiles())
        {
            Profiles.Add(p);
        }

        SelectedProfile = Profiles.FirstOrDefault(p => p.Directory == _main.Settings.LastProfileDirectory)
                          ?? Profiles.FirstOrDefault(p => Directory.Exists(p.SaveDirectory))
                          ?? Profiles.FirstOrDefault();
    }

    private void RefreshSlots()
    {
        Slots.Clear();
        if (_selectedProfile is null)
        {
            return;
        }

        foreach (var slot in SaveSlotService.ListSlots(_selectedProfile))
        {
            Slots.Add(slot);
        }

        SelectedSlot = Slots.FirstOrDefault();
    }

    private async Task LoadAsync()
    {
        var slot = SelectedSlot!;
        _main.Busy($"Lade „{slot.Name}“ …");
        _loaded = await Task.Run(() => SaveSlotService.Load(slot));
        PullFromModel();
        IsDirty = false;
        _main.Toast($"Spielstand geladen: {slot.Name}");
        _main.OnSaveLoaded();
    }

    private async Task SaveAsync()
    {
        var loaded = _loaded!;
        PushToModel();
        var mode = Overwrite ? SaveWriteMode.OverwriteWithBackup : SaveWriteMode.NewSlot;
        if (_selectedProfile is { IsSteamCloud: true } cloudProfile)
        {
            await SaveToCloudAsync(loaded, cloudProfile, mode);
            return;
        }

        _main.Busy("Speichere …");
        var folder = await Task.Run(() => SaveSlotService.Write(loaded, mode));
        IsDirty = false;
        RefreshSlots();
        var hint = SaveSlotService.IsGameRunning()
            ? "Jetzt im Spiel: Menü → Laden → „[Trainer] …“ wählen."
            : "Beim nächsten Start unter „Laden“ auswählen.";
        _main.Toast($"Gespeichert (Ordner {Path.GetFileName(folder)}). {hint}");
    }

    /// <summary>
    /// Steam-Cloud profiles are read by ETS2 through Steam, so files written from outside stay
    /// invisible in-game. The save is staged locally and the plugin writes it through the game's Steam API.
    /// </summary>
    private async Task SaveToCloudAsync(LoadedSave loaded, ProfileInfo profile, SaveWriteMode mode)
    {
        if (!_main.Live.CanWriteCloudSaves)
        {
            _main.ShowError("Steam-Cloud-Profil: Starte ETS2 mit dem Plugin (Hauptmenü reicht) und speichere dann erneut – nur so zeigt das Spiel den Spielstand an.");
            return;
        }

        _main.Busy("Speichere über Steam Cloud …");
        var folder = await Task.Run(() =>
        {
            if (mode == SaveWriteMode.OverwriteWithBackup)
            {
                SaveSlotService.BackupFolder(loaded.Slot.Directory);
            }

            var slotFolder = SaveSlotService.PrepareForWrite(loaded, mode);
            CloudSaveStager.Stage(loaded, profile, slotFolder);
            return slotFolder;
        });

        var (code, message) = await _main.Live.WriteCloudSaveAsync(CloudWriteTimeout);
        if (code < 0)
        {
            _main.ShowError($"Speichern fehlgeschlagen: {message}");
            return;
        }

        CloudSaveStager.CleanUp();
        IsDirty = false;
        RefreshSlots();
        _main.Toast($"In Steam Cloud gespeichert (Ordner {folder}). Im Spiel: Laden → „[Trainer] …“ – ist das Menü schon offen, einmal schließen und neu öffnen.");
    }

    private void PullFromModel()
    {
        var game = _loaded!.Game;
        _money = game.Money;
        _experience = game.ExperiencePoints;
        foreach (var flag in Adr)
        {
            flag.Load((game.Adr & (1 << flag.Bit)) != 0);
        }

        foreach (var skill in Skills)
        {
            skill.Load(game.GetSkill(skill.Key));
        }

        RefreshLists();
        RaiseAll();
    }

    private void PushToModel()
    {
        var game = _loaded!.Game;
        game.Money = Money;
        game.ExperiencePoints = Experience;
        game.Adr = Adr.Where(f => f.IsChecked).Sum(f => 1 << f.Bit);
        foreach (var skill in Skills)
        {
            game.SetSkill(skill.Key, skill.Value);
        }
    }

    private void RefreshLists()
    {
        var game = _loaded!.Game;
        Drivers.Clear();
        foreach (var d in game.HiredDrivers())
        {
            Drivers.Add(new DriverRow(d.Id.Replace("driver.", "Fahrer ", StringComparison.Ordinal), d.Hometown,
                d.Experience.ToString("N0", German),
                $"ADR {BitCount(d.Adr)}/6 · {d.LongDistance}/{d.Heavy}/{d.Fragile}/{d.Urgent}/{d.Mechanical}"));
        }

        Trucks.Clear();
        foreach (var t in game.Trucks().OrderByDescending(t => t.IsPlayerTruck))
        {
            Trucks.Add(new TruckRow(t.Model, t.LicensePlate, $"{t.WorstWear * 100:0.0} %",
                $"{t.Fuel * 100:0} %", t.Odometer.ToString("N0", German) + " km",
                Path.GetFileNameWithoutExtension(t.EnginePath), t.IsPlayerTruck));
        }

        Raise(nameof(GarageSummary));
        Raise(nameof(FleetSummary));
    }

    private void MaxSkills()
    {
        foreach (var flag in Adr)
        {
            flag.IsChecked = true;
        }

        foreach (var skill in Skills)
        {
            skill.Value = SaveGame.MaxSkill;
        }
    }

    private void Edit(Func<SaveGame, string> action)
    {
        PushToModel();
        var message = action(_loaded!.Game);
        PullFromModel();
        IsDirty = true;
        _main.Toast(message + " – zum Übernehmen „Speichern“ klicken.");
    }

    private static string ClearLoans(SaveGame game)
    {
        var count = game.LoanCount;
        game.ClearLoans();
        return $"{count} Kredit(e) getilgt";
    }

    private static string RepairFleet(SaveGame game)
    {
        var (trucks, trailers) = game.RepairAndRefuelFleet();
        return $"{trucks} Trucks + {trailers} Auflieger repariert & betankt";
    }

    private static long ParseAmount(object? parameter) =>
        long.TryParse(parameter?.ToString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : 0;

    private static int BitCount(int value) => System.Numerics.BitOperations.PopCount((uint)value);
}

/// <summary>ADR class checkbox.</summary>
public sealed class FlagItem : ObservableObject
{
    private readonly Action _changed;
    private bool _isChecked;

    public FlagItem(string label, int bit, Action changed)
    {
        Label = label;
        Bit = bit;
        _changed = changed;
    }

    public string Label { get; }

    public int Bit { get; }

    public bool IsChecked
    {
        get => _isChecked;
        set
        {
            if (Set(ref _isChecked, value))
            {
                _changed();
            }
        }
    }

    public void Load(bool value)
    {
        _isChecked = value;
        Raise(nameof(IsChecked));
    }
}

/// <summary>Skill slider 0..6.</summary>
public sealed class SkillItem : ObservableObject
{
    private readonly Action _changed;
    private int _value;

    public SkillItem(string key, string label, Action changed)
    {
        Key = key;
        Label = label;
        _changed = changed;
    }

    public string Key { get; }

    public string Label { get; }

    public int Value
    {
        get => _value;
        set
        {
            if (Set(ref _value, Math.Clamp(value, 0, SaveGame.MaxSkill)))
            {
                _changed();
            }
        }
    }

    public void Load(int value)
    {
        _value = value;
        Raise(nameof(Value));
    }
}
