using System.Globalization;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Bridge;

namespace Ets2Trainer.App.ViewModels;

/// <summary>Live cheats through the in-game plugin (works in singleplayer and in official SCS convoys).</summary>
public sealed class LiveViewModel : ObservableObject, IDisposable
{
    private const float MinFactor = 1f;
    private const float MaxFactor = 10f;
    private const double RecalibrateAllMask = 15;

    private readonly BridgeClient _bridge = new();
    private readonly MainViewModel _main;
    private TelemetrySnapshot? _telemetry;
    private StatusSnapshot? _status;
    private uint _lastMenuToggle;
    private bool _menuToggleInitialized;
    private bool _infiniteFuel;
    private bool _noDamage;
    private bool _powerBoost;
    private float _powerFactor;
    private bool _nitroEnabled;
    private float _nitroAccel;
    private bool _speedCapEnabled;
    private float _speedCapKmh;

    public LiveViewModel(MainViewModel main)
    {
        _main = main;
        _powerFactor = Math.Clamp(main.Settings.PowerFactor, MinFactor, MaxFactor);
        _nitroAccel = main.Settings.NitroAccel;
        _speedCapKmh = main.Settings.SpeedCapKmh;

        RefuelCommand = new RelayCommand(() => Send(BridgeCommand.Refuel, "Volltanken angefordert"), () => IsLive);
        RepairCommand = new RelayCommand(() => Send(BridgeCommand.Repair, "Reparatur angefordert"), () => IsLive);
        StopCommand = new RelayCommand(() => Send(BridgeCommand.StopTruck, "Not-Stopp!"), () => IsLive);
        RecalibrateCommand = new RelayCommand(
            () => Send(BridgeCommand.Recalibrate, "Neu-Kalibrierung gestartet", RecalibrateAllMask), () => IsLive);
    }

    public event Action? MenuHotkeyPressed;

    public ICommand RefuelCommand { get; }

    public ICommand RepairCommand { get; }

    public ICommand StopCommand { get; }

    public ICommand RecalibrateCommand { get; }

    public bool IsLive => _bridge.IsPluginAlive;

    public bool IsBlocked => _telemetry?.Flags.HasFlag(TelemetryFlags.TruckersMpDetected) == true;

    public string ConnectionText => IsBlocked ? "TruckersMP erkannt – gesperrt"
        : IsLive ? $"Live · {TruckTitle}"
        : _bridge.IsConnected ? "Spiel pausiert / lädt …"
        : "Kein Spiel verbunden";

    public string TruckTitle => _telemetry is { TruckBrand.Length: > 0 } t ? $"{t.TruckBrand} {t.TruckName}".Trim() : "kein Truck";

    public string SpeedText => ((int)Math.Round(Math.Abs(_telemetry?.SpeedKmh ?? 0))).ToString(CultureInfo.InvariantCulture);

    public string GearText => _telemetry?.Gear switch
    {
        null => "–",
        0 => "N",
        < 0 and int reverse => $"R{-reverse}",
        int forward => forward.ToString(CultureInfo.InvariantCulture),
    };

    public double RpmRatio => _telemetry is { RpmMax: > 0 } t ? Math.Clamp(t.Rpm / t.RpmMax, 0, 1) : 0;

    public string RpmText => $"{_telemetry?.Rpm ?? 0:0} U/min";

    public double FuelRatio => _telemetry is { FuelCapacity: > 0 } t ? Math.Clamp(t.Fuel / t.FuelCapacity, 0, 1) : 0;

    public string FuelText => _telemetry is { } t ? $"{t.Fuel:0} / {t.FuelCapacity:0} L" : "– L";

    public double DamageRatio => Math.Clamp(_telemetry?.WorstWear ?? 0, 0, 1);

    public string DamageText => $"{DamageRatio * 100:0.0} %";

    public string SpeedLimitText => _telemetry is { SpeedLimitMs: > 0 } t ? $"{t.SpeedLimitKmh:0}" : "–";

    public string JobText => _telemetry is { Cargo.Length: > 0 } t ? $"{t.Cargo} → {t.DestinationCity}" : "Kein Auftrag";

    public string PluginMessage => _status?.Message is { Length: > 0 } m ? m
        : IsLive ? "Bereit. Schalte unten eine Funktion ein." : "Starte ETS2 mit installiertem Plugin (Tab SETUP).";

    public bool InfiniteFuel { get => _infiniteFuel; set => Set(ref _infiniteFuel, value); }

    public bool NoDamage { get => _noDamage; set => Set(ref _noDamage, value); }

    public bool PowerBoost { get => _powerBoost; set => Set(ref _powerBoost, value); }

    public float PowerFactor
    {
        get => _powerFactor;
        set
        {
            if (Set(ref _powerFactor, (float)Math.Round(Math.Clamp(value, MinFactor, MaxFactor), 1)))
            {
                Raise(nameof(PowerFactorText));
                _main.UpdateSettings(s => s with { PowerFactor = _powerFactor });
            }
        }
    }

    public string PowerFactorText => $"×{PowerFactor.ToString("0.0", CultureInfo.InvariantCulture)}";

    public bool NitroEnabled { get => _nitroEnabled; set => Set(ref _nitroEnabled, value); }

    public float NitroAccel
    {
        get => _nitroAccel;
        set
        {
            if (Set(ref _nitroAccel, (float)Math.Round(value, 1)))
            {
                Raise(nameof(NitroText));
                _main.UpdateSettings(s => s with { NitroAccel = _nitroAccel });
            }
        }
    }

    public string NitroText => $"+{NitroAccel:0.0} m/s²  ·  Taste halten: {KeyNames.Name(_main.Settings.NitroVk)}";

    public bool SpeedCapEnabled { get => _speedCapEnabled; set => Set(ref _speedCapEnabled, value); }

    public float SpeedCapKmh
    {
        get => _speedCapKmh;
        set
        {
            if (Set(ref _speedCapKmh, (float)Math.Round(value)))
            {
                Raise(nameof(SpeedCapText));
                _main.UpdateSettings(s => s with { SpeedCapKmh = _speedCapKmh });
            }
        }
    }

    public string SpeedCapText => $"{SpeedCapKmh:0} km/h";

    public FeatureChip FuelChip => FeatureChip.From(_status?.Fuel, InfiniteFuel);

    public FeatureChip WearChip => FeatureChip.From(_status?.Wear, NoDamage);

    public FeatureChip BoostChip => FeatureChip.From(_status?.Velocity, PowerBoost || NitroEnabled || SpeedCapEnabled);

    /// <summary>Called ~10x per second by the main timer.</summary>
    public void Tick()
    {
        if (!_bridge.IsConnected && !_bridge.TryConnect())
        {
            _telemetry = null;
            _status = null;
            RaiseAll();
            return;
        }

        _telemetry = _bridge.ReadTelemetry();
        _status = _bridge.ReadStatus();
        _bridge.WriteControl(new ControlState(
            InfiniteFuel, NoDamage, PowerBoost, PowerFactor, NitroEnabled, _main.Settings.NitroVk, NitroAccel,
            SpeedCapEnabled, SpeedCapKmh, _main.Settings.MenuHotkeyVk));
        DetectMenuHotkey();
        RaiseAll();
    }

    public void Dispose() => _bridge.Dispose();

    private void DetectMenuHotkey()
    {
        if (_status is null)
        {
            return;
        }

        if (_menuToggleInitialized && _status.MenuToggleCount != _lastMenuToggle)
        {
            MenuHotkeyPressed?.Invoke();
        }

        _lastMenuToggle = _status.MenuToggleCount;
        _menuToggleInitialized = true;
    }

    private void Send(BridgeCommand command, string message, params double[] args)
    {
        _bridge.SendCommand(command, args);
        _main.Toast(message);
    }
}

/// <summary>Status chip shown on every live-feature card. Kind: idle | busy | ok | error.</summary>
public sealed record FeatureChip(string Text, string Kind)
{
    public static FeatureChip From(FeatureStatus? status, bool requested)
    {
        if (!requested || status is null)
        {
            return new FeatureChip("AUS", "idle");
        }

        return status.State switch
        {
            FeatureState.Active => new FeatureChip(status.Confirmed > 1 ? $"AKTIV · {status.Confirmed}" : "AKTIV", "ok"),
            FeatureState.WaitingForData => new FeatureChip("WARTET AUF FAHRT", "busy"),
            FeatureState.Calibrating => new FeatureChip($"KALIBRIERT · {status.Candidates}", "busy"),
            FeatureState.Verifying => new FeatureChip("PRÜFT", "busy"),
            FeatureState.Failed => new FeatureChip("FEHLGESCHLAGEN", "error"),
            FeatureState.Blocked => new FeatureChip("GESPERRT", "error"),
            _ => new FeatureChip("STARTET", "busy"),
        };
    }
}
