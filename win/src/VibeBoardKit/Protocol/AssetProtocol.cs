using System;
using System.Text;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Asset kind categories understood by the device.
/// </summary>
public enum AssetKind
{
    Image,
    Animation,
    GlyphBitmap
}

public static class AssetKindExtensions
{
    public static string WireString(this AssetKind kind) => kind switch
    {
        AssetKind.Image => "image",
        AssetKind.Animation => "animation",
        AssetKind.GlyphBitmap => "glyph_bitmap",
        _ => throw new ProtocolException($"Unknown asset kind: {kind}")
    };
}

/// <summary>
/// Commands for asset management over the replacement protocol.
/// </summary>
public abstract class AssetCommand
{
    private AssetCommand() { }

    public sealed class StorageFormat : AssetCommand { }

    public sealed class Begin : AssetCommand
    {
        public uint TransferID { get; }
        public string SHA256 { get; }
        public uint TotalBytes { get; }
        public AssetKind Kind { get; }
        public Begin(uint transferID, string sha256, uint totalBytes, AssetKind kind)
        { TransferID = transferID; SHA256 = sha256; TotalBytes = totalBytes; Kind = kind; }
    }

    public sealed class Query : AssetCommand
    {
        public uint TransferID { get; }
        public Query(uint transferID) { TransferID = transferID; }
    }

    public sealed class End : AssetCommand
    {
        public uint TransferID { get; }
        public string SHA256 { get; }
        public uint TotalBytes { get; }
        public AssetKind Kind { get; }
        public End(uint transferID, string sha256, uint totalBytes, AssetKind kind)
        { TransferID = transferID; SHA256 = sha256; TotalBytes = totalBytes; Kind = kind; }
    }

    public sealed class Abort : AssetCommand
    {
        public uint TransferID { get; }
        public Abort(uint transferID) { TransferID = transferID; }
    }

    public sealed class List : AssetCommand
    {
        public uint SnapshotID { get; }
        public uint Cursor { get; }
        public byte Limit { get; }
        public List(uint snapshotID, uint cursor, byte limit)
        { SnapshotID = snapshotID; Cursor = cursor; Limit = limit; }
    }

    public sealed class Delete : AssetCommand
    {
        public string SHA256 { get; }
        public uint ExpectedRevision { get; }
        public Delete(string sha256, uint expectedRevision)
        { SHA256 = sha256; ExpectedRevision = expectedRevision; }
    }
}

/// <summary>
/// Screen commands for the replacement protocol.
/// </summary>
public abstract class ScreenCommand
{
    private ScreenCommand() { }

    public sealed class Query : ScreenCommand { }

    public sealed class Commit : ScreenCommand
    {
        public ScreenCommit CommitData { get; }
        public Commit(ScreenCommit commitData) { CommitData = commitData; }
    }
}

/// <summary>
/// Encodes replacement-protocol commands (asset/screen) into ordinary JSON frames.
/// </summary>
public static class ReplacementCommandEncoder
{
    public static byte[] Encode(AssetCommand command)
    {
        string body;

        switch (command)
        {
            case AssetCommand.StorageFormat:
                body = "{\"confirmation\":\"verified_erased_spiffs\",\"event\":\"vk_storage_format\"}";
                break;

            case AssetCommand.Begin b:
                ValidateTransfer(b.TransferID);
                ValidateSHA(b.SHA256);
                ValidatePositive(b.TotalBytes, "total_bytes");
                body = $"{{\"event\":\"vk_asset_begin\",\"kind\":\"{b.Kind.WireString()}\",\"sha256\":\"{b.SHA256}\",\"total_bytes\":{b.TotalBytes},\"transfer_id\":{b.TransferID}}}";
                break;

            case AssetCommand.Query q:
                ValidateTransfer(q.TransferID);
                body = $"{{\"event\":\"vk_asset_query\",\"transfer_id\":{q.TransferID}}}";
                break;

            case AssetCommand.End e:
                ValidateTransfer(e.TransferID);
                ValidateSHA(e.SHA256);
                ValidatePositive(e.TotalBytes, "total_bytes");
                body = $"{{\"event\":\"vk_asset_end\",\"kind\":\"{e.Kind.WireString()}\",\"sha256\":\"{e.SHA256}\",\"total_bytes\":{e.TotalBytes},\"transfer_id\":{e.TransferID}}}";
                break;

            case AssetCommand.Abort a:
                ValidateTransfer(a.TransferID);
                body = $"{{\"event\":\"vk_asset_abort\",\"transfer_id\":{a.TransferID}}}";
                break;

            case AssetCommand.List l:
                if (l.Limit < 1 || l.Limit > 64 || (l.SnapshotID == 0 && l.Cursor != 0))
                    throw new ProtocolException("Invalid value: asset_list");
                body = $"{{\"cursor\":{l.Cursor},\"event\":\"vk_asset_list\",\"limit\":{l.Limit},\"snapshot_id\":{l.SnapshotID}}}";
                break;

            case AssetCommand.Delete d:
                ValidateSHA(d.SHA256);
                body = $"{{\"event\":\"vk_asset_delete\",\"expected_revision\":{d.ExpectedRevision},\"sha256\":\"{d.SHA256}\"}}";
                break;

            default:
                throw new ProtocolException($"Unknown asset command: {command.GetType().Name}");
        }

        return FrameEncoder.EncodeOrdinary(FrameType.State, body);
    }

    public static byte[] Encode(ScreenCommand command)
    {
        string body = command switch
        {
            ScreenCommand.Query => "{\"event\":\"vk_screen_query\"}",
            ScreenCommand.Commit c => c.CommitData.CanonicalBody(),
            _ => throw new ProtocolException($"Unknown screen command: {command.GetType().Name}")
        };
        return FrameEncoder.EncodeOrdinary(FrameType.State, body);
    }

    private static void ValidateTransfer(uint id)
    {
        if (id == 0) throw new ProtocolException("Invalid value: transfer_id");
    }

    private static void ValidateSHA(string hash)
    {
        if (!IsValidSHA256(hash))
            throw new ProtocolException("Invalid value: sha256");
    }

    private static void ValidatePositive(uint value, string field)
    {
        if (value == 0) throw new ProtocolException($"Invalid value: {field}");
    }

    public static bool IsValidSHA256(string hash)
    {
        if (hash.Length != 64) return false;
        foreach (char c in hash)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    }
}

/// <summary>
/// Encodes binary asset chunk frames (type 0x40).
/// </summary>
public static class AssetChunkEncoder
{
    public const byte FrameTypeByte = 0x40;
    public const int MaximumPayloadLength = 4084;

    public static byte[] Encode(uint transferID, uint nextOffset, byte[] payload)
    {
        if (transferID == 0) throw new ProtocolException("Invalid value: transfer_id");
        if (payload.Length < 1 || payload.Length > MaximumPayloadLength)
            throw new ProtocolException("Invalid value: payload");

        ushort bodyLength = (ushort)(8 + payload.Length);
        var frame = new byte[4 + bodyLength];
        frame[0] = FrameStreamParser.ProtocolVersion;
        frame[1] = FrameTypeByte;
        frame[2] = (byte)(bodyLength & 0xFF);
        frame[3] = (byte)((bodyLength >> 8) & 0xFF);

        // transfer_id (UInt32 LE)
        frame[4] = (byte)(transferID & 0xFF);
        frame[5] = (byte)((transferID >> 8) & 0xFF);
        frame[6] = (byte)((transferID >> 16) & 0xFF);
        frame[7] = (byte)((transferID >> 24) & 0xFF);

        // next_offset (UInt32 LE)
        frame[8] = (byte)(nextOffset & 0xFF);
        frame[9] = (byte)((nextOffset >> 8) & 0xFF);
        frame[10] = (byte)((nextOffset >> 16) & 0xFF);
        frame[11] = (byte)((nextOffset >> 24) & 0xFF);

        Array.Copy(payload, 0, frame, 12, payload.Length);
        return frame;
    }
}
