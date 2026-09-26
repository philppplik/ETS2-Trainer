using System.Buffers.Binary;

namespace Ets2Trainer.Core.HashFs;

/// <summary>
/// CityHash64 as used by SCS HashFS archives. SCS uses the v1.0.x short-input functions
/// (with k3), not the ones from CityHash v1.1 — using v1.1 produces wrong path hashes.
/// </summary>
public static class CityHash
{
    private const ulong K0 = 0xc3a5c85c97cb3127UL;
    private const ulong K1 = 0xb492b66fbe98f273UL;
    private const ulong K2 = 0x9ae16a3b2f90404fUL;
    private const ulong K3 = 0xc949d7c7509e6557UL;
    private const ulong KMul = 0x9ddfea08eb382d69UL;

    public static ulong Hash64(ReadOnlySpan<byte> s)
    {
        var len = (ulong)s.Length;
        if (len <= 16)
        {
            return HashLen0To16(s);
        }

        if (len <= 32)
        {
            return HashLen17To32(s);
        }

        return len <= 64 ? HashLen33To64(s) : HashLongInput(s);
    }

    private static ulong HashLongInput(ReadOnlySpan<byte> s)
    {
        var len = (ulong)s.Length;
        var x = Fetch64(s, s.Length - 40);
        var y = Fetch64(s, s.Length - 16) + Fetch64(s, s.Length - 56);
        var z = HashLen16(Fetch64(s, s.Length - 48) + len, Fetch64(s, s.Length - 24));
        var v = WeakHashLen32WithSeeds(s, s.Length - 64, len, z);
        var w = WeakHashLen32WithSeeds(s, s.Length - 32, y + K1, x);
        x = x * K1 + Fetch64(s, 0);

        var remaining = (len - 1) & ~63UL;
        var pos = 0;
        do
        {
            x = Rotate(x + y + v.First + Fetch64(s, pos + 8), 37) * K1;
            y = Rotate(y + v.Second + Fetch64(s, pos + 48), 42) * K1;
            x ^= w.Second;
            y += v.First + Fetch64(s, pos + 40);
            z = Rotate(z + w.First, 33) * K1;
            v = WeakHashLen32WithSeeds(s, pos, v.Second * K1, x + w.First);
            w = WeakHashLen32WithSeeds(s, pos + 32, z + w.Second, y + Fetch64(s, pos + 16));
            (z, x) = (x, z);
            pos += 64;
            remaining -= 64;
        }
        while (remaining != 0);

        return HashLen16(HashLen16(v.First, w.First) + ShiftMix(y) * K1 + z,
            HashLen16(v.Second, w.Second) + x);
    }

    private static ulong HashLen0To16(ReadOnlySpan<byte> s)
    {
        var len = (ulong)s.Length;
        if (len > 8)
        {
            var a = Fetch64(s, 0);
            var b = Fetch64(s, s.Length - 8);
            return HashLen16(a, RotateByAtLeast1(b + len, (int)len)) ^ b;
        }

        if (len >= 4)
        {
            ulong a = Fetch32(s, 0);
            return HashLen16(len + (a << 3), Fetch32(s, s.Length - 4));
        }

        if (len > 0)
        {
            uint a = s[0];
            uint b = s[s.Length >> 1];
            uint c = s[s.Length - 1];
            var y = a + (b << 8);
            var z = (uint)len + (c << 2);
            return ShiftMix(y * K2 ^ z * K3) * K2;
        }

        return K2;
    }

    private static ulong HashLen17To32(ReadOnlySpan<byte> s)
    {
        var len = (ulong)s.Length;
        var a = Fetch64(s, 0) * K1;
        var b = Fetch64(s, 8);
        var c = Fetch64(s, s.Length - 8) * K2;
        var d = Fetch64(s, s.Length - 16) * K0;
        return HashLen16(Rotate(a - b, 43) + Rotate(c, 30) + d, a + Rotate(b ^ K3, 20) - c + len);
    }

    private static ulong HashLen33To64(ReadOnlySpan<byte> s)
    {
        var len = (ulong)s.Length;
        var z = Fetch64(s, 24);
        var a = Fetch64(s, 0) + (len + Fetch64(s, s.Length - 16)) * K0;
        var b = Rotate(a + z, 52);
        var c = Rotate(a, 37);
        a += Fetch64(s, 8);
        c += Rotate(a, 7);
        a += Fetch64(s, 16);
        var vf = a + z;
        var vs = b + Rotate(a, 31) + c;
        a = Fetch64(s, 16) + Fetch64(s, s.Length - 32);
        z = Fetch64(s, s.Length - 8);
        b = Rotate(a + z, 52);
        c = Rotate(a, 37);
        a += Fetch64(s, s.Length - 24);
        c += Rotate(a, 7);
        a += Fetch64(s, s.Length - 16);
        var wf = a + z;
        var ws = b + Rotate(a, 31) + c;
        var r = ShiftMix((vf + ws) * K2 + (wf + vs) * K0);
        return ShiftMix(r * K0 + vs) * K2;
    }

    private static (ulong First, ulong Second) WeakHashLen32WithSeeds(ReadOnlySpan<byte> s, int pos, ulong a, ulong b)
    {
        var w = Fetch64(s, pos);
        var x = Fetch64(s, pos + 8);
        var y = Fetch64(s, pos + 16);
        var z = Fetch64(s, pos + 24);
        a += w;
        b = Rotate(b + a + z, 21);
        var c = a;
        a += x;
        a += y;
        b += Rotate(a, 44);
        return (a + z, b + c);
    }

    private static ulong HashLen16(ulong u, ulong v)
    {
        var a = (u ^ v) * KMul;
        a ^= a >> 47;
        var b = (v ^ a) * KMul;
        b ^= b >> 47;
        return b * KMul;
    }

    private static ulong Fetch64(ReadOnlySpan<byte> s, int pos) => BinaryPrimitives.ReadUInt64LittleEndian(s[pos..]);

    private static uint Fetch32(ReadOnlySpan<byte> s, int pos) => BinaryPrimitives.ReadUInt32LittleEndian(s[pos..]);

    private static ulong Rotate(ulong val, int shift) => shift == 0 ? val : (val >> shift) | (val << (64 - shift));

    private static ulong RotateByAtLeast1(ulong val, int shift) => (val >> shift) | (val << (64 - shift));

    private static ulong ShiftMix(ulong val) => val ^ (val >> 47);
}
