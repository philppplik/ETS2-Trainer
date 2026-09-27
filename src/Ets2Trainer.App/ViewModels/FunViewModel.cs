using System.Globalization;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Bridge;

namespace Ets2Trainer.App.ViewModels;

/// <summary>Tricks that the rest of a convoy can see: stunts, physics toys and the light/horn show.</summary>
public sealed class FunViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private bool _moonOn;
    private bool _anchorOn;
    private bool _spinOn;
    private bool _autoUprightOn;
    private bool _discoOn;
    private bool _hornOn;
    private bool _lowriderOn;
    private float _moonGravity;
    private float _spinSpeed;

    public FunViewModel(MainViewModel main)
    {
        _main = main;
        _moonGravity = main.Settings.MoonGravity;
        _spinSpeed = main.Settings.SpinSpeed;
        JumpCommand = new RelayCommand(() => Trick(BridgeCommand.Jump, "Sprung"), () => IsLive);
        RocketCommand = new RelayCommand(() => Trick(BridgeCommand.Rocket, "Rakete"), () => IsLive);
        RollCommand = new RelayCommand(() => Trick(BridgeCommand.BarrelRoll, "Fassrolle"), () => IsLive);
    }

    public ICommand JumpCommand { get; }

    public ICommand RocketCommand { get; }

    public ICommand RollCommand { get; }

    public bool IsLive => _main.Live.IsLive;

    public TeleportViewModel Motion => _main.Teleport;

    public bool MoonOn { get => _moonOn; set => Set(ref _moonOn, value); }

    public bool AnchorOn { get => _anchorOn; set => Set(ref _anchorOn, value); }

    public bool SpinOn { get => _spinOn; set => Set(ref _spinOn, value); }

    public bool AutoUprightOn { get => _autoUprightOn; set => Set(ref _autoUprightOn, value); }

    public bool DiscoOn { get => _discoOn; set => Set(ref _discoOn, value); }

    public bool HornOn { get => _hornOn; set => Set(ref _hornOn, value); }

    public bool LowriderOn { get => _lowriderOn; set => Set(ref _lowriderOn, value); }

    public float MoonGravity
    {
        get => _moonGravity;
        set
        {
            if (Set(ref _moonGravity, (float)Math.Round(Math.Clamp(value, 0f, 1f), 2)))
            {
                Raise(nameof(MoonText));
                _main.UpdateSettings(s => s with { MoonGravity = _moonGravity });
            }
        }
    }

    public string MoonText => $"{(1 - MoonGravity) * 100:0} % Schwerkraft";

    public float SpinSpeed
    {
        get => _spinSpeed;
        set
        {
            if (Set(ref _spinSpeed, (float)Math.Round(Math.Clamp(value, -3f, 3f), 1)))
            {
                Raise(nameof(SpinText));
                _main.UpdateSettings(s => s with { SpinSpeed = _spinSpeed });
            }
        }
    }

    public string SpinText => $"{SpinSpeed.ToString("0.0", CultureInfo.InvariantCulture)} Umdrehungen/s";

    public FunFlags Flags =>
        (MoonOn ? FunFlags.MoonGravity : 0) | (AnchorOn ? FunFlags.Anchor : 0) | (SpinOn ? FunFlags.Spin : 0) |
        (AutoUprightOn ? FunFlags.AutoUpright : 0) | (DiscoOn ? FunFlags.Disco : 0) | (HornOn ? FunFlags.Horn : 0) |
        (LowriderOn ? FunFlags.Lowrider : 0);

    public bool InputAvailable => _main.Live.Status?.Capabilities.HasFlag(Capabilities.InputDevice) == true;

    public string InputText => InputAvailable
        ? "Eingabegerät aktiv – Licht, Hupe und Federung steuerbar"
        : "Eingabegerät noch nicht aktiv (ETS2 mit aktuellem Plugin neu starten)";

    public string KeyText =>
        $"{KeyNames.Name(_main.Settings.JumpVk)} Sprung  ·  {KeyNames.Name(_main.Settings.RocketVk)} Rakete  ·  " +
        $"{KeyNames.Name(_main.Settings.RollVk)} Fassrolle  ·  {KeyNames.Name(_main.Settings.HoverVk)} halten = Schweben  ·  " +
        $"{KeyNames.Name(_main.Settings.UnflipVk)} Aufstellen";

    public void Tick() => RaiseAll();

    private void Trick(BridgeCommand command, string label)
    {
        if (!Motion.PrepareMotion)
        {
            Motion.PrepareMotion = true;
            _main.Toast($"{label}: Kalibrierung gestartet – kurz fahren, dann nochmal");
            return;
        }

        _main.Live.Send(command);
    }
}
