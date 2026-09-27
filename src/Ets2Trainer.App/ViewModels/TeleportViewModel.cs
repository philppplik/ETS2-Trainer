using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Input;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Bridge;
using Ets2Trainer.Core.Game;

namespace Ets2Trainer.App.ViewModels;

/// <summary>
/// Teleport + recovery. Targets: learned companies (recorded at job start/delivery), bookmarks,
/// position codes of friends and the plugin's breadcrumb trail ("back to the road").
/// </summary>
public sealed class TeleportViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private readonly PlacesStore _store = new();
    private bool _prepareMotion;
    private Place? _selectedPlace;
    private string _bookmarkName = "";
    private string _pastedCode = "";
    private uint _lastStarted;
    private uint _lastDelivered;
    private bool _countersInitialized;
    private bool _awaitingPickup;
    private string _jobCompany = "";
    private string _jobCity = "";

    public TeleportViewModel(MainViewModel main)
    {
        _main = main;
        _prepareMotion = main.Settings.PrepareMotion;
        UnflipCommand = new RelayCommand(() => SendRecovery(BridgeCommand.Unflip, "Aufstellen angefordert"), () => IsLive);
        ReturnCommand = new RelayCommand(() => SendRecovery(BridgeCommand.ReturnToRoad, "Zurück auf die Straße angefordert"), () => IsLive);
        JobTargetCommand = new RelayCommand(TeleportToJobTarget, () => IsLive && JobTargetPlace is not null);
        TeleportPlaceCommand = new RelayCommand(() => TeleportTo(SelectedPlace!), () => IsLive && SelectedPlace is not null);
        DeletePlaceCommand = new RelayCommand(DeleteSelected, () => SelectedPlace is not null);
        AddBookmarkCommand = new RelayCommand(AddBookmark, () => _main.Live.Telemetry is { Flags: var f } && f.HasFlag(TelemetryFlags.HasTruck));
        CopyCodeCommand = new RelayCommand(CopyCode, () => MyCode.Length > 0);
        TeleportCodeCommand = new RelayCommand(TeleportToCode, () => IsLive && PositionCode.TryDecode(PastedCode, out _, out _, out _, out _));
        RefreshPlaces();
    }

    public ObservableCollection<Place> Places { get; } = new();

    public ICommand UnflipCommand { get; }

    public ICommand ReturnCommand { get; }

    public ICommand JobTargetCommand { get; }

    public ICommand TeleportPlaceCommand { get; }

    public ICommand DeletePlaceCommand { get; }

    public ICommand AddBookmarkCommand { get; }

    public ICommand CopyCodeCommand { get; }

    public ICommand TeleportCodeCommand { get; }

    public bool IsLive => _main.Live.IsLive;

    /// <summary>Lets the plugin calibrate velocity, position and rotation in the background.</summary>
    public bool PrepareMotion
    {
        get => _prepareMotion;
        set
        {
            if (Set(ref _prepareMotion, value))
            {
                _main.UpdateSettings(s => s with { PrepareMotion = value });
            }
        }
    }

    public Place? SelectedPlace { get => _selectedPlace; set => Set(ref _selectedPlace, value); }

    public string BookmarkName { get => _bookmarkName; set => Set(ref _bookmarkName, value); }

    public string PastedCode { get => _pastedCode; set => Set(ref _pastedCode, value); }

    public string MyCode => _main.Live.Telemetry is { Flags: var f } t && f.HasFlag(TelemetryFlags.HasTruck)
        ? PositionCode.Encode(t.PosX, t.PosY, t.PosZ, t.Heading)
        : "";

    public FeatureChip VelocityChip => FeatureChip.From(_main.Live.Status?.Velocity, PrepareMotion);

    public FeatureChip PositionChip => FeatureChip.From(_main.Live.Status?.Position, PrepareMotion);

    public FeatureChip OrientationChip => FeatureChip.From(_main.Live.Status?.Orientation, PrepareMotion);

    public string BreadcrumbText => _main.Live.Status is { } s
        ? $"{s.BreadcrumbCount} sichere Punkte auf deiner Strecke gemerkt"
        : "Noch keine sicheren Punkte";

    public Place? JobTargetPlace => _jobCompany.Length > 0 ? _store.FindCompany(_jobCompany, _jobCity) : null;

    public string JobTargetText => _jobCompany.Length == 0
        ? "Kein aktiver Auftrag"
        : JobTargetPlace is not null
            ? $"Ziel bekannt: {PlacesStore.CompanyLabel(_jobCompany, _jobCity)}"
            : $"{PlacesStore.CompanyLabel(_jobCompany, _jobCity)} – noch unbekannt (wird bei der ersten Lieferung gelernt)";

    public string PlacesSummary => $"{Places.Count(p => p.Kind == PlaceKind.Company)} Firmen gelernt · {Places.Count(p => p.Kind == PlaceKind.Bookmark)} Lesezeichen";

    /// <summary>Called ~10x per second after the live tick.</summary>
    public void Tick()
    {
        LearnFromJobs();
        RaiseAll();
    }

    private void LearnFromJobs()
    {
        if (_main.Live.Status is not { } status || _main.Live.Telemetry is not { } tel)
        {
            return;
        }

        if (tel.HasJob && tel.DestinationCompanyId.Length > 0)
        {
            _jobCompany = tel.DestinationCompanyId;
            _jobCity = tel.DestinationCityId;
        }

        if (!_countersInitialized || status.JobStartedCount < _lastStarted || status.JobDeliveredCount < _lastDelivered)
        {
            (_lastStarted, _lastDelivered, _countersInitialized) = (status.JobStartedCount, status.JobDeliveredCount, true);
            return;  // first contact or plugin restarted: nothing to learn from old counters
        }

        if (status.JobStartedCount != _lastStarted)
        {
            _lastStarted = status.JobStartedCount;
            _awaitingPickup = true;
        }

        // Freight-market jobs start anywhere; the truck is only at the source company once the trailer
        // is hitched there. Quick jobs and cargo-market jobs start hitched, so they are learned at once.
        if (!tel.HasJob)
        {
            _awaitingPickup = false;
        }
        else if (_awaitingPickup && tel.Flags.HasFlag(TelemetryFlags.TrailerAttached))
        {
            _awaitingPickup = false;
            Learn(tel.SourceCompanyId, tel.SourceCityId, tel);
        }

        if (status.JobDeliveredCount != _lastDelivered)
        {
            _lastDelivered = status.JobDeliveredCount;
            Learn(_jobCompany, _jobCity, tel);
            _jobCompany = _jobCity = "";
        }
    }

    private void Learn(string companyId, string cityId, TelemetrySnapshot tel)
    {
        if (companyId.Length == 0 || cityId.Length == 0)
        {
            return;
        }

        var name = PlacesStore.CompanyLabel(companyId, cityId);
        _store.Upsert(new Place(name, PlaceKind.Company, companyId, cityId, tel.PosX, tel.PosY, tel.PosZ, tel.Heading, DateTime.UtcNow));
        RefreshPlaces();
        _main.Toast($"Ort gelernt: {name}");
    }

    private void SendRecovery(BridgeCommand command, string message)
    {
        if (!PrepareMotion)
        {
            PrepareMotion = true;  // recovery needs position + rotation
            message += " – Kalibrierung gestartet, kurz fahren/Kurve";
        }

        _main.Live.Send(command);
        _main.Toast(message);
    }

    private void TeleportTo(Place place) => Teleport(place.X, place.Y, place.Z, place.Heading, place.Name);

    private void Teleport(double x, double y, double z, float heading, string label)
    {
        PrepareMotion = true;
        _main.Live.Send(BridgeCommand.Teleport, x, y, z, heading, 1);
        _main.Toast($"Teleport zu „{label}“ angefordert");
    }

    private void TeleportToJobTarget()
    {
        if (JobTargetPlace is { } place)
        {
            TeleportTo(place);
        }
    }

    private void TeleportToCode()
    {
        if (PositionCode.TryDecode(PastedCode, out var x, out var y, out var z, out var heading))
        {
            Teleport(x, y, z, heading, "Mitspieler");
        }
    }

    private void CopyCode()
    {
        Clipboard.SetText(MyCode);
        _main.Toast("Positions-Code kopiert – an deinen Mitspieler schicken (z. B. Discord)");
    }

    private void AddBookmark()
    {
        if (_main.Live.Telemetry is not { } t)
        {
            return;
        }

        var name = BookmarkName.Trim().Length > 0 ? BookmarkName.Trim() : $"Punkt {DateTime.Now:HH:mm:ss}";
        _store.Upsert(new Place(name, PlaceKind.Bookmark, "", "", t.PosX, t.PosY, t.PosZ, t.Heading, DateTime.UtcNow));
        BookmarkName = "";
        RefreshPlaces();
        _main.Toast($"Lesezeichen „{name}“ gespeichert");
    }

    private void DeleteSelected()
    {
        _store.Remove(SelectedPlace!);
        RefreshPlaces();
    }

    private void RefreshPlaces()
    {
        Places.Clear();
        foreach (var place in _store.All)
        {
            Places.Add(place);
        }
    }
}
