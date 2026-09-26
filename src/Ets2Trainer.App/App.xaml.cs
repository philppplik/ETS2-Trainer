using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Ets2Trainer.App.ViewModels;

namespace Ets2Trainer.App;

public partial class App : Application
{
    private static readonly TimeSpan ScreenshotSettleTime = TimeSpan.FromMilliseconds(600);
    private static readonly TimeSpan SaveLoadTimeout = TimeSpan.FromSeconds(30);

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(args.Exception.Message, "ETS2 Trainer – unerwarteter Fehler", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

        var window = new MainWindow();
        MainWindow = window;

        var screenshotIndex = Array.IndexOf(e.Args, "--screenshot");
        if (screenshotIndex >= 0 && screenshotIndex + 1 < e.Args.Length)
        {
            _ = CaptureScreenshotsAsync(window, e.Args[screenshotIndex + 1], e.Args.Contains("--load"));
            return;
        }

        window.Show();
    }

    /// <summary>Developer aid: renders every section to PNG (read-only; never saves game files) and exits.</summary>
    private static async Task CaptureScreenshotsAsync(MainWindow window, string directory, bool loadSave)
    {
        Directory.CreateDirectory(directory);
        window.WindowStartupLocation = WindowStartupLocation.Manual;
        window.Left = -30000;
        window.Top = 0;
        window.ShowActivated = false;
        window.Show();
        var vm = (MainViewModel)window.DataContext;

        if (loadSave && vm.Save.LoadCommand.CanExecute(null))
        {
            vm.Save.LoadCommand.Execute(null);
            var deadline = DateTime.UtcNow + SaveLoadTimeout;
            while (!vm.Save.IsLoaded && DateTime.UtcNow < deadline)
            {
                await Task.Delay(200);
            }
        }

        foreach (var section in Enum.GetValues<Section>())
        {
            vm.CurrentSection = section;
            await Task.Delay(section == Section.Workshop ? ScreenshotSettleTime * 8 : ScreenshotSettleTime);
            await window.Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
            SaveVisual(window, Path.Combine(directory, $"{(int)section}_{section}.png"));
        }

        window.Close();
    }

    private static void SaveVisual(Window window, string path)
    {
        var content = (FrameworkElement)window.Content;
        var dpi = VisualTreeHelper.GetDpi(window);
        var bitmap = new RenderTargetBitmap(
            (int)(content.ActualWidth * dpi.DpiScaleX), (int)(content.ActualHeight * dpi.DpiScaleY),
            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        bitmap.Render(content);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(path);
        encoder.Save(stream);
    }
}
