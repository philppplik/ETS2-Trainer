namespace Ets2Trainer.Core.Bridge;

/// <summary>
/// C# mirror of <c>shared/bridge_protocol.h</c>: byte offsets inside the shared memory block.
/// Keep in sync with the C++ static_asserts (sizes 424 / 344 / 104 / 888).
/// </summary>
public static class BridgeProtocol
{
    public const string MappingName = @"Local\ETS2TrainerBridge_v1";
    public const uint Magic = 0x42543245;
    public const uint Version = 1;
    public const int SharedSize = 888;

    public const int TelemetryOffset = 16;
    public const int TelemetrySize = 424;
    public const int StatusOffset = 440;
    public const int StatusSize = 344;
    public const int ControlOffset = 784;
    public const int ControlSize = 104;

    public static class Telemetry
    {
        public const int SeqBegin = 0, FrameCounter = 4, GameVersion = 8, Flags = 12;
        public const int SimTimeUs = 16, RenderTimeUs = 24;
        public const int PosX = 32, PosY = 40, PosZ = 48;
        public const int Heading = 56, Pitch = 60, Roll = 64, Speed = 68;
        public const int VelLocal = 72, AccLocal = 84;
        public const int Rpm = 96, RpmMax = 100, Gear = 104, GearsForward = 108;
        public const int InputThrottle = 112, InputBrake = 116, EffThrottle = 120, EffBrake = 124;
        public const int Fuel = 128, FuelCapacity = 132, FuelRange = 136, FuelAvgConsumption = 140;
        public const int WearEngine = 144, WearTransmission = 148, WearCabin = 152, WearChassis = 156, WearWheels = 160;
        public const int TrailerWearChassis = 164, CargoDamage = 168;
        public const int SpeedLimit = 172, CruiseControl = 176, Odometer = 180;
        public const int GameTimeMin = 184, RestStopMin = 188;
        public const int TruckBrand = 192, TruckBrandLength = 32;
        public const int TruckName = 224, TruckNameLength = 48;
        public const int TruckId = 272, TruckIdLength = 48;
        public const int Cargo = 320, CargoLength = 48;
        public const int DestinationCity = 368, DestinationCityLength = 48;
        public const int SeqEnd = 420;
    }

    public static class Status
    {
        public const int PluginBuild = 0, Heartbeat = 4;
        public const int Fuel = 8, Wear = 24, Velocity = 40, Position = 56; // FeatureStatus, 16 bytes each
        public const int LastCommandAck = 72, ActiveFlags = 76, MenuToggleCount = 80;
        public const int Message = 88, MessageLength = 256;
    }

    public static class Control
    {
        public const int AppHeartbeat = 0, InfiniteFuel = 4, NoDamage = 8, PowerBoost = 12, PowerFactor = 16;
        public const int NitroEnabled = 20, NitroVk = 24, NitroAccel = 28, SpeedCapEnabled = 32, SpeedCapKmh = 36;
        public const int MenuHotkeyVk = 40, CommandSeq = 44, CommandType = 48, CommandArgs = 56, CommandArgCount = 6;
    }
}

[Flags]
public enum TelemetryFlags : uint
{
    None = 0,
    Paused = 1 << 0,
    EngineOn = 1 << 1,
    ParkingBrake = 1 << 2,
    TrailerAttached = 1 << 3,
    TruckersMpDetected = 1 << 4,
    HasTruck = 1 << 5,
}

public enum FeatureState : uint
{
    Off = 0,
    WaitingForData = 1,
    Calibrating = 2,
    Verifying = 3,
    Active = 4,
    Failed = 5,
    Blocked = 6,
}

[Flags]
public enum ActiveFlags : uint
{
    None = 0,
    Fuel = 1 << 0,
    NoDamage = 1 << 1,
    Boost = 1 << 2,
    Nitro = 1 << 3,
    SpeedCap = 1 << 4,
}

public enum BridgeCommand : uint
{
    None = 0,
    Refuel = 1,
    Repair = 2,
    StopTruck = 3,
    Recalibrate = 4,
    Teleport = 5,
    ResetAll = 6,
}

public sealed record FeatureStatus(FeatureState State, uint Candidates, uint Confirmed, float Progress);

public sealed record TelemetrySnapshot(
    uint FrameCounter, uint GameVersion, TelemetryFlags Flags, double PosX, double PosY, double PosZ, float Heading,
    float SpeedMs, float Rpm, float RpmMax, int Gear, float Throttle, float Brake, float Fuel, float FuelCapacity,
    float FuelRange, float WearEngine, float WearTransmission, float WearCabin, float WearChassis, float WearWheels,
    float TrailerWearChassis, float CargoDamage, float SpeedLimitMs, float CruiseControlMs, float OdometerKm,
    uint GameTimeMin, string TruckBrand, string TruckName, string TruckId, string Cargo, string DestinationCity)
{
    public float SpeedKmh => SpeedMs * 3.6f;

    public float SpeedLimitKmh => SpeedLimitMs * 3.6f;

    public float WorstWear => new[] { WearEngine, WearTransmission, WearCabin, WearChassis, WearWheels }.Max();
}

public sealed record StatusSnapshot(uint PluginBuild, uint Heartbeat, FeatureStatus Fuel, FeatureStatus Wear,
    FeatureStatus Velocity, FeatureStatus Position, uint LastCommandAck, ActiveFlags ActiveFlags, uint MenuToggleCount,
    string Message);

/// <summary>Desired live-cheat configuration written by the app every tick.</summary>
public sealed record ControlState(
    bool InfiniteFuel = false,
    bool NoDamage = false,
    bool PowerBoost = false,
    float PowerFactor = 2f,
    bool NitroEnabled = false,
    uint NitroVk = 0x10, // VK_SHIFT
    float NitroAccel = 6f,
    bool SpeedCapEnabled = false,
    float SpeedCapKmh = 130f,
    uint MenuHotkeyVk = 0x77); // VK_F8
