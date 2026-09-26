using System.IO.MemoryMappedFiles;
using System.Text;
using C = Ets2Trainer.Core.Bridge.BridgeProtocol.Control;
using S = Ets2Trainer.Core.Bridge.BridgeProtocol.Status;
using T = Ets2Trainer.Core.Bridge.BridgeProtocol.Telemetry;

namespace Ets2Trainer.Core.Bridge;

/// <summary>
/// Connects to the shared memory created by the in-game plugin. Not thread-safe: use from one (UI timer) thread.
/// </summary>
public sealed class BridgeClient : IDisposable
{
    private const int SeqlockRetries = 4;
    private static readonly TimeSpan PluginTimeout = TimeSpan.FromSeconds(2);

    private MemoryMappedFile? _file;
    private MemoryMappedViewAccessor? _view;
    private uint _appHeartbeat;
    private uint _lastPluginHeartbeat;
    private DateTime _lastHeartbeatChange = DateTime.MinValue;

    public bool IsConnected => _view is not null;

    /// <summary>True when connected and the plugin heartbeat advanced recently (game running, not frozen).</summary>
    public bool IsPluginAlive => IsConnected && DateTime.UtcNow - _lastHeartbeatChange < PluginTimeout;

    public bool TryConnect()
    {
        if (_view is not null)
        {
            return true;
        }

        try
        {
            _file = MemoryMappedFile.OpenExisting(BridgeProtocol.MappingName, MemoryMappedFileRights.ReadWrite);
            _view = _file.CreateViewAccessor(0, BridgeProtocol.SharedSize, MemoryMappedFileAccess.ReadWrite);
            if (_view.ReadUInt32(0) != BridgeProtocol.Magic || _view.ReadUInt32(4) != BridgeProtocol.Version ||
                _view.ReadUInt32(8) != BridgeProtocol.SharedSize)
            {
                Disconnect();
                return false;
            }

            return true;
        }
        catch (Exception ex) when (ex is FileNotFoundException or IOException or UnauthorizedAccessException)
        {
            Disconnect();
            return false;
        }
    }

    public void Disconnect()
    {
        _view?.Dispose();
        _file?.Dispose();
        _view = null;
        _file = null;
    }

    /// <summary>Reads a consistent telemetry snapshot (seqlock) or null when not connected.</summary>
    public TelemetrySnapshot? ReadTelemetry()
    {
        if (_view is not { } v)
        {
            return null;
        }

        for (var attempt = 0; attempt < SeqlockRetries; attempt++)
        {
            var begin = v.ReadUInt32(BridgeProtocol.TelemetryOffset + T.SeqBegin);
            var snapshot = ReadTelemetryFields(v, BridgeProtocol.TelemetryOffset);
            var end = v.ReadUInt32(BridgeProtocol.TelemetryOffset + T.SeqEnd);
            if (begin == end)
            {
                return snapshot;
            }
        }

        return ReadTelemetryFields(v, BridgeProtocol.TelemetryOffset); // best effort
    }

    public StatusSnapshot? ReadStatus()
    {
        if (_view is not { } v)
        {
            return null;
        }

        const int o = BridgeProtocol.StatusOffset;
        var heartbeat = v.ReadUInt32(o + S.Heartbeat);
        if (heartbeat != _lastPluginHeartbeat)
        {
            _lastPluginHeartbeat = heartbeat;
            _lastHeartbeatChange = DateTime.UtcNow;
        }

        return new StatusSnapshot(
            v.ReadUInt32(o + S.PluginBuild), heartbeat,
            ReadFeature(v, o + S.Fuel), ReadFeature(v, o + S.Wear), ReadFeature(v, o + S.Velocity), ReadFeature(v, o + S.Position),
            v.ReadUInt32(o + S.LastCommandAck), (ActiveFlags)v.ReadUInt32(o + S.ActiveFlags), v.ReadUInt32(o + S.MenuToggleCount),
            ReadString(v, o + S.Message, S.MessageLength));
    }

    /// <summary>Writes the desired live configuration and bumps the app heartbeat.</summary>
    public void WriteControl(ControlState state)
    {
        if (_view is not { } v)
        {
            return;
        }

        const int o = BridgeProtocol.ControlOffset;
        v.Write(o + C.InfiniteFuel, state.InfiniteFuel ? 1u : 0u);
        v.Write(o + C.NoDamage, state.NoDamage ? 1u : 0u);
        v.Write(o + C.PowerBoost, state.PowerBoost ? 1u : 0u);
        v.Write(o + C.PowerFactor, state.PowerFactor);
        v.Write(o + C.NitroEnabled, state.NitroEnabled ? 1u : 0u);
        v.Write(o + C.NitroVk, state.NitroVk);
        v.Write(o + C.NitroAccel, state.NitroAccel);
        v.Write(o + C.SpeedCapEnabled, state.SpeedCapEnabled ? 1u : 0u);
        v.Write(o + C.SpeedCapKmh, state.SpeedCapKmh);
        v.Write(o + C.MenuHotkeyVk, state.MenuHotkeyVk);
        v.Write(o + C.AppHeartbeat, unchecked(++_appHeartbeat));
    }

    /// <summary>Issues a one-shot command; the plugin acknowledges via <see cref="StatusSnapshot.LastCommandAck"/>.</summary>
    public uint SendCommand(BridgeCommand command, params double[] args)
    {
        if (_view is not { } v)
        {
            return 0;
        }

        const int o = BridgeProtocol.ControlOffset;
        for (var i = 0; i < C.CommandArgCount; i++)
        {
            v.Write(o + C.CommandArgs + i * sizeof(double), i < args.Length ? args[i] : 0d);
        }

        v.Write(o + C.CommandType, (uint)command);
        var seq = unchecked(v.ReadUInt32(o + C.CommandSeq) + 1);
        Thread.MemoryBarrier();
        v.Write(o + C.CommandSeq, seq); // written last: the plugin reacts to the sequence change
        return seq;
    }

    public void Dispose() => Disconnect();

    private static TelemetrySnapshot ReadTelemetryFields(MemoryMappedViewAccessor v, int o) => new(
        v.ReadUInt32(o + T.FrameCounter), v.ReadUInt32(o + T.GameVersion), (TelemetryFlags)v.ReadUInt32(o + T.Flags),
        v.ReadDouble(o + T.PosX), v.ReadDouble(o + T.PosY), v.ReadDouble(o + T.PosZ), v.ReadSingle(o + T.Heading),
        v.ReadSingle(o + T.Speed), v.ReadSingle(o + T.Rpm), v.ReadSingle(o + T.RpmMax), v.ReadInt32(o + T.Gear),
        v.ReadSingle(o + T.EffThrottle), v.ReadSingle(o + T.EffBrake), v.ReadSingle(o + T.Fuel), v.ReadSingle(o + T.FuelCapacity),
        v.ReadSingle(o + T.FuelRange), v.ReadSingle(o + T.WearEngine), v.ReadSingle(o + T.WearTransmission),
        v.ReadSingle(o + T.WearCabin), v.ReadSingle(o + T.WearChassis), v.ReadSingle(o + T.WearWheels),
        v.ReadSingle(o + T.TrailerWearChassis), v.ReadSingle(o + T.CargoDamage), v.ReadSingle(o + T.SpeedLimit),
        v.ReadSingle(o + T.CruiseControl), v.ReadSingle(o + T.Odometer), v.ReadUInt32(o + T.GameTimeMin),
        ReadString(v, o + T.TruckBrand, T.TruckBrandLength), ReadString(v, o + T.TruckName, T.TruckNameLength),
        ReadString(v, o + T.TruckId, T.TruckIdLength), ReadString(v, o + T.Cargo, T.CargoLength),
        ReadString(v, o + T.DestinationCity, T.DestinationCityLength));

    private static FeatureStatus ReadFeature(MemoryMappedViewAccessor v, int offset) => new(
        (FeatureState)v.ReadUInt32(offset), v.ReadUInt32(offset + 4), v.ReadUInt32(offset + 8), v.ReadSingle(offset + 12));

    private static string ReadString(MemoryMappedViewAccessor v, int offset, int length)
    {
        var bytes = new byte[length];
        v.ReadArray(offset, bytes, 0, length);
        var end = Array.IndexOf(bytes, (byte)0);
        return Encoding.UTF8.GetString(bytes, 0, end < 0 ? length : end);
    }
}
