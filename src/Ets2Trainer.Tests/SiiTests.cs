using Ets2Trainer.Core.Game;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Tests;

public static class SiiTests
{
    private const string SampleText = """
        SiiNunit
        {
        bank : _nameless.1f5.d4c0 {
         money_account: 1234567
         loans: 0
         ratio: &3dcccccd
        }

        garage : garage.berlin
        {
         vehicles: 2
         vehicles[0]: null
         vehicles[1]: _nameless.1f5.aaaa
         # comment line
         status: 3 // trailing comment
         name: "Berl#in \"HQ\""
        }
        }
        """;

    [Test]
    public static void TextParser_ReadsScalarsArraysAndComments()
    {
        var doc = SiiTextParser.Parse(SampleText);

        Assert.Equal(2, doc.Units.Count, "unit count");
        var bank = doc.Find("_nameless.1f5.d4c0")!;
        Assert.Equal("1234567", bank.Get("money_account"), "money");
        var garage = doc.Find("garage.berlin")!;
        Assert.Equal(2, garage.GetArray("vehicles").Count, "vehicles array");
        Assert.Equal("_nameless.1f5.aaaa", garage.GetArray("vehicles")[1], "vehicle 1");
        Assert.Equal("3", garage.Get("status"), "status without trailing comment");
        Assert.Equal("Berl#in \"HQ\"", SiiValues.Unquote(garage.Get("name")), "quoted string with # and escapes");
    }

    [Test]
    public static void TextWriter_RoundTripsThroughParser()
    {
        var doc = SiiTextParser.Parse(SampleText);
        var first = SiiTextWriter.Write(doc);
        var second = SiiTextWriter.Write(SiiTextParser.Parse(first));
        Assert.Equal(first, second, "text round trip");
    }

    [Test]
    public static void Values_FloatFormattingIsExact()
    {
        Assert.Equal("1", SiiValues.FormatFloat(1f), "integral float");
        Assert.Equal("-5", SiiValues.FormatFloat(-5f), "negative integral float");
        Assert.Equal("&3dcccccd", SiiValues.FormatFloat(0.1f), "fraction as hex bits");
        Assert.Equal(0.1f, SiiValues.ParseFloat("&3dcccccd"), "hex parse");
        Assert.Equal(12.5f, SiiValues.ParseFloat("12.5"), "decimal parse");
    }

    [Test]
    public static void Values_QuoteEscapesNonAscii()
    {
        var quoted = SiiValues.Quote("Köln \"1\"");
        Assert.Equal("\"K\\xc3\\xb6ln \\\"1\\\"\"", quoted, "escaped");
        Assert.Equal("Köln \"1\"", SiiValues.Unquote(quoted), "unescaped");
    }

    [Test]
    public static void Detect_RecognizesSignatures()
    {
        Assert.Equal(SiiFormat.Text, ScsCrypto.Detect("SiiNunit"u8), "text");
        Assert.Equal(SiiFormat.Binary, ScsCrypto.Detect("BSII\u0003"u8), "binary");
        Assert.Equal(SiiFormat.Encrypted, ScsCrypto.Detect("ScsC...."u8), "encrypted");
        Assert.Equal(SiiFormat.Unknown, ScsCrypto.Detect("PK\u0003\u0004"u8), "zip");
    }

    [Test]
    public static void RealSave_DecodesAndRoundTripsLosslessly()
    {
        var save = TestData.NewestGameSii() ?? throw new SkipException("kein Spielstand gefunden");
        var doc = SiiFile.LoadFile(save);

        Assert.True(doc.Units.Count > 1000, "save has many units");
        var economy = doc.FirstOfClass("economy") ?? throw new InvalidOperationException("economy missing");
        var bank = doc.Find(economy.Get("bank")) ?? throw new InvalidOperationException("bank missing");
        Assert.True(long.TryParse(bank.Get("money_account"), out _), "money is an integer");

        var text = SiiTextWriter.Write(doc);
        var reparsed = SiiTextParser.Parse(text);
        Assert.Equal(doc.Units.Count, reparsed.Units.Count, "unit count after reparse");
        Assert.Equal(text, SiiTextWriter.Write(reparsed), "text identical after reparse");
    }
}

internal static class TestData
{
    public static string? NewestGameSii()
    {
        var saves = GamePaths.FindProfiles()
            .Where(p => Directory.Exists(p.SaveDirectory))
            .SelectMany(p => Directory.GetDirectories(p.SaveDirectory))
            .Select(d => Path.Combine(d, "game.sii"))
            .Where(File.Exists)
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .ToList();
        return saves.FirstOrDefault();
    }
}
