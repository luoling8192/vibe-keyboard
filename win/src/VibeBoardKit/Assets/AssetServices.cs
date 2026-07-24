using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using VibeBoardKit.Protocol;
using VibeBoardKit.USB;

namespace VibeBoardKit.Assets;

/// <summary>
/// A prepared asset ready for upload, with its VKA1 container and limits.
/// </summary>
public sealed class PreparedAsset
{
    public byte[] Data { get; }
    public string SHA256 { get; }
    public AssetKind Kind { get; }
    public VKA1.VKA1Limits Limits { get; }
    public uint TotalBytes => (uint)Data.Length;

    public PreparedAsset(byte[] data, VKA1.VKA1Limits limits)
    {
        Data = data;
        Limits = limits;
        // Decode to get SHA256 and kind
        var container = VKA1.VKA1Codec.Decode(data, limits);
        SHA256 = container.SHA256;
        Kind = container.Kind switch
        {
            VKA1.VKA1Kind.Image => AssetKind.Image,
            VKA1.VKA1Kind.Animation => AssetKind.Animation,
            VKA1.VKA1Kind.GlyphBitmap => AssetKind.GlyphBitmap,
            _ => throw new ProtocolException($"Unknown VKA1 kind: {container.Kind}")
        };
    }
}

/// <summary>
/// Progress information for an asset transfer.
/// </summary>
public struct AssetTransferProgress
{
    public uint NextOffset { get; }
    public uint TotalBytes { get; }
    public double Fraction => TotalBytes == 0 ? 0 : (double)NextOffset / TotalBytes;

    public AssetTransferProgress(uint nextOffset, uint totalBytes)
    {
        NextOffset = nextOffset;
        TotalBytes = totalBytes;
    }
}

/// <summary>
/// Service that manages asset uploads to the device.
/// </summary>
public sealed class AssetTransferService
{
    private readonly USBSession _session;

    public AssetTransferService(USBSession session)
    {
        _session = session;
    }

    /// <summary>
    /// Upload an asset to the device.
    /// Flow: begin → wait for ready → send chunks → end → wait for stored
    /// </summary>
    public async Task<AssetTransferProgress> Upload(PreparedAsset asset, uint transferID)
    {
        // Begin transfer
        await _session.SendAssetCommand(new AssetCommand.Begin(
            transferID, asset.SHA256, asset.TotalBytes, asset.Kind));

        // Wait for vk_asset_ready
        var readyTimeout = DateTime.UtcNow.AddSeconds(30);
        ActiveAssetTransfer? authorization = null;

        while (DateTime.UtcNow < readyTimeout)
        {
            authorization = _session.CurrentActiveAssetTransfer;
            if (authorization != null && authorization.TransferID == transferID)
                break;
            await Task.Delay(50);
        }

        if (authorization == null)
            throw new USBSessionException("Asset transfer timed out waiting for ready");

        // Send chunks
        uint offset = authorization.NextOffset;
        ushort chunkBytes = Math.Min(authorization.ChunkBytes, (ushort)AssetChunkEncoder.MaximumPayloadLength);

        while (offset < asset.TotalBytes)
        {
            int remaining = (int)(asset.TotalBytes - offset);
            int chunkSize = Math.Min(chunkBytes, remaining);
            chunkSize = Math.Min(chunkSize, AssetChunkEncoder.MaximumPayloadLength);

            var payload = new byte[chunkSize];
            Array.Copy(asset.Data, (int)offset, payload, 0, chunkSize);

            await _session.SendAssetChunk(payload, authorization);

            // Wait for progress
            var progressTimeout = DateTime.UtcNow.AddSeconds(30);
            while (DateTime.UtcNow < progressTimeout)
            {
                var current = _session.CurrentActiveAssetTransfer;
                if (current != null && current.TransferID == transferID && current.NextOffset > offset)
                {
                    offset = current.NextOffset;
                    authorization = current;
                    break;
                }

                // Check for error outcome
                var outcome = _session.GetAssetTransferOutcome(transferID);
                if (outcome != null)
                    throw new USBSessionException($"Asset transfer failed: {outcome.GetType().Name}");

                await Task.Delay(50);
            }

            if (DateTime.UtcNow >= progressTimeout)
                throw new USBSessionException("Asset transfer timed out waiting for progress");
        }

        // End transfer
        await _session.SendAssetCommand(new AssetCommand.End(
            transferID, asset.SHA256, asset.TotalBytes, asset.Kind));

        // Wait for vk_asset_stored
        var storedTimeout = DateTime.UtcNow.AddSeconds(30);
        while (DateTime.UtcNow < storedTimeout)
        {
            var outcome = _session.GetAssetTransferOutcome(transferID);
            if (outcome != null)
            {
                if (outcome is AssetTransferOutcome.Stored)
                    return new AssetTransferProgress(asset.TotalBytes, asset.TotalBytes);
                throw new USBSessionException($"Asset transfer failed: {outcome.GetType().Name}");
            }
            await Task.Delay(50);
        }

        throw new USBSessionException("Asset transfer timed out waiting for stored");
    }

    /// <summary>
    /// Cancel an in-progress transfer.
    /// </summary>
    public async Task Cancel(uint transferID)
    {
        await _session.SendAssetCommand(new AssetCommand.Abort(transferID));

        var timeout = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < timeout)
        {
            var outcome = _session.GetAssetTransferOutcome(transferID);
            if (outcome is AssetTransferOutcome.Aborted)
                return;
            await Task.Delay(50);
        }
    }
}

/// <summary>
/// Service for screen configuration operations (query/commit).
/// </summary>
public sealed class ScreenConfigurationService
{
    private readonly USBSession _session;

    public ScreenConfigurationService(USBSession session)
    {
        _session = session;
    }

    public async Task Query()
    {
        await _session.SendScreenCommand(new ScreenCommand.Query());
    }

    public async Task Commit(ScreenCommit commit)
    {
        await _session.SendScreenCommand(new ScreenCommand.Commit(commit));
    }
}
