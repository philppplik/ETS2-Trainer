using System.Diagnostics;
using System.Reflection;

namespace Ets2Trainer.Tests;

/// <summary>Marks a public static method as a test.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class TestAttribute : Attribute
{
}

/// <summary>Thrown by a test to mark itself as skipped (e.g. game files not present).</summary>
public sealed class SkipException(string reason) : Exception(reason);

public static class Assert
{
    public static void True(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException("Assertion failed: " + message);
        }
    }

    public static void Equal<T>(T expected, T actual, string message)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Assertion failed: {message} (expected '{expected}', got '{actual}')");
        }
    }

    public static T Throws<T>(Action action, string message)
        where T : Exception
    {
        try
        {
            action();
        }
        catch (T ex)
        {
            return ex;
        }

        throw new InvalidOperationException($"Assertion failed: {message} (expected {typeof(T).Name})");
    }

    public static void Skip(string reason) => throw new SkipException(reason);
}

/// <summary>Minimal reflection-based runner so the test suite needs no NuGet packages.</summary>
public static class TestRunner
{
    public static int RunAll(string? filter)
    {
        var tests = typeof(TestRunner).Assembly.GetTypes()
            .SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.Static))
            .Where(m => m.GetCustomAttribute<TestAttribute>() is not null)
            .Where(m => filter is null || $"{m.DeclaringType!.Name}.{m.Name}".Contains(filter, StringComparison.OrdinalIgnoreCase))
            .OrderBy(m => m.DeclaringType!.Name).ThenBy(m => m.Name)
            .ToList();

        int passed = 0, failed = 0, skipped = 0;
        foreach (var test in tests)
        {
            var name = $"{test.DeclaringType!.Name}.{test.Name}";
            var sw = Stopwatch.StartNew();
            try
            {
                test.Invoke(null, null);
                passed++;
                Console.WriteLine($"  PASS  {name} ({sw.ElapsedMilliseconds} ms)");
            }
            catch (TargetInvocationException ex) when (ex.InnerException is SkipException skip)
            {
                skipped++;
                Console.WriteLine($"  SKIP  {name}: {skip.Message}");
            }
            catch (TargetInvocationException ex)
            {
                failed++;
                Console.WriteLine($"  FAIL  {name}: {ex.InnerException?.Message}");
                Console.WriteLine(ex.InnerException?.StackTrace);
            }
        }

        Console.WriteLine($"\n{passed} passed, {failed} failed, {skipped} skipped");
        return failed == 0 ? 0 : 1;
    }
}
