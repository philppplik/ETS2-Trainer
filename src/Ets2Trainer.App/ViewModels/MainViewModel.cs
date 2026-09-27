using System.Windows.Input;
using System.Windows.Threading;
using Ets2Trainer.App.Infrastructure;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.App.ViewModels;

public enum Section
{
    Live,
    Teleport,
    Fun,
    Company,
    Fleet,
    Workshop,
    Setup,
}

/// <summary>Root view model: navigation, shared services, toasts and the 10 Hz live tick.</summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private const int GameCheckEveryTicks = 20;
    private static readonly TimeSpan TickInterval = TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan ToastDuration = TimeSpan.FromSeconds(6);

    private readonly DispatcherTimer _timer;
    private readonly DispatcherTimer _toastTimer;
    private AppSettings _settings;
    private Section _section = Section.Live;
    private string _toast = "";
    private bool _toastIsError;
    private bool _gameRunning;
    private int _tickCount;

    public MainViewModel(bool offline = false)
    {
        _settings = AppSettings.Load();
        GameDirectory = GamePaths.FindGameDirectory();
        Live = new LiveViewModel(this, offline);
        Teleport = new TeleportViewModel(this);
        Fun = new FunViewModel(this);
        Save = new SaveEditorViewModel(this);
        Workshop = new WorkshopViewModel(this, Save);
        Setup = new SetupViewModel(this);
        NavigateCommand = new RelayCommand(p =>
            CurrentSection = Enum.TryParse<Section>(p?.ToString(), out var s) ? s : Section.Live);
        DismissToastCommand = new RelayCommand(() => ToastText = "");

        _toastTimer = new DispatcherTimer { Interval = ToastDuration };
        _toastTimer.Tick += (_, _) =>
        {
            _toastTimer.Stop();
            ToastText = "";
        };
        _timer = new DispatcherTimer(TickInterval, DispatcherPriority.Background, (_, _) => OnTick(),
            Dispatcher.CurrentDispatcher);
        _timer.Start();
    }

    public event Action? HotkeyChanged;

    public event Action? SettingsChanged;

    public LiveViewModel Live { get; }

    public TeleportViewModel Teleport { get; }

    public FunViewModel Fun { get; }

    public SaveEditorViewModel Save { get; }

    public WorkshopViewModel Workshop { get; }

    public SetupViewModel Setup { get; }

    public ICommand NavigateCommand { get; }

    public ICommand DismissToastCommand { get; }

    public AppSettings Settings => _settings;

    public string? GameDirectory { get; }

    public Section CurrentSection
    {
        get => _section;
        set
        {
            if (!Set(ref _section, value))
            {
                return;
            }

            if (value == Section.Setup)
            {
                Setup.Refresh();
            }

            if (value == Section.Workshop && !Workshop.IsReady && Workshop.InitializeCommand.CanExecute(null))
            {
                Workshop.InitializeCommand.Execute(null);
            }
        }
    }

    public string ToastText
    {
        get => _toast;
        private set
        {
            if (Set(ref _toast, value))
            {
                Raise(nameof(HasToast));
            }
        }
    }

    public bool HasToast => _toast.Length > 0;

    public bool ToastIsError { get => _toastIsError; private set => Set(ref _toastIsError, value); }

    public bool GameRunning { get => _gameRunning; private set => Set(ref _gameRunning, value); }

    public string MenuHotkeyText => KeyNames.Name(_settings.MenuHotkeyVk);

    public void UpdateSettings(Func<AppSettings, AppSettings> change)
    {
        _settings = change(_settings);
        _settings.Save();
        Raise(nameof(MenuHotkeyText));
        SettingsChanged?.Invoke();
    }

    public void ReRegisterHotkey() => HotkeyChanged?.Invoke();

    public void Toast(string message) => ShowToast(message, isError: false);

    public void Busy(string message) => ShowToast(message, isError: false);

    public void ShowError(Exception ex) => ShowToast("Fehler: " + ex.Message, isError: true);

    public void ShowError(string message) => ShowToast(message, isError: true);

    public void OnSaveLoaded() => Workshop.OnSaveLoaded();

    public void Dispose()
    {
        _timer.Stop();
        Live.Dispose();
        Workshop.Dispose();
    }

    private void ShowToast(string message, bool isError)
    {
        ToastIsError = isError;
        ToastText = message;
        _toastTimer.Stop();
        _toastTimer.Start();
    }

    private void OnTick()
    {
        Live.Tick();
        Teleport.Tick();
        Fun.Tick();
        if (_tickCount++ % GameCheckEveryTicks == 0)
        {
            GameRunning = SaveSlotService.IsGameRunning();
        }
    }
}
