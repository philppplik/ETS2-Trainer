using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Ets2Trainer.Core.HashFs;
using Ets2Trainer.Core.Sii;

namespace Ets2Trainer.Core.Mods;

/// <summary>An engine available for a truck model.</summary>
public sealed record EngineOption(string FileName, string DataPath, string DisplayName, float TorqueNm, int Horsepower)
{
    public bool IsTrainerEngine => FileName.StartsWith(EngineModBuilder.FilePrefix, StringComparison.Ordinal);

    public override string ToString() => $"{DisplayName}  ({Horsepower} PS · {TorqueNm:0} Nm)";
}

/// <summary>A generated engine definition ready to be packed into the trainer mod.</summary>
public sealed record GeneratedEngine(string TruckModel, string FileName, string DataPath, string DefinitionText,
    int Horsepower, int TorqueNm);

/// <summary>
/// Creates boosted engine definitions by copying an original engine of the same truck (keeping its sound,
/// torque curve and badges) and changing power, torque, name and price.
/// </summary>
public static partial class EngineModBuilder
{
    public const string FilePrefix = "e2t_";
    public const int MinHorsepower = 100;
    public const int MaxHorsepower = 99_999;
    private const double KwPerHp = 0.7355;
    private const double HpPerNmAt1400Rpm = 1400.0 / 7023.5;

    public static string EngineDirectory(string truckModel) => $"/def/vehicle/truck/{truckModel}/engine";

    public static IReadOnlyList<EngineOption> ListEngines(GameFileSystem vfs, string truckModel)
    {
        var dir = EngineDirectory(truckModel);
        var result = new List<EngineOption>();
        foreach (var file in vfs.ListFiles(dir).Where(f => f.EndsWith(".sii", StringComparison.Ordinal)))
        {
            var path = $"{dir}/{file}";
            if (vfs.ReadText(path) is not { } text ||
                SiiTextParser.Parse(text).FirstOfClass("accessory_engine_data") is not { } unit)
            {
                continue;
            }

            var torque = SiiValues.ParseFloat(unit.Get("torque"));
            var hp = ParseHorsepower(unit.GetArray("info")) ?? (int)Math.Round(torque * HpPerNmAt1400Rpm);
            result.Add(new EngineOption(file, path, DisplayNameOf(unit, file), torque, hp));
        }

        return result.OrderBy(e => e.Horsepower).ToList();
    }

    /// <summary>Builds a copy of <paramref name="baseEngine"/> scaled to <paramref name="targetHorsepower"/>.</summary>
    public static GeneratedEngine Build(GameFileSystem vfs, string truckModel, EngineOption baseEngine, int targetHorsepower)
    {
        var hp = Math.Clamp(targetHorsepower, MinHorsepower, MaxHorsepower);
        var original = vfs.ReadText(baseEngine.DataPath)
                       ?? throw new FileNotFoundException("Original-Motordatei nicht gefunden.", baseEngine.DataPath);

        var torque = (int)Math.Round(baseEngine.TorqueNm * hp / Math.Max(1, baseEngine.Horsepower));
        var unitName = $"{FilePrefix}{hp}";
        var text = Transform(original, unitName, hp, torque, $"ETS2 Trainer {hp} PS");
        var fileName = $"{unitName}.sii";
        return new GeneratedEngine(truckModel, fileName, $"{EngineDirectory(truckModel)}/{fileName}", text, hp, torque);
    }

    /// <summary>Line-based edit of the original def so includes, curves, sounds and overrides stay intact.</summary>
    public static string Transform(string original, string unitName, int hp, int torque, string displayName)
    {
        var lines = original.Replace("\r\n", "\n").Split('\n');
        var rpmInfo = lines.Select(l => l.Trim())
            .FirstOrDefault(l => InfoRegex().IsMatch(l) && l.Contains("@@rpm@@", StringComparison.Ordinal));

        var output = new StringBuilder(original.Length + 256);
        var infoWritten = false;
        foreach (var rawLine in lines)
        {
            var line = rawLine.TrimStart();
            if (HeaderRegex().IsMatch(line))
            {
                output.Append(HeaderRegex().Replace(line, m => $"{m.Groups[1].Value}{unitName}.")).Append('\n');
            }
            else if (InfoRegex().IsMatch(line))
            {
                if (!infoWritten)
                {
                    AppendInfo(output, hp, torque, rpmInfo);
                    infoWritten = true;
                }
            }
            else
            {
                output.Append(ReplaceScalar(rawLine, line, displayName, torque)).Append('\n');
            }
        }

        return output.ToString().TrimEnd('\n') + "\n";
    }

    public static int? ParseHorsepower(IEnumerable<string> infoLines)
    {
        foreach (var info in infoLines)
        {
            var text = SiiValues.Unquote(info).Replace("@@dg@@", string.Empty, StringComparison.Ordinal);
            var m = HorsepowerRegex().Match(text);
            if (m.Success && int.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var hp))
            {
                return hp;
            }
        }

        return null;
    }

    private static void AppendInfo(StringBuilder output, int hp, int torque, string? rpmInfo)
    {
        var kw = (int)Math.Round(hp * KwPerHp);
        output.Append(CultureInfo.InvariantCulture, $"\tinfo[]: \"{hp} @@hp@@ ({kw}@@kw@@)\"\n");
        output.Append(CultureInfo.InvariantCulture, $"\tinfo[]: \"{torque} @@nm@@\"\n");
        if (rpmInfo is not null)
        {
            output.Append('\t').Append(rpmInfo).Append('\n');
        }
    }

    private static string ReplaceScalar(string rawLine, string line, string displayName, int torque)
    {
        var match = ScalarRegex().Match(line);
        if (!match.Success)
        {
            return rawLine;
        }

        var indent = rawLine[..(rawLine.Length - line.Length)];
        return match.Groups[1].Value switch
        {
            "name" => $"{indent}name: \"{displayName}\"",
            "price" => $"{indent}price: 1",
            "unlock" => $"{indent}unlock: 0",
            "torque" => string.Create(CultureInfo.InvariantCulture, $"{indent}torque: {torque}"),
            _ => rawLine,
        };
    }

    private static string DisplayNameOf(SiiUnit unit, string file)
    {
        var name = SiiValues.Unquote(unit.Get("name"));
        var stem = Path.GetFileNameWithoutExtension(file);
        return name.Length == 0 || name.StartsWith("@@", StringComparison.Ordinal) ? stem.ToUpperInvariant() : name;
    }

    [GeneratedRegex(@"^(accessory_engine_data\s*:\s*)[a-z0-9_]+\.")]
    private static partial Regex HeaderRegex();

    [GeneratedRegex(@"^info\[\d*\]\s*:")]
    private static partial Regex InfoRegex();

    [GeneratedRegex(@"^(name|price|unlock|torque)\s*:")]
    private static partial Regex ScalarRegex();

    [GeneratedRegex(@"^\s*(\d+)\s*@@hp@@")]
    private static partial Regex HorsepowerRegex();
}
