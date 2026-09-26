using System.Text;
using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.HashFs;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Tests;

public static class HashFsTests
{
    [Test]
    public static void CityHash_EmptyInputIsK2()
    {
        Assert.Equal(0x9ae16a3b2f90404fUL, CityHash.Hash64(ReadOnlySpan<byte>.Empty), "empty hash");
    }

    [Test]
    public static void CityHash_IsDeterministicAcrossLengthClasses()
    {
        foreach (var len in new[] { 3, 8, 12, 20, 40, 64, 65, 130 })
        {
            var data = Encoding.ASCII.GetBytes(new string('a', len));
            Assert.Equal(CityHash.Hash64(data), CityHash.Hash64(data), $"stable for length {len}");
        }
    }

    [Test]
    public static void GameArchives_ListTrucksAndReadEngineDefinition()
    {
        var game = GamePaths.FindGameDirectory() ?? throw new SkipException("ETS2 nicht installiert");
        using var vfs = GameFileSystem.Mount(game);
        Assert.True(vfs.Archives.Count > 3, "archives mounted");

        var trucks = vfs.ListDirectories("/def/vehicle/truck");
        Assert.True(trucks.Contains("scania.s_2016"), "scania.s_2016 listed (paths 33-64 bytes hash correctly)");

        var engineDir = "/def/vehicle/truck/scania.s_2016/engine";
        var engines = vfs.ListFiles(engineDir).Where(f => f.EndsWith(".sii", StringComparison.Ordinal)).ToList();
        Assert.True(engines.Count > 0, "engines listed");

        var path = $"{engineDir}/{engines[0]}";
        var text = vfs.ReadText(path) ?? throw new InvalidOperationException("engine def unreadable");
        var doc = SiiTextParser.Parse(text, vfs.IncludeResolver(engineDir));
        var engine = doc.FirstOfClass("accessory_engine_data") ?? throw new InvalidOperationException("no engine unit");
        Assert.True(SiiValues.ParseFloat(engine.Get("torque")) > 100, "torque parsed");
    }
}
