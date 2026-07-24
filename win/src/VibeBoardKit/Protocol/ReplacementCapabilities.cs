using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Storage state reported by the device.
/// </summary>
public enum StorageState
{
    Unformatted,
    Ready,
    Corrupt,
    MountFailed,
    Busy
}

public static class StorageStateExtensions
{
    public static string WireString(this StorageState state) => state switch
    {
        StorageState.Unformatted => "unformatted",
        StorageState.Ready => "ready",
        StorageState.Corrupt => "corrupt",
        StorageState.MountFailed => "mount_failed",
        StorageState.Busy => "busy",
        _ => throw new ProtocolException($"Unknown storage state: {state}")
    };

    public static StorageState FromWire(string s) => s switch
    {
        "unformatted" => StorageState.Unformatted,
        "ready" => StorageState.Ready,
        "corrupt" => StorageState.Corrupt,
        "mount_failed" => StorageState.MountFailed,
        "busy" => StorageState.Busy,
        _ => throw new ProtocolException($"Unknown storage state: {s}")
    };
}

/// <summary>
/// Feature availability: either available with a capability profile, or unavailable with a reason.
/// </summary>
public abstract class FeatureAvailability<T> where T : class
{
    private FeatureAvailability() { }

    public sealed class Available : FeatureAvailability<T>
    {
        public T Capability { get; }
        public Available(T capability) { Capability = capability; }
    }

    public sealed class Unavailable : FeatureAvailability<T>
    {
        public ushort Version { get; }
        public string Reason { get; }
        public Unavailable(ushort version, string reason) { Version = version; Reason = reason; }
    }

    public bool IsAvailable => this is Available;
    public bool IsUnavailable => this is Unavailable;
}

/// <summary>
/// Unavailable feature info.
/// </summary>
public sealed class UnavailableFeature
{
    public ushort Version { get; }
    public string Reason { get; }
    public UnavailableFeature(ushort version, string reason) { Version = version; Reason = reason; }
}

public sealed class AssetsCapability
{
    public bool Management { get; }
    public StorageState StorageState { get; }
    public uint FreeBytes { get; }
    public uint ReserveBytes { get; }
    public uint UploadMaxBytes { get; }
    public uint MaxAssetBytes { get; }
    public ushort ChunkBytes { get; }
    public ushort MaxAssets { get; }
    public ushort MaxFrames { get; }
    public ushort MinFrameMS { get; }
    public ushort MaxFrameMS { get; }
    public uint MaxActiveDecodedBytes { get; }
    public uint DecoderScratchBytes { get; }
    public string[] Encodings { get; }
    public uint Revision { get; }

    public AssetsCapability(bool management, StorageState storageState, uint freeBytes, uint reserveBytes,
        uint uploadMaxBytes, uint maxAssetBytes, ushort chunkBytes, ushort maxAssets, ushort maxFrames,
        ushort minFrameMS, ushort maxFrameMS, uint maxActiveDecodedBytes, uint decoderScratchBytes,
        string[] encodings, uint revision)
    {
        Management = management;
        StorageState = storageState;
        FreeBytes = freeBytes;
        ReserveBytes = reserveBytes;
        UploadMaxBytes = uploadMaxBytes;
        MaxAssetBytes = maxAssetBytes;
        ChunkBytes = chunkBytes;
        MaxAssets = maxAssets;
        MaxFrames = maxFrames;
        MinFrameMS = minFrameMS;
        MaxFrameMS = maxFrameMS;
        MaxActiveDecodedBytes = maxActiveDecodedBytes;
        DecoderScratchBytes = decoderScratchBytes;
        Encodings = encodings;
        Revision = revision;
    }
}

public sealed class ScreenFontCapability
{
    public string ID { get; }
    public ushort Version { get; }
    public string MetricsSHA256 { get; }
    public ScreenFontCapability(string id, ushort version, string metricsSHA256)
    { ID = id; Version = version; MetricsSHA256 = metricsSHA256; }
}

public sealed class LEDCapability
{
    public Dictionary<string, byte> KeyPixels { get; }
    public byte MaxBrightness { get; }
    public ushort MaxFrameChannelSum { get; }
    public LEDCapability(Dictionary<string, byte> keyPixels, byte maxBrightness, ushort maxFrameChannelSum)
    { KeyPixels = keyPixels; MaxBrightness = maxBrightness; MaxFrameChannelSum = maxFrameChannelSum; }
}

public sealed class UpdateCapability
{
    public ushort ChunkBytes { get; }
    public uint MaxImageBytes { get; }
    public string Target { get; }
    public string StagedMetadata { get; }
    public string Rollback { get; }
    public UpdateCapability(ushort chunkBytes, uint maxImageBytes, string target, string stagedMetadata, string rollback)
    { ChunkBytes = chunkBytes; MaxImageBytes = maxImageBytes; Target = target; StagedMetadata = stagedMetadata; Rollback = rollback; }
}

public sealed class ScreenCapability
{
    public string[] Modes { get; }
    public ushort MaxCommitBytes { get; }
    public ushort MaxLayoutBytes { get; }
    public ushort MaxAssets { get; }
    public ushort MaxObjects { get; }
    public byte MaxDepth { get; }
    public ushort MaxWidgets { get; }
    public ushort MaxFonts { get; }
    public byte MaxPetStates { get; }
    public ushort MaxStringBytes { get; }
    public ushort MaxJSONTokens { get; }
    public ushort MaxWidgetValueBytes { get; }
    public uint Revision { get; }
    public bool Configured { get; }
    public List<ScreenFontCapability> Fonts { get; }

    public ScreenCapability(string[] modes, ushort maxCommitBytes, ushort maxLayoutBytes,
        ushort maxAssets, ushort maxObjects, byte maxDepth, ushort maxWidgets, ushort maxFonts,
        byte maxPetStates, ushort maxStringBytes, ushort maxJSONTokens, ushort maxWidgetValueBytes,
        uint revision, bool configured, List<ScreenFontCapability> fonts)
    {
        Modes = modes; MaxCommitBytes = maxCommitBytes; MaxLayoutBytes = maxLayoutBytes;
        MaxAssets = maxAssets; MaxObjects = maxObjects; MaxDepth = maxDepth;
        MaxWidgets = maxWidgets; MaxFonts = maxFonts; MaxPetStates = maxPetStates;
        MaxStringBytes = maxStringBytes; MaxJSONTokens = maxJSONTokens;
        MaxWidgetValueBytes = maxWidgetValueBytes; Revision = revision;
        Configured = configured; Fonts = fonts;
    }

    public ScreenCapability Selecting(uint revision, bool configured) => new(
        Modes, MaxCommitBytes, MaxLayoutBytes, MaxAssets, MaxObjects, MaxDepth,
        MaxWidgets, MaxFonts, MaxPetStates, MaxStringBytes, MaxJSONTokens,
        MaxWidgetValueBytes, revision, configured, Fonts);
}

/// <summary>
/// Snapshot of all device capabilities from a vk_capabilities event.
/// </summary>
public sealed class ReplacementCapabilitySnapshot
{
    public ushort ProtocolVersion { get; }
    public CapabilityDisplay Display { get; }
    public FeatureAvailability<AssetsCapability>? Assets { get; }
    public FeatureAvailability<ScreenCapability>? Screen { get; }
    public FeatureAvailability<UpdateCapability>? Update { get; }
    public FeatureAvailability<LEDCapability>? LED { get; }

    public ReplacementCapabilitySnapshot(ushort protocolVersion, CapabilityDisplay display,
        FeatureAvailability<AssetsCapability>? assets,
        FeatureAvailability<ScreenCapability>? screen,
        FeatureAvailability<UpdateCapability>? update,
        FeatureAvailability<LEDCapability>? led)
    {
        ProtocolVersion = protocolVersion;
        Display = display;
        Assets = assets;
        Screen = screen;
        Update = update;
        LED = led;
    }

    public static ReplacementCapabilitySnapshot Decode(byte[] data)
    {
        var root = BoundedJSON.Object(data);
        BoundedJSON.ExactKeys(root, new HashSet<string> { "display", "event", "features", "protocol" }, "vk_capabilities");

        if (BoundedJSON.String(root["event"]) != "vk_capabilities")
            throw new ProtocolException("Invalid envelope: event");
        if (BoundedJSON.UInt(root["protocol"]) != 1)
            throw new ProtocolException("Invalid envelope: protocol");

        var displayObject = BoundedJSON.Dictionary(root["display"])
            ?? throw new ProtocolException("Invalid value: display");
        var featureObject = BoundedJSON.Dictionary(root["features"])
            ?? throw new ProtocolException("Invalid value: features");

        BoundedJSON.ExactKeys(displayObject, new HashSet<string> { "format", "height", "width" }, "display");

        ushort width = BoundedJSON.UInt16(displayObject["width"])
            ?? throw new ProtocolException("Invalid value: display.width");
        ushort height = BoundedJSON.UInt16(displayObject["height"])
            ?? throw new ProtocolException("Invalid value: display.height");
        string format = BoundedJSON.String(displayObject["format"])
            ?? throw new ProtocolException("Invalid value: display.format");

        var assets = featureObject.ContainsKey("assets") ? DecodeAssets(featureObject["assets"]) : null;
        var screen = featureObject.ContainsKey("screen") ? DecodeScreen(featureObject["screen"]) : null;
        var update = featureObject.ContainsKey("update") ? DecodeUpdate(featureObject["update"]) : null;
        var led = featureObject.ContainsKey("led") ? DecodeLED(featureObject["led"]) : null;

        // Cross-feature invariant: screen.maxAssets <= assets.maxAssets
        if (screen is FeatureAvailability<ScreenCapability>.Available screenAvail &&
            assets is FeatureAvailability<AssetsCapability>.Available assetsAvail)
        {
            if (screenAvail.Capability.MaxAssets > assetsAvail.Capability.MaxAssets)
                throw new ProtocolException("Cross-feature invariant violated");
        }

        return new ReplacementCapabilitySnapshot(1,
            new CapabilityDisplay(width, height, format), assets, screen, update, led);
    }

    private static FeatureAvailability<AssetsCapability> DecodeAssets(object? value)
    {
        var obj = BoundedJSON.Dictionary(value) ?? throw new ProtocolException("Invalid value: assets");
        if (BoundedJSON.UInt16(obj["version"]) != 1)
            throw new ProtocolException("Invalid value: assets.version");
        bool? available = BoundedJSON.Bool(obj["available"]);
        if (available != true && available != false)
            throw new ProtocolException("Invalid value: assets.available");

        if (available == false)
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "reason", "version" }, "assets unavailable");
            string reason = BoundedJSON.String(obj["reason"]) ?? throw new ProtocolException("Invalid value: assets.reason");
            if (reason != "display_acceptance_required" && reason != "storage_unavailable" &&
                reason != "integrity_unavailable" && reason != "policy_blocked")
                throw new ProtocolException("Invalid value: assets.reason");
            return new FeatureAvailability<AssetsCapability>.Unavailable(1, reason);
        }

        var keys = new HashSet<string> { "available", "chunk_bytes", "decoder_scratch_bytes", "encodings",
            "free_bytes", "management", "max_active_decoded_bytes", "max_asset_bytes", "max_assets",
            "max_frame_ms", "max_frames", "min_frame_ms", "reserve_bytes", "revision",
            "storage_state", "upload_max_bytes", "version" };
        BoundedJSON.ExactKeys(obj, keys, "assets");

        if (BoundedJSON.Bool(obj["management"]) != true)
            throw new ProtocolException("Invalid value: assets.management");
        var storage = StorageStateExtensions.FromWire(BoundedJSON.String(obj["storage_state"]) ?? "");
        uint free = BoundedJSON.UInt32(obj["free_bytes"]) ?? throw new ProtocolException("Invalid value: assets.free_bytes");
        uint reserve = BoundedJSON.PositiveUInt32(obj["reserve_bytes"]) ?? throw new ProtocolException("Invalid value: assets.reserve_bytes");
        uint upload = BoundedJSON.UInt32(obj["upload_max_bytes"]) ?? throw new ProtocolException("Invalid value: assets.upload_max_bytes");
        uint maxAsset = BoundedJSON.PositiveUInt32(obj["max_asset_bytes"]) ?? throw new ProtocolException("Invalid value: assets.max_asset_bytes");
        ushort chunk = BoundedJSON.RangeUInt16(obj["chunk_bytes"], 1, 4084) ?? throw new ProtocolException("Invalid value: assets.chunk_bytes");
        ushort maxAssets = BoundedJSON.RangeUInt16(obj["max_assets"], 1, 1024) ?? throw new ProtocolException("Invalid value: assets.max_assets");
        ushort maxFrames = BoundedJSON.PositiveUInt16(obj["max_frames"]) ?? throw new ProtocolException("Invalid value: assets.max_frames");
        ushort minFrame = BoundedJSON.PositiveUInt16(obj["min_frame_ms"]) ?? throw new ProtocolException("Invalid value: assets.min_frame_ms");
        ushort maxFrame = BoundedJSON.PositiveUInt16(obj["max_frame_ms"]) ?? throw new ProtocolException("Invalid value: assets.max_frame_ms");
        if (minFrame > maxFrame) throw new ProtocolException("Invalid value: assets.min_frame_ms > max_frame_ms");
        uint decoded = BoundedJSON.PositiveUInt32(obj["max_active_decoded_bytes"]) ?? throw new ProtocolException("Invalid value: assets.max_active_decoded_bytes");
        uint scratch = BoundedJSON.RangeUInt32(obj["decoder_scratch_bytes"], 1, decoded) ?? throw new ProtocolException("Invalid value: assets.decoder_scratch_bytes");
        var encodings = BoundedJSON.StringArray(obj["encodings"]);
        if (encodings == null || encodings.Length != 2 || encodings[0] != "raw" || encodings[1] != "row_rle")
            throw new ProtocolException("Invalid value: assets.encodings");
        uint revision = BoundedJSON.UInt32(obj["revision"]) ?? throw new ProtocolException("Invalid value: assets.revision");

        if (upload > maxAsset) throw new ProtocolException("Invalid value: assets.upload > max_asset");
        uint availableSpace = free > reserve ? free - reserve : 0;
        if (upload > availableSpace) throw new ProtocolException("Invalid value: assets.upload > available");
        if (storage != StorageState.Ready && storage != StorageState.Busy && free != 0)
            throw new ProtocolException("Invalid value: assets.storage_state vs free_bytes");
        if (storage != StorageState.Ready && upload != 0)
            throw new ProtocolException("Invalid value: assets.storage_state vs upload_max_bytes");

        return new FeatureAvailability<AssetsCapability>.Available(new AssetsCapability(
            true, storage, free, reserve, upload, maxAsset, chunk, maxAssets, maxFrames,
            minFrame, maxFrame, decoded, scratch, encodings, revision));
    }

    private static FeatureAvailability<ScreenCapability> DecodeScreen(object? value)
    {
        var obj = BoundedJSON.Dictionary(value) ?? throw new ProtocolException("Invalid value: screen");
        if (BoundedJSON.UInt16(obj["version"]) != 1)
            throw new ProtocolException("Invalid value: screen.version");
        bool? available = BoundedJSON.Bool(obj["available"]);
        if (available == null) throw new ProtocolException("Invalid value: screen.available");

        if (available == false)
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "reason", "version" }, "screen unavailable");
            string reason = BoundedJSON.String(obj["reason"]) ?? throw new ProtocolException("Invalid value: screen.reason");
            if (reason != "display_acceptance_required" && reason != "panel_unavailable" &&
                reason != "model_unavailable" && reason != "storage_unavailable" && reason != "policy_blocked")
                throw new ProtocolException("Invalid value: screen.reason");
            return new FeatureAvailability<ScreenCapability>.Unavailable(1, reason);
        }

        var keys = new HashSet<string> { "available", "configured", "fonts", "max_assets", "max_commit_bytes",
            "max_depth", "max_fonts", "max_json_tokens", "max_layout_bytes", "max_objects",
            "max_pet_states", "max_string_bytes", "max_widget_value_bytes", "max_widgets",
            "modes", "revision", "version" };
        BoundedJSON.ExactKeys(obj, keys, "screen");

        var modes = BoundedJSON.StringArray(obj["modes"]);
        if (modes == null || modes.Length == 0)
            throw new ProtocolException("Invalid value: screen.modes");
        // Verify modes are a subset sorted
        var allModes = new[] { "image", "pet", "dashboard", "custom" };
        var filtered = Array.FindAll(allModes, m => Array.Exists(modes, x => x == m));
        if (modes.Length != filtered.Length) throw new ProtocolException("Invalid value: screen.modes");
        if (new HashSet<string>(modes).Count != modes.Length) throw new ProtocolException("Invalid value: screen.modes");

        ushort commit = BoundedJSON.RangeUInt16(obj["max_commit_bytes"], 1, 4092) ?? throw new ProtocolException("Invalid value: screen.max_commit_bytes");
        ushort layout = BoundedJSON.RangeUInt16(obj["max_layout_bytes"], 1, commit) ?? throw new ProtocolException("Invalid value: screen.max_layout_bytes");
        ushort maxAssets = BoundedJSON.RangeUInt16(obj["max_assets"], 1, 1024) ?? throw new ProtocolException("Invalid value: screen.max_assets");
        ushort objects = BoundedJSON.PositiveUInt16(obj["max_objects"]) ?? throw new ProtocolException("Invalid value: screen.max_objects");
        byte depth = BoundedJSON.RangeByte(obj["max_depth"], 1, 8) ?? throw new ProtocolException("Invalid value: screen.max_depth");
        ushort widgets = BoundedJSON.PositiveUInt16(obj["max_widgets"]) ?? throw new ProtocolException("Invalid value: screen.max_widgets");
        ushort maxFonts = BoundedJSON.PositiveUInt16(obj["max_fonts"]) ?? throw new ProtocolException("Invalid value: screen.max_fonts");
        byte petStates = BoundedJSON.RangeByte(obj["max_pet_states"], 1, 6) ?? throw new ProtocolException("Invalid value: screen.max_pet_states");
        ushort strings = BoundedJSON.RangeUInt16(obj["max_string_bytes"], 1, 512) ?? throw new ProtocolException("Invalid value: screen.max_string_bytes");
        ushort tokens = BoundedJSON.RangeUInt16(obj["max_json_tokens"], 32, 1024) ?? throw new ProtocolException("Invalid value: screen.max_json_tokens");
        ushort widgetBytes = BoundedJSON.RangeUInt16(obj["max_widget_value_bytes"], 1, 512) ?? throw new ProtocolException("Invalid value: screen.max_widget_value_bytes");
        uint revision = BoundedJSON.UInt32(obj["revision"]) ?? throw new ProtocolException("Invalid value: screen.revision");
        bool configured = BoundedJSON.Bool(obj["configured"]) ?? throw new ProtocolException("Invalid value: screen.configured");
        if (configured && revision == 0) throw new ProtocolException("Invalid value: screen.configured vs revision");
        if (!configured && revision != 0) throw new ProtocolException("Invalid value: screen.configured vs revision");

        var fontValues = BoundedJSON.Array(obj["fonts"]) ?? throw new ProtocolException("Invalid value: screen.fonts");
        if (fontValues.Length == 0) throw new ProtocolException("Invalid value: screen.fonts");
        if (fontValues.Length > maxFonts) throw new ProtocolException("Invalid value: screen.fonts count");

        var fonts = new List<ScreenFontCapability>();
        foreach (var fv in fontValues)
        {
            var font = BoundedJSON.Dictionary(fv) ?? throw new ProtocolException("Invalid value: screen.fonts");
            BoundedJSON.ExactKeys(font, new HashSet<string> { "id", "metrics_sha256", "version" }, "font");
            string id = BoundedJSON.String(font["id"]) ?? throw new ProtocolException("Invalid value: screen.font.id");
            if (!BoundedJSON.IsValidIdentifier(id)) throw new ProtocolException("Invalid value: screen.font.id");
            ushort version = BoundedJSON.PositiveUInt16(font["version"]) ?? throw new ProtocolException("Invalid value: screen.font.version");
            string hash = BoundedJSON.String(font["metrics_sha256"]) ?? throw new ProtocolException("Invalid value: screen.font.metrics_sha256");
            if (!BoundedJSON.IsValidSHA(hash)) throw new ProtocolException("Invalid value: screen.font.metrics_sha256");
            fonts.Add(new ScreenFontCapability(id, version, hash));
        }

        // Font IDs must be sorted and unique
        var fontIds = new List<string>();
        foreach (var f in fonts) fontIds.Add(f.ID);
        var sortedIds = new List<string>(fontIds);
        sortedIds.Sort();
        if (!fontIds.SequenceEqual(sortedIds)) throw new ProtocolException("Invalid value: screen.fonts order");
        if (new HashSet<string>(fontIds).Count != fontIds.Count) throw new ProtocolException("Invalid value: screen.fonts unique");

        return new FeatureAvailability<ScreenCapability>.Available(new ScreenCapability(
            modes, commit, layout, maxAssets, objects, depth, widgets, maxFonts,
            petStates, strings, tokens, widgetBytes, revision, configured, fonts));
    }

    private static FeatureAvailability<UpdateCapability> DecodeUpdate(object? value)
    {
        var obj = BoundedJSON.Dictionary(value) ?? throw new ProtocolException("Invalid value: update");
        if (BoundedJSON.UInt16(obj["version"]) != 1)
            throw new ProtocolException("Invalid value: update.version");
        bool? available = BoundedJSON.Bool(obj["available"]);
        if (available == null) throw new ProtocolException("Invalid value: update.available");

        if (available == false)
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "reason", "version" }, "update unavailable");
            string reason = BoundedJSON.String(obj["reason"]) ?? throw new ProtocolException("Invalid value: update.reason");
            if (reason != "bootloader_migration_required" && reason != "busy" && reason != "wrong_running_slot" &&
                reason != "target_unavailable" && reason != "integrity_unavailable" && reason != "policy_blocked")
                throw new ProtocolException("Invalid value: update.reason");
            return new FeatureAvailability<UpdateCapability>.Unavailable(1, reason);
        }

        BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "chunk_bytes", "max_image_bytes",
            "rollback", "staged_metadata", "target", "version" }, "update");

        ushort chunk = BoundedJSON.RangeUInt16(obj["chunk_bytes"], 1, 512) ?? throw new ProtocolException("Invalid value: update.chunk_bytes");
        uint maximum = BoundedJSON.PositiveUInt32(obj["max_image_bytes"]) ?? throw new ProtocolException("Invalid value: update.max_image_bytes");
        string target = BoundedJSON.String(obj["target"]) ?? throw new ProtocolException("Invalid value: update.target");
        if (target != "ota_0" && target != "ota_1") throw new ProtocolException("Invalid value: update.target");
        if (BoundedJSON.String(obj["staged_metadata"]) != "ram_epoch") throw new ProtocolException("Invalid value: update.staged_metadata");
        if (BoundedJSON.String(obj["rollback"]) != "bootloader_pending_verify") throw new ProtocolException("Invalid value: update.rollback");

        return new FeatureAvailability<UpdateCapability>.Available(
            new UpdateCapability(chunk, maximum, target, "ram_epoch", "bootloader_pending_verify"));
    }

    private static FeatureAvailability<LEDCapability> DecodeLED(object? value)
    {
        var obj = BoundedJSON.Dictionary(value) ?? throw new ProtocolException("Invalid value: led");
        if (BoundedJSON.UInt16(obj["version"]) != 1)
            throw new ProtocolException("Invalid value: led.version");
        bool? available = BoundedJSON.Bool(obj["available"]);
        if (available == null) throw new ProtocolException("Invalid value: led.available");

        if (available == false)
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "reason", "version" }, "led unavailable");
            string reason = BoundedJSON.String(obj["reason"]) ?? throw new ProtocolException("Invalid value: led.reason");
            if (reason != "calibration_required" && reason != "hardware_failed" && reason != "tainted")
                throw new ProtocolException("Invalid value: led.reason");
            return new FeatureAvailability<LEDCapability>.Unavailable(1, reason);
        }

        BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "color_model", "key_pixels",
            "max_brightness", "max_frame_channel_sum", "pixel_count", "strip_count",
            "strip_first", "tick_ms", "version", "wire_order" }, "led");

        if (BoundedJSON.Byte(obj["pixel_count"]) != 17) throw new ProtocolException("Invalid value: led.pixel_count");
        if (BoundedJSON.Byte(obj["strip_first"]) != 4) throw new ProtocolException("Invalid value: led.strip_first");
        if (BoundedJSON.Byte(obj["strip_count"]) != 13) throw new ProtocolException("Invalid value: led.strip_count");
        if (BoundedJSON.String(obj["color_model"]) != "rgb8") throw new ProtocolException("Invalid value: led.color_model");
        if (BoundedJSON.String(obj["wire_order"]) != "grb") throw new ProtocolException("Invalid value: led.wire_order");
        if (BoundedJSON.Byte(obj["tick_ms"]) != 30) throw new ProtocolException("Invalid value: led.tick_ms");
        byte brightness = BoundedJSON.RangeByte(obj["max_brightness"], 1, 255) ?? throw new ProtocolException("Invalid value: led.max_brightness");
        ushort frameSum = BoundedJSON.RangeUInt16(obj["max_frame_channel_sum"], 1, 13005) ?? throw new ProtocolException("Invalid value: led.max_frame_channel_sum");
        var pixels = BoundedJSON.Dictionary(obj["key_pixels"]) ?? throw new ProtocolException("Invalid value: led.key_pixels");

        BoundedJSON.ExactKeys(pixels, new HashSet<string> { "k1", "k2", "k3", "k4" }, "led.key_pixels");
        var mapping = new Dictionary<string, byte>();
        foreach (var key in new[] { "k1", "k2", "k3", "k4" })
        {
            byte pixel = BoundedJSON.RangeByte(pixels[key], 0, 3) ?? throw new ProtocolException("Invalid value: led.key_pixels");
            mapping[key] = pixel;
        }

        var values = new HashSet<byte>(mapping.Values);
        if (values.Count != 4) throw new ProtocolException("Invalid value: led.key_pixels");

        return new FeatureAvailability<LEDCapability>.Available(
            new LEDCapability(mapping, brightness, frameSum));
    }
}

/// <summary>
/// Session context for the replacement protocol, tracking epoch and snapshot generations.
/// </summary>
public sealed class ReplacementSessionContext
{
    public ulong EpochGeneration { get; }
    public ulong SnapshotGeneration { get; }
    public ReplacementCapabilitySnapshot Snapshot { get; }

    public ReplacementSessionContext(ulong epochGeneration, ulong snapshotGeneration, ReplacementCapabilitySnapshot snapshot)
    {
        EpochGeneration = epochGeneration;
        SnapshotGeneration = snapshotGeneration;
        Snapshot = snapshot;
    }
}
