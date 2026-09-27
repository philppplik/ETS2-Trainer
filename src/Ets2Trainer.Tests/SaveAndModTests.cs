using System.IO.MemoryMappedFiles;
using Ets2Trainer.Core.Bridge;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.HashFs;
using Ets2Trainer.Core.Mods;
using Ets2Trainer.Core.Save;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Tests;

public static class SaveGameTests
{
    private static SaveGame LoadRealSave()
    {
        var path = TestData.NewestGameSii() ?? throw new SkipException("kein Spielstand gefunden");
        return new SaveGame(SiiFile.LoadFile(path));
    }

    [Test]
    public static void MoneyXpAndSkills_AreEditableAndClamped()
    {
        var save = LoadRealSave();
        save.Money = 123_456_789;
        save.ExperiencePoints = long.MaxValue;
        save.Adr = 99;
        save.SetSkill("heavy", 42);

        Assert.Equal(123_456_789L, save.Money, "money");
        Assert.Equal((long)uint.MaxValue, save.ExperiencePoints, "xp clamped to u32");
        Assert.Equal(SaveGame.MaxAdr, save.Adr, "adr clamped");
        Assert.Equal(SaveGame.MaxSkill, save.GetSkill("heavy"), "skill clamped");
        Assert.Throws<ArgumentException>(() => save.SetSkill("flying", 1), "unknown skill rejected");
    }

    [Test]
    public static void Garages_BuyAndUpgradeMakesAllLarge()
    {
        var save = LoadRealSave();
        save.BuyAndUpgradeAllGarages();
        var garages = save.Garages();
        Assert.True(garages.Count > 0, "garages present");
        Assert.True(garages.All(g => g.Status == SaveGame.GarageStatusLarge && g.Slots >= SaveGame.LargeGarageSlots),
            "all garages large with 5 slots");
    }

    [Test]
    public static void Fleet_RepairAndRefuelZeroesWear()
    {
        var save = LoadRealSave();
        save.RepairAndRefuelFleet();
        var trucks = save.Trucks();
        Assert.True(trucks.All(t => t.WorstWear == 0f && t.Fuel == 1f), "all trucks repaired and full");
    }

    [Test]
    public static void Drivers_MaxAllSetsSkills()
    {
        var save = LoadRealSave();
        var count = save.MaxAllDrivers(1_000_000);
        var drivers = save.HiredDrivers();
        Assert.Equal(drivers.Count, count, "all hired drivers touched");
        Assert.True(drivers.All(d => d.Adr == SaveGame.MaxAdr && d.Heavy == SaveGame.MaxSkill && d.Experience >= 1_000_000),
            "skills and xp maxed");
    }

    [Test]
    public static void CleanPlate_StripsMarkupAndCountry()
    {
        Assert.Equal("B 78 HNO", SaveGame.CleanPlate("<offset hshift=-5>B 78 HNO|romania"), "markup + country");
        Assert.Equal("DU 42", SaveGame.CleanPlate("DU<offset hshift=2> 42|germany"), "inline markup");
        Assert.Equal("KGU 084", SaveGame.CleanPlate("KGU 084"), "plain");
    }

    [Test]
    public static void PlayerTruck_HasEngineAndModel()
    {
        var save = LoadRealSave();
        var truck = save.PlayerTruck() ?? throw new SkipException("kein aktiver Truck im Spielstand");
        Assert.True(save.EnginePathOf(truck).Contains("/engine/", StringComparison.Ordinal), "engine path");
        Assert.True(save.TruckModelOf(truck)?.Contains('.') == true, "truck model like brand.model");
    }

    [Test]
    public static void Write_NewSlotRoundTripsInTempFolder()
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

            var profile = new ProfileInfo(temp, "Test", false);
            var slot = SaveSlotService.ListSlots(profile).Single();
            var loaded = SaveSlotService.Load(slot);
            loaded.Game.Money = 987_654_321;
            var written = SaveSlotService.Write(loaded, SaveWriteMode.NewSlot);

            Assert.Equal("6", Path.GetFileName(written), "next numbered slot");
            var reloaded = SaveSlotService.Load(SaveSlotService.ListSlots(profile).First(s => s.FolderName == "6"));
            Assert.Equal(987_654_321L, reloaded.Game.Money, "money persisted");
            Assert.True(reloaded.Slot.Name.StartsWith(SaveSlotService.TrainerSaveMarker, StringComparison.Ordinal),
                "slot renamed with trainer marker");
            Assert.True(File.ReadAllBytes(Path.Combine(slotDir, "game.sii")).AsSpan().SequenceEqual(File.ReadAllBytes(source)),
                "original slot untouched");
        }
        finally
        {
            Directory.Delete(temp, recursive: true);
        }
    }
}

public static class EngineModTests
{
    private const string SampleEngine = """
        SiiNunit
        {
        accessory_engine_data : dc16_730.scania.s_2016.engine
        {
        	name: "@@engine_dc16@@"
        	price: 29100
        	unlock: 25
        	info[]: "730 @@hp@@ (537@@kw@@)"
        	info[]: "3@@dg@@500 @@nm@@"
        	info[]: "1@@dg@@000-1@@dg@@400 @@rpm@@"
        	torque: 3500
        	torque_curve[]: (300, 0.1)
        @include "sound_hi.sui"
        }
        }
        """;

    [Test]
    public static void Transform_ScalesTorqueAndKeepsIncludes()
    {
        var text = EngineModBuilder.Transform(SampleEngine, "e2t_2000", 2000, 9589, "ETS2 Trainer 2000 PS");
        var unit = SiiTextParser.Parse(text).FirstOfClass("accessory_engine_data")!;

        Assert.Equal("e2t_2000.scania.s_2016.engine", unit.Id, "unit renamed");
        Assert.Equal("9589", unit.Get("torque"), "torque");
        Assert.Equal("1", unit.Get("price"), "price");
        Assert.Equal("0", unit.Get("unlock"), "unlock");
        Assert.Equal(3, unit.GetArray("info").Count, "info lines incl. rpm");
        Assert.Equal(2000, EngineModBuilder.ParseHorsepower(unit.GetArray("info")), "hp info");
        Assert.True(text.Contains("@include \"sound_hi.sui\"", StringComparison.Ordinal), "sound include kept");
        Assert.True(text.Contains("torque_curve[]: (300, 0.1)", StringComparison.Ordinal), "curve kept");
    }

    [Test]
    public static void ParseHorsepower_HandlesDigitGroups()
    {
        Assert.Equal(1050, EngineModBuilder.ParseHorsepower(new[] { "\"1@@dg@@050 @@hp@@ (772@@kw@@)\"" }), "grouped");
        Assert.Equal(null, EngineModBuilder.ParseHorsepower(new[] { "\"3500 @@nm@@\"" }), "no hp");
    }

    [Test]
    public static void Build_FromRealGameEngine()
    {
        var game = GamePaths.FindGameDirectory() ?? throw new SkipException("ETS2 nicht installiert");
        using var vfs = GameFileSystem.Mount(game);
        var engines = EngineModBuilder.ListEngines(vfs, "scania.s_2016");
        Assert.True(engines.Count > 5 && engines.All(e => e.Horsepower > 100), "engines with hp listed");

        var baseEngine = engines[^1];
        var generated = EngineModBuilder.Build(vfs, "scania.s_2016", baseEngine, baseEngine.Horsepower * 2);
        var resolver = vfs.IncludeResolver(EngineModBuilder.EngineDirectory("scania.s_2016"));
        var unit = SiiTextParser.Parse(generated.DefinitionText, resolver).FirstOfClass("accessory_engine_data")!;

        Assert.Equal($"/def/vehicle/truck/scania.s_2016/engine/e2t_{baseEngine.Horsepower * 2}.sii", generated.DataPath, "path");
        Assert.True(Math.Abs(SiiValues.ParseFloat(unit.Get("torque")) - baseEngine.TorqueNm * 2) < 2, "torque doubled");
        Assert.True(unit.GetArray("sounds").Count > 0, "sounds resolved through include");
    }
}

public static class BridgeTests
{
    [Test]
    public static void Layout_MatchesCppStaticAsserts()
    {
        Assert.Equal(1152, BridgeProtocol.SharedSize, "shared size");
        Assert.Equal(BridgeProtocol.TelemetryOffset + BridgeProtocol.TelemetrySize, BridgeProtocol.StatusOffset, "status follows telemetry");
        Assert.Equal(BridgeProtocol.StatusOffset + BridgeProtocol.StatusSize, BridgeProtocol.ControlOffset, "control follows status");
        Assert.Equal(BridgeProtocol.ControlOffset + BridgeProtocol.ControlSize, BridgeProtocol.SharedSize, "control ends block");
        Assert.Equal(BridgeProtocol.TelemetrySize - 4, BridgeProtocol.Telemetry.SeqEnd, "seqEnd last telemetry field");
    }

    [Test]
    public static void Client_ReadsTelemetryAndWritesControl()
    {
        using var mapping = CreateFakePluginMapping();
        using var view = mapping.CreateViewAccessor(0, BridgeProtocol.SharedSize);
        const int t = BridgeProtocol.TelemetryOffset;
        view.Write(t + BridgeProtocol.Telemetry.Speed, 25f);
        view.Write(t + BridgeProtocol.Telemetry.Fuel, 412.5f);
        view.WriteArray(t + BridgeProtocol.Telemetry.TruckBrand, "Scania\0"u8.ToArray(), 0, 7);

        using var client = new BridgeClient();
        Assert.True(client.TryConnect(), "connects to plugin mapping");
        var telemetry = client.ReadTelemetry()!;
        Assert.Equal(90f, telemetry.SpeedKmh, "speed km/h");
        Assert.Equal(412.5f, telemetry.Fuel, "fuel");
        Assert.Equal("Scania", telemetry.TruckBrand, "brand string");

        client.WriteControl(new ControlState(InfiniteFuel: true, PowerFactor: 3.5f));
        const int c = BridgeProtocol.ControlOffset;
        Assert.Equal(1u, view.ReadUInt32(c + BridgeProtocol.Control.InfiniteFuel), "fuel toggle written");
        Assert.Equal(3.5f, view.ReadSingle(c + BridgeProtocol.Control.PowerFactor), "factor written");
        Assert.Equal(1u, view.ReadUInt32(c + BridgeProtocol.Control.AppHeartbeat), "heartbeat bumped");

        var seq = client.SendCommand(BridgeCommand.Teleport, 1, 2, 3);
        Assert.Equal(seq, view.ReadUInt32(c + BridgeProtocol.Control.CommandSeq), "command seq");
        Assert.Equal(2d, view.ReadDouble(c + BridgeProtocol.Control.CommandArgs + 8), "command arg 1");
    }

    private static MemoryMappedFile CreateFakePluginMapping()
    {
        MemoryMappedFile mapping;
        try
        {
            mapping = MemoryMappedFile.CreateNew(BridgeProtocol.MappingName, BridgeProtocol.SharedSize);
        }
        catch (IOException)
        {
            throw new SkipException("Plugin-Mapping existiert bereits (Spiel läuft?)");
        }

        using var view = mapping.CreateViewAccessor(0, BridgeProtocol.SharedSize);
        view.Write(4, BridgeProtocol.Version);
        view.Write(8, (uint)BridgeProtocol.SharedSize);
        view.Write(0, BridgeProtocol.Magic);
        return mapping;
    }
}
