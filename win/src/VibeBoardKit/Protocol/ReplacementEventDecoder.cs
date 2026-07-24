using System;
using System.Collections.Generic;
using System.Text;

namespace VibeBoardKit.Protocol;

// Note: LEDProtocolEvent is defined in LEDProtocol.cs
// WidgetProtocolEvent is defined in WidgetProtocol.cs

/// <summary>
/// Asset list entry from a vk_asset_page event.
/// </summary>
public sealed class AssetListEntry
{
    public string SHA256 { get; }
    public uint TotalBytes { get; }
    public AssetKind Kind { get; }
    public bool Referenced { get; }

    public AssetListEntry(string sha256, uint totalBytes, AssetKind kind, bool referenced)
    {
        SHA256 = sha256;
        TotalBytes = totalBytes;
        Kind = kind;
        Referenced = referenced;
    }
}

/// <summary>
/// Replacement asset events from the device.
/// </summary>
public abstract class ReplacementAssetEvent
{
    private ReplacementAssetEvent() { }

    public sealed class StorageFormatted : ReplacementAssetEvent
    {
        public uint Revision { get; }
        public StorageFormatted(uint revision) { Revision = revision; }
    }

    public sealed class Ready : ReplacementAssetEvent
    {
        public uint TransferID { get; }
        public string SHA256 { get; }
        public uint TotalBytes { get; }
        public AssetKind Kind { get; }
        public uint NextOffset { get; }
        public ushort ChunkBytes { get; }
        public Ready(uint transferID, string sha256, uint totalBytes, AssetKind kind, uint nextOffset, ushort chunkBytes)
        { TransferID = transferID; SHA256 = sha256; TotalBytes = totalBytes; Kind = kind; NextOffset = nextOffset; ChunkBytes = chunkBytes; }
    }

    public sealed class Progress : ReplacementAssetEvent
    {
        public uint TransferID { get; }
        public uint NextOffset { get; }
        public Progress(uint transferID, uint nextOffset) { TransferID = transferID; NextOffset = nextOffset; }
    }

    public sealed class Stored : ReplacementAssetEvent
    {
        public uint TransferID { get; }
        public string SHA256 { get; }
        public uint TotalBytes { get; }
        public AssetKind Kind { get; }
        public Stored(uint transferID, string sha256, uint totalBytes, AssetKind kind)
        { TransferID = transferID; SHA256 = sha256; TotalBytes = totalBytes; Kind = kind; }
    }

    public sealed class Aborted : ReplacementAssetEvent
    {
        public uint TransferID { get; }
        public Aborted(uint transferID) { TransferID = transferID; }
    }

    public sealed class Page : ReplacementAssetEvent
    {
        public uint SnapshotID { get; }
        public uint Cursor { get; }
        public List<AssetListEntry> Entries { get; }
        public uint? NextCursor { get; }
        public uint Revision { get; }
        public Page(uint snapshotID, uint cursor, List<AssetListEntry> entries, uint? nextCursor, uint revision)
        { SnapshotID = snapshotID; Cursor = cursor; Entries = entries; NextCursor = nextCursor; Revision = revision; }
    }

    public sealed class Deleted : ReplacementAssetEvent
    {
        public string SHA256 { get; }
        public uint Revision { get; }
        public Deleted(string sha256, uint revision) { SHA256 = sha256; Revision = revision; }
    }

    public sealed class Invalidated : ReplacementAssetEvent
    {
        public uint TransferID { get; }
        public Invalidated(uint transferID) { TransferID = transferID; }
    }
}

/// <summary>
/// Replacement screen events from the device.
/// </summary>
public abstract class ReplacementScreenEvent
{
    private ReplacementScreenEvent() { }

    public sealed class State : ReplacementScreenEvent
    {
        public bool Configured { get; }
        public ScreenMode? Mode { get; }
        public uint Revision { get; }
        public string? AssetsManifestSHA256 { get; }
        public string? ScreenManifestSHA256 { get; }
        public State(bool configured, ScreenMode? mode, uint revision,
            string? assetsManifestSHA256, string? screenManifestSHA256)
        { Configured = configured; Mode = mode; Revision = revision; AssetsManifestSHA256 = assetsManifestSHA256; ScreenManifestSHA256 = screenManifestSHA256; }
    }

    public sealed class Committed : ReplacementScreenEvent
    {
        public string AssetsManifestSHA256 { get; }
        public uint PreviousRevision { get; }
        public uint Revision { get; }
        public string ScreenManifestSHA256 { get; }
        public Committed(string assetsManifestSHA256, uint previousRevision, uint revision, string screenManifestSHA256)
        { AssetsManifestSHA256 = assetsManifestSHA256; PreviousRevision = previousRevision; Revision = revision; ScreenManifestSHA256 = screenManifestSHA256; }
    }
}

/// <summary>
/// Replacement error event from the device.
/// </summary>
public sealed class ReplacementErrorEvent
{
    public string Operation { get; }
    public string Code { get; }
    public uint? TransferID { get; }
    public uint? NextOffset { get; }
    public string? SHA256 { get; }
    public string? Message { get; }

    public ReplacementErrorEvent(string operation, string code, uint? transferID,
        uint? nextOffset, string? sha256, string? message)
    {
        Operation = operation;
        Code = code;
        TransferID = transferID;
        NextOffset = nextOffset;
        SHA256 = sha256;
        Message = message;
    }
}

/// <summary>
/// Top-level replacement protocol event: asset, screen, widget, LED, or error.
/// </summary>
public abstract class ReplacementProtocolEvent
{
    private ReplacementProtocolEvent() { }

    public sealed class Asset : ReplacementProtocolEvent
    {
        public ReplacementAssetEvent Event { get; }
        public Asset(ReplacementAssetEvent @event) { Event = @event; }
    }

    public sealed class Screen : ReplacementProtocolEvent
    {
        public ReplacementScreenEvent Event { get; }
        public Screen(ReplacementScreenEvent @event) { Event = @event; }
    }

    public sealed class Widget : ReplacementProtocolEvent
    {
        public WidgetProtocolEvent Event { get; }
        public Widget(WidgetProtocolEvent @event) { Event = @event; }
    }

    public sealed class LED : ReplacementProtocolEvent
    {
        public LEDProtocolEvent Event { get; }
        public LED(LEDProtocolEvent @event) { Event = @event; }
    }

    public sealed class Error : ReplacementProtocolEvent
    {
        public ReplacementErrorEvent Event { get; }
        public Error(ReplacementErrorEvent @event) { Event = @event; }
    }
}

/// <summary>
/// Decoder for replacement protocol events from the device.
/// Parses vk_* JSON events with strict validation.
/// </summary>
public static class ReplacementEventDecoder
{
    private static readonly HashSet<string> AssetErrorCodes = new()
    {
        "invalid_request", "unavailable", "wrong_epoch", "busy", "conflict",
        "not_found", "bad_offset", "bad_size", "bad_hash", "kind_mismatch",
        "write_failed", "incomplete", "invalid_asset", "timeout", "no_space",
        "referenced", "revision_conflict", "snapshot_expired", "partition_mismatch",
        "not_erased", "format_failed", "internal"
    };

    private static readonly HashSet<string> ScreenErrorCodes = new()
    {
        "invalid_request", "unavailable", "wrong_epoch", "revision_conflict",
        "conflict", "invalid_manifest", "missing_asset", "font_mismatch",
        "limit_exceeded", "allocation_failed", "render_failed", "internal"
    };

    private static readonly HashSet<string> InputErrorCodes = new()
    {
        "invalid_request", "wrong_epoch", "busy", "input_queue_overflow",
        "audio_start_failed", "audio_stop_failed", "audio_runtime_failed", "tainted"
    };

    public static ReplacementProtocolEvent Decode(byte[] body)
    {
        var obj = BoundedJSON.Object(body);
        string? eventName = BoundedJSON.String(obj["event"]);
        if (eventName == null)
            throw new ProtocolException("Invalid value: event");

        return eventName switch
        {
            "vk_storage_formatted" => DecodeStorageFormatted(obj),
            "vk_asset_ready" => DecodeAssetReady(obj),
            "vk_asset_progress" => DecodeAssetProgress(obj),
            "vk_asset_stored" => DecodeAssetStored(obj),
            "vk_asset_aborted" => DecodeAssetAborted(obj),
            "vk_asset_page" => DecodeAssetPage(obj),
            "vk_screen_state" => DecodeScreenState(obj),
            "vk_screen_committed" => DecodeScreenCommitted(obj),
            "vk_error" => DecodeError(obj),
            "vk_led_state" => new ReplacementProtocolEvent.LED(LEDProtocolCodec.Decode(body)),
            "vk_widget_applied" => new ReplacementProtocolEvent.Widget(WidgetProtocolCodec.Decode(body)),
            _ => throw new ProtocolException($"Invalid value: event")
        };
    }

    private static ReplacementProtocolEvent DecodeStorageFormatted(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "event", "revision" }, "vk_storage_formatted");
        uint revision = RequireUInt32(obj, "revision");
        return new ReplacementProtocolEvent.Asset(new ReplacementAssetEvent.StorageFormatted(revision));
    }

    private static ReplacementProtocolEvent DecodeAssetReady(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "chunk_bytes", "event", "kind", "next_offset", "sha256", "total_bytes", "transfer_id" }, "vk_asset_ready");
        uint transferID = RequireNonzeroUInt32(obj, "transfer_id");
        string sha256 = RequireSHA(obj, "sha256");
        uint totalBytes = RequireNonzeroUInt32(obj, "total_bytes");
        AssetKind kind = RequireKind(obj);
        uint nextOffset = RequireUInt32(obj, "next_offset");
        ushort chunkBytes = BoundedJSON.RangeUInt16(obj["chunk_bytes"], 1, 4084)
            ?? throw new ProtocolException("Invalid value: chunk_bytes");

        return new ReplacementProtocolEvent.Asset(
            new ReplacementAssetEvent.Ready(transferID, sha256, totalBytes, kind, nextOffset, chunkBytes));
    }

    private static ReplacementProtocolEvent DecodeAssetProgress(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "event", "next_offset", "transfer_id" }, "vk_asset_progress");
        uint transferID = RequireNonzeroUInt32(obj, "transfer_id");
        uint nextOffset = RequireUInt32(obj, "next_offset");
        return new ReplacementProtocolEvent.Asset(
            new ReplacementAssetEvent.Progress(transferID, nextOffset));
    }

    private static ReplacementProtocolEvent DecodeAssetStored(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "event", "kind", "sha256", "total_bytes", "transfer_id" }, "vk_asset_stored");
        uint transferID = RequireNonzeroUInt32(obj, "transfer_id");
        string sha256 = RequireSHA(obj, "sha256");
        uint totalBytes = RequireNonzeroUInt32(obj, "total_bytes");
        AssetKind kind = RequireKind(obj);
        return new ReplacementProtocolEvent.Asset(
            new ReplacementAssetEvent.Stored(transferID, sha256, totalBytes, kind));
    }

    private static ReplacementProtocolEvent DecodeAssetAborted(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "event", "transfer_id" }, "vk_asset_aborted");
        uint transferID = RequireNonzeroUInt32(obj, "transfer_id");
        return new ReplacementProtocolEvent.Asset(
            new ReplacementAssetEvent.Aborted(transferID));
    }

    private static ReplacementProtocolEvent DecodeAssetPage(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "cursor", "entries", "event", "next_cursor", "revision", "snapshot_id" }, "vk_asset_page");

        uint snapshotID = RequireNonzeroUInt32(obj, "snapshot_id");
        uint cursor = RequireUInt32(obj, "cursor");
        uint revision = RequireUInt32(obj, "revision");

        var entriesArr = BoundedJSON.Array(obj["entries"]) ?? throw new ProtocolException("Invalid value: entries");
        var entries = new List<AssetListEntry>();
        foreach (var value in entriesArr)
        {
            var entry = BoundedJSON.Dictionary(value) ?? throw new ProtocolException("Invalid value: entry");
            BoundedJSON.ExactKeys(entry, new HashSet<string> { "kind", "referenced", "sha256", "total_bytes" }, "entry");
            bool? referenced = BoundedJSON.Bool(entry["referenced"]);
            if (referenced == null) throw new ProtocolException("Invalid value: referenced");
            string sha = RequireSHA(entry, "sha256");
            uint total = RequireNonzeroUInt32(entry, "total_bytes");
            AssetKind kind = RequireKind(entry);
            entries.Add(new AssetListEntry(sha, total, kind, referenced.Value));
        }

        uint? nextCursor = null;
        if (obj["next_cursor"] != null)
            nextCursor = RequireNonzeroUInt32(obj, "next_cursor");

        return new ReplacementProtocolEvent.Asset(
            new ReplacementAssetEvent.Page(snapshotID, cursor, entries, nextCursor, revision));
    }

    private static ReplacementProtocolEvent DecodeScreenState(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "assets_manifest_sha256", "configured", "configured_mode", "event", "revision", "screen_manifest_sha256" }, "vk_screen_state");

        bool? configured = BoundedJSON.Bool(obj["configured"]);
        if (configured == null) throw new ProtocolException("Invalid value: configured");
        uint revision = RequireUInt32(obj, "revision");

        if (!configured.Value)
        {
            if (obj["configured_mode"] != null || obj["assets_manifest_sha256"] != null ||
                obj["screen_manifest_sha256"] != null || revision != 0)
                throw new ProtocolException("Invalid value: screen_state");
            return new ReplacementProtocolEvent.Screen(
                new ReplacementScreenEvent.State(false, null, 0, null, null));
        }

        string? modeRaw = BoundedJSON.String(obj["configured_mode"]);
        if (modeRaw == null) throw new ProtocolException("Invalid value: configured_mode");
        ScreenMode mode = ScreenModeExtensions.FromWire(modeRaw);

        string assetsSHA = RequireSHA(obj, "assets_manifest_sha256");
        string screenSHA = RequireSHA(obj, "screen_manifest_sha256");
        if (revision == 0) throw new ProtocolException("Invalid value: revision");

        return new ReplacementProtocolEvent.Screen(
            new ReplacementScreenEvent.State(true, mode, revision, assetsSHA, screenSHA));
    }

    private static ReplacementProtocolEvent DecodeScreenCommitted(Dictionary<string, object?> obj)
    {
        BoundedJSON.ExactKeys(obj, new HashSet<string> { "assets_manifest_sha256", "event", "previous_revision", "revision", "screen_manifest_sha256" }, "vk_screen_committed");

        string assetsSHA = RequireSHA(obj, "assets_manifest_sha256");
        uint previousRevision = RequireUInt32(obj, "previous_revision");
        uint revision = RequireNonzeroUInt32(obj, "revision");
        string screenSHA = RequireSHA(obj, "screen_manifest_sha256");

        return new ReplacementProtocolEvent.Screen(
            new ReplacementScreenEvent.Committed(assetsSHA, previousRevision, revision, screenSHA));
    }

    private static ReplacementProtocolEvent DecodeError(Dictionary<string, object?> obj)
    {
        string? operation = BoundedJSON.String(obj["operation"]);
        string? code = BoundedJSON.String(obj["code"]);
        if (operation == null || code == null)
            throw new ProtocolException("Invalid keys: vk_error");

        var baseKeys = new HashSet<string> { "event", "operation", "code" };
        var assetOptionalKeys = new HashSet<string> { "transfer_id", "next_offset", "sha256", "message" };
        HashSet<string> allowed;
        HashSet<string> validCodes;

        switch (operation)
        {
            case "asset":
            case "storage":
                allowed = new HashSet<string>(baseKeys);
                allowed.UnionWith(assetOptionalKeys);
                validCodes = AssetErrorCodes;
                break;
            case "screen":
                allowed = new HashSet<string>(baseKeys);
                allowed.Add("message");
                validCodes = ScreenErrorCodes;
                break;
            case "input":
                allowed = new HashSet<string>(baseKeys);
                validCodes = InputErrorCodes;
                break;
            default:
                throw new ProtocolException("Invalid value: operation");
        }

        var objKeys = new HashSet<string>(obj.Keys);
        if (!baseKeys.IsSubsetOf(objKeys) || !objKeys.IsSubsetOf(allowed))
            throw new ProtocolException($"Invalid keys: vk_error.{operation}");

        if (!validCodes.Contains(code))
            throw new ProtocolException("Invalid value: error");

        uint? transferID = null;
        if (obj["transfer_id"] != null)
            transferID = RequireNonzeroUInt32(obj, "transfer_id");

        uint? nextOffset = null;
        if (obj["next_offset"] != null)
            nextOffset = RequireUInt32(obj, "next_offset");

        string? sha256 = null;
        if (obj["sha256"] != null)
        {
            sha256 = BoundedJSON.String(obj["sha256"]);
            if (sha256 == null || !BoundedJSON.IsValidSHA256(sha256))
                throw new ProtocolException("Invalid value: sha256");
        }

        string? message = BoundedJSON.String(obj["message"]);
        if (obj["message"] != null && (message == null || Encoding.UTF8.GetByteCount(message) > 96))
            throw new ProtocolException("Invalid value: message");

        return new ReplacementProtocolEvent.Error(
            new ReplacementErrorEvent(operation, code, transferID, nextOffset, sha256, message));
    }

    // --- Helper methods ---

    private static uint RequireUInt32(Dictionary<string, object?> obj, string key)
    {
        return BoundedJSON.UInt32(obj[key]) ?? throw new ProtocolException($"Invalid value: {key}");
    }

    private static uint RequireNonzeroUInt32(Dictionary<string, object?> obj, string key)
    {
        uint v = RequireUInt32(obj, key);
        if (v == 0) throw new ProtocolException($"Invalid value: {key}");
        return v;
    }

    private static string RequireSHA(Dictionary<string, object?> obj, string key)
    {
        string? s = BoundedJSON.String(obj[key]);
        if (s == null || !BoundedJSON.IsValidSHA256(s))
            throw new ProtocolException($"Invalid value: {key}");
        return s;
    }

    private static AssetKind RequireKind(Dictionary<string, object?> obj)
    {
        string? raw = BoundedJSON.String(obj["kind"]);
        if (raw == null) throw new ProtocolException("Invalid value: kind");
        return raw switch
        {
            "image" => AssetKind.Image,
            "animation" => AssetKind.Animation,
            "glyph_bitmap" => AssetKind.GlyphBitmap,
            _ => throw new ProtocolException("Invalid value: kind")
        };
    }
}
