using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.HashFs;
using Ets2Trainer.Core.Mods;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.App.ViewModels;

/// <summary>Engine workshop: pick a truck + base engine, choose horsepower, generate a mod engine and optionally install it.</summary>
public sealed class WorkshopViewModel : ObservableObject, IDisposable
{
    private const int DefaultTargetHp = 2000;
    private static readonly CultureInfo German = CultureInfo.GetCultureInfo("de-DE");

    private readonly MainViewModel _main;
    private readonly SaveEditorViewModel _save;
    private GameFileSystem? _vfs;
    private string? _selectedTruck;
    private EngineOption? _selectedEngine;
    private int _targetHp = DefaultTargetHp;
    private bool _isReady;
    private string _modState = "";

    public WorkshopViewModel(MainViewModel main, SaveEditorViewModel save)
    {
        _main = main;
        _save = save;
        InitializeCommand = new AsyncCommand(InitializeAsync, main.ShowError, () => !IsReady);
        GenerateCommand = new AsyncCommand(() => GenerateAsync(install: false), main.ShowError, CanGenerate);
        GenerateAndInstallCommand = new AsyncCommand(() => GenerateAsync(install: true), main.ShowError,
            () => CanGenerate() && _save.Loaded?.Game.PlayerTruck() is not null);
        ActivateModCommand = new RelayCommand(ActivateMod, () => _save.SelectedProfile is not null);
        PresetCommand = new RelayCommand(p => TargetHp = int.TryParse(p?.ToString(), out var hp) ? hp : TargetHp);
    }

    public ObservableCollection<string> TruckModels { get; } = new();

    public ObservableCollection<EngineOption> Engines { get; } = new();

    public ObservableCollection<string> GeneratedEngines { get; } = new();

    public ICommand InitializeCommand { get; }

    public ICommand GenerateCommand { get; }

    public ICommand GenerateAndInstallCommand { get; }

    public ICommand ActivateModCommand { get; }

    public ICommand PresetCommand { get; }

    public bool IsReady { get => _isReady; private set => Set(ref _isReady, value); }

    public string? SelectedTruck
    {
        get => _selectedTruck;
        set
        {
            if (Set(ref _selectedTruck, value))
            {
                LoadEngines();
            }
        }
    }

    public EngineOption? SelectedEngine
    {
        get => _selectedEngine;
        set
        {
            if (Set(ref _selectedEngine, value))
            {
                RaiseComparison();
            }
        }
    }

    public int TargetHp
    {
        get => _targetHp;
        set
        {
            if (Set(ref _targetHp, Math.Clamp(value, EngineModBuilder.MinHorsepower, EngineModBuilder.MaxHorsepower)))
            {
                RaiseComparison();
                Raise(nameof(TargetHpText));
            }
        }
    }

    public string TargetHpText => _targetHp.ToString("N0", German) + " PS";

    public string TorquePreview => SelectedEngine is { Horsepower: > 0 } e
        ? $"≈ {e.TorqueNm * TargetHp / e.Horsepower:N0} Nm Drehmoment · Faktor ×{(double)TargetHp / e.Horsepower:0.0} gegenüber {e.Horsepower} PS"
        : "Wähle einen Basis-Motor";

    public string BaseHpText => SelectedEngine is { } e
        ? $"{e.Horsepower.ToString("N0", German)} PS · {e.TorqueNm.ToString("N0", German)} Nm"
        : "–";

    public string NewHpText => SelectedEngine is { Horsepower: > 0 } e
        ? $"{TargetHp.ToString("N0", German)} PS · {(e.TorqueNm * TargetHp / e.Horsepower).ToString("N0", German)} Nm"
        : "–";

    public double BaseRatio => SelectedEngine is { Horsepower: > 0 } e ? (double)e.Horsepower / Math.Max(e.Horsepower, TargetHp) : 0;

    public double NewRatio => SelectedEngine is { Horsepower: > 0 } e ? (double)TargetHp / Math.Max(e.Horsepower, TargetHp) : 0;

    public string ModState { get => _modState; private set => Set(ref _modState, value); }

    /// <summary>Preselects the truck of the loaded save.</summary>
    public void OnSaveLoaded()
    {
        if (_save.Loaded?.Game is not { } game || game.PlayerTruck() is not { } truck || game.TruckModelOf(truck) is not { } model)
        {
            return;
        }

        if (IsReady && TruckModels.Contains(model))
        {
            SelectedTruck = model;
        }
        else
        {
            _selectedTruck = model;
        }
    }

    public void Dispose() => _vfs?.Dispose();

    private void RaiseComparison()
    {
        Raise(nameof(TorquePreview));
        Raise(nameof(BaseHpText));
        Raise(nameof(NewHpText));
        Raise(nameof(BaseRatio));
        Raise(nameof(NewRatio));
    }

    private bool CanGenerate() => IsReady && SelectedTruck is not null && SelectedEngine is not null;

    private async Task InitializeAsync()
    {
        var gameDir = _main.GameDirectory ?? throw new InvalidOperationException("ETS2-Installation nicht gefunden.");
        _main.Busy("Lese Spielarchive …");
        var (vfs, trucks) = await Task.Run(() =>
        {
            var fs = GameFileSystem.Mount(gameDir);
            return (fs, fs.ListDirectories("/def/vehicle/truck"));
        });
        _vfs = vfs;
        TruckModels.Clear();
        foreach (var t in trucks)
        {
            TruckModels.Add(t);
        }

        IsReady = true;
        var preselect = _selectedTruck is { } s && trucks.Contains(s) ? s : trucks.FirstOrDefault();
        _selectedTruck = null;
        SelectedTruck = preselect;
        RefreshGenerated();
        _main.Toast($"{trucks.Count} Truck-Modelle gefunden");
    }

    private void LoadEngines()
    {
        Engines.Clear();
        if (_vfs is null || _selectedTruck is null)
        {
            return;
        }

        foreach (var engine in EngineModBuilder.ListEngines(_vfs, _selectedTruck).Where(e => !e.IsTrainerEngine))
        {
            Engines.Add(engine);
        }

        SelectedEngine = Engines.LastOrDefault();
    }

    private async Task GenerateAsync(bool install)
    {
        var vfs = _vfs!;
        var truck = SelectedTruck!;
        var baseEngine = SelectedEngine!;
        var hp = TargetHp;
        if (install && _save.Loaded?.Game is { } g && g.PlayerTruck() is { } t && g.TruckModelOf(t) != truck)
        {
            throw new InvalidOperationException(
                $"Dein Truck im Spielstand ist ein „{g.TruckModelOf(t)}“ – wähle oben dieses Modell.");
        }

        var engine = await Task.Run(() =>
        {
            var generated = EngineModBuilder.Build(vfs, truck, baseEngine, hp);
            TrainerModPackage.AddOrReplace(generated);
            return generated;
        });
        RefreshGenerated();

        if (install && _save.Loaded is { } loaded && loaded.Game.PlayerTruck() is { } playerTruck)
        {
            loaded.Game.SetEngine(playerTruck, engine.DataPath);
            SaveSlotService.AddModDependency(loaded.Info, TrainerModPackage.PackageName, TrainerModPackage.DisplayName);
            _save.NotifyExternalEdit($"Motor mit {engine.Horsepower} PS in deinen Truck eingebaut");
            return;
        }

        _main.Toast($"Motor mit {engine.Horsepower} PS erzeugt – im Spiel beim Händler/Werkstatt für 1 € wählbar");
    }

    private void ActivateMod()
    {
        var changed = TrainerModPackage.ActivateInProfile(_save.SelectedProfile!);
        RefreshGenerated();
        _main.Toast(changed ? "Mod im Profil aktiviert" : "Mod war bereits aktiv");
    }

    private void RefreshGenerated()
    {
        GeneratedEngines.Clear();
        foreach (var path in TrainerModPackage.ListEngines())
        {
            GeneratedEngines.Add(path.Replace("/def/vehicle/truck/", string.Empty, StringComparison.Ordinal));
        }

        var active = _save.SelectedProfile is { } p && TrainerModPackage.IsActiveInProfile(p);
        ModState = GeneratedEngines.Count == 0
            ? "Noch keine Motoren erzeugt."
            : active
                ? $"Mod „{TrainerModPackage.DisplayName}“ ist im Profil aktiv."
                : "Mod ist noch NICHT aktiv – „Mod aktivieren“ klicken (Spiel geschlossen) oder im Mod-Manager einschalten.";
    }
}
