namespace Ets2Trainer.App.Infrastructure;

/// <summary>A selectable key (Win32 virtual-key code + German label).</summary>
public sealed record KeyOption(uint VirtualKey, string Label)
{
    public override string ToString() => Label;
}

/// <summary>Keys offered for the menu hotkey and the nitro key.</summary>
public static class KeyNames
{
    public static readonly IReadOnlyList<KeyOption> MenuKeys = Enumerable.Range(1, 12)
        .Select(i => new KeyOption((uint)(0x70 + i - 1), $"F{i}"))
        .Append(new KeyOption(0x2D, "Einfg"))
        .Append(new KeyOption(0x24, "Pos1"))
        .ToList();

    public static readonly IReadOnlyList<KeyOption> NitroKeys = new List<KeyOption>
    {
        new(0x10, "Shift"),
        new(0x11, "Strg"),
        new(0x12, "Alt"),
        new(0x14, "Feststell"),
        new(0x4E, "N"),
        new(0x42, "B"),
        new(0x58, "X"),
        new(0x05, "Maus 4"),
        new(0x06, "Maus 5"),
    };

    /// <summary>Num 0–9: default trick hotkeys (jump, rocket, roll, hover, unflip).</summary>
    public static readonly IReadOnlyList<KeyOption> NumpadKeys = Enumerable.Range(0, 10)
        .Select(i => new KeyOption((uint)(0x60 + i), $"Num {i}"))
        .ToList();

    public static string Name(uint vk) =>
        MenuKeys.Concat(NitroKeys).Concat(NumpadKeys).FirstOrDefault(k => k.VirtualKey == vk)?.Label ?? $"0x{vk:X2}";
}
