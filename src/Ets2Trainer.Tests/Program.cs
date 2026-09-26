using Ets2Trainer.Core.Sii;
using Ets2Trainer.Tests;

// Usage:
//   Ets2Trainer.Tests                 run all tests
//   Ets2Trainer.Tests test <filter>   run matching tests
//   Ets2Trainer.Tests classes <file>  list unit classes of a SII file (read-only)
//   Ets2Trainer.Tests dump <file> <class> [max]  print units of a class (read-only)
if (args.Length == 0 || args[0] == "test")
{
    return TestRunner.RunAll(args.Length > 1 ? args[1] : null);
}

var doc = SiiFile.LoadFile(args[1]);
switch (args[0])
{
    case "classes":
        foreach (var g in doc.Units.GroupBy(u => u.ClassName).OrderByDescending(g => g.Count()))
        {
            Console.WriteLine($"{g.Count(),7}  {g.Key}");
        }

        return 0;
    case "dump":
        var max = args.Length > 3 ? int.Parse(args[3]) : 3;
        var subset = new SiiDocument(doc.OfClass(args[2]).Take(max));
        Console.Write(SiiTextWriter.Write(subset));
        return 0;
    default:
        Console.Error.WriteLine("Unbekannter Befehl.");
        return 2;
}
