using System.Globalization;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Save;

namespace Ets2Trainer.Tests;

public static class PositionCodeTests
{
    [Test]
    public static void RoundTrip_KeepsCoordinatesAndHeading()
    {
        var code = PositionCode.Encode(-12345.678, 42.5, 98765.4321, 0.25f);

        Assert.True(PositionCode.TryDecode(code, out var x, out var y, out var z, out var heading), "decodes");
        Assert.True(Math.Abs(x + 12345.68) < 0.01 && Math.Abs(y - 42.5) < 0.01 && Math.Abs(z - 98765.43) < 0.01, "coordinates");
        Assert.True(Math.Abs(heading - 0.25f) < 0.0001f, "heading");
    }

    [Test]
    public static void Encode_IsCultureInvariant()
    {
        var previous = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
            var code = PositionCode.Encode(1.5, 2.5, 3.5, 0.5f);
            Assert.Equal("ETS2T:1.50;2.50;3.50;0.5000", code, "dot decimals under German culture");
            Assert.True(PositionCode.TryDecode(code, out _, out _, out _, out _), "decodes under German culture");
        }
        finally
        {
            CultureInfo.CurrentCulture = previous;
        }
    }

    [Test]
    public static void TryDecode_RejectsGarbage()
    {
        string?[] bad = { null, "", "hello", "ETS2T:1;2;3", "ETS2T:a;b;c;d", "ETS2T:NaN;0;0;0", "ETS2T:1e9;0;0;0", "XYZ:1;2;3;4" };
        foreach (var text in bad)
        {
            Assert.True(!PositionCode.TryDecode(text, out _, out _, out _, out _), $"rejects '{text}'");
        }

        Assert.True(PositionCode.TryDecode("  ets2t:1;2;3;0.5 ", out _, out _, out _, out _), "trims and ignores prefix case");
    }
}

public static class PlacesStoreTests
{
    private static Place Company(double x) =>
        new("Tree Et · Felixstowe", PlaceKind.Company, "tree_et", "felixstowe", x, 10, 20, 0.5f, DateTime.UtcNow);

    [Test]
    public static void Upsert_ReplacesSameCompanyAndPersists()
    {
        var path = Path.Combine(Path.GetTempPath(), "e2t_places_" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            var store = new PlacesStore(path);
            store.Upsert(Company(1));
            store.Upsert(Company(2));
            store.Upsert(new Place("Rastplatz", PlaceKind.Bookmark, "", "", 5, 6, 7, 0f, DateTime.UtcNow));

            var reloaded = new PlacesStore(path);
            Assert.Equal(2, reloaded.All.Count, "one company + one bookmark");
            Assert.Equal(2d, reloaded.FindCompany("tree_et", "felixstowe")?.X ?? -1, "newest company position wins");
            Assert.Equal(PlaceKind.Bookmark, reloaded.All[0].Kind, "bookmarks sorted first");

            reloaded.Remove(reloaded.All[0]);
            Assert.Equal(1, new PlacesStore(path).All.Count, "remove persisted");
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Test]
    public static void CorruptFile_StartsEmpty()
    {
        var path = Path.Combine(Path.GetTempPath(), "e2t_places_" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            File.WriteAllText(path, "{ not json");
            Assert.Equal(0, new PlacesStore(path).All.Count, "damaged file ignored");
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Test]
    public static void CompanyLabel_IsReadable() =>
        Assert.Equal("Tree Et · Felixstowe", PlacesStore.CompanyLabel("tree_et", "felixstowe"), "title case without underscores");
}

public static class CloudSaveTests
{
    [Test]
    public static void RemoteName_MatchesPluginWhitelist()
    {
        var profile = new ProfileInfo(Path.Combine("C:", "Steam", "userdata", "1", "227300", "remote", "profiles", "4A6F686E"), "John", true);

        var remote = CloudSaveStager.RemoteName(profile, "7", "game.sii");

        // cloud_storage.cpp only accepts "profiles/…/save/….sii" without backslashes or "..".
        Assert.Equal("profiles/4A6F686E/save/7/game.sii", remote, "remote layout");
    }

    [Test]
    public static void PrepareForWrite_PicksNextSlotWithoutTouchingDisk()
    {
        var source = TestData.NewestGameSii() ?? throw new SkipException("kein Spielstand gefunden");
        var temp = Path.Combine(Path.GetTempPath(), "e2t_test_" + Guid.NewGuid().ToString("N"));
        var slotDir = Path.Combine(temp, "save", "5");
        try
        {
            Directory.CreateDirectory(slotDir);
            File.Copy(source, Path.Combine(slotDir, "game.sii"));
            var infoSource = Path.Combine(Path.GetDirectoryName(source)!, "info.sii");
            if (File.Exists(infoSource))
            {
                File.Copy(infoSource, Path.Combine(slotDir, "info.sii"));
            }

            var loaded = SaveSlotService.Load(SaveSlotService.ListSlots(new ProfileInfo(temp, "Test", true)).Single());

            Assert.Equal("6", SaveSlotService.PrepareForWrite(loaded, SaveWriteMode.NewSlot), "next numbered slot");
            Assert.True(!Directory.Exists(Path.Combine(temp, "save", "6")), "no folder created (plugin writes via Steam)");
            Assert.Equal("5", SaveSlotService.PrepareForWrite(loaded, SaveWriteMode.OverwriteWithBackup), "overwrite keeps slot");
        }
        finally
        {
            Directory.Delete(temp, recursive: true);
        }
    }
}
