using Xunit;
using VibeBoardKit.Protocol;

namespace VibeBoardKit.Tests;

public class AssetChunkEncoderTests
{
    [Fact]
    public void EncodesValidChunk()
    {
        uint transferID = 42;
        uint nextOffset = 100;
        var payload = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF };

        var frame = AssetChunkEncoder.Encode(transferID, nextOffset, payload);

        Assert.Equal(0x01, frame[0]);                          // protocol version
        Assert.Equal(AssetChunkEncoder.FrameTypeByte, frame[1]); // type 0x40
        // body_length = 8 + payload.Length = 12
        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        Assert.Equal(12, bodyLen);

        // transfer_id (LE)
        Assert.Equal(42u, (uint)(frame[4] | (frame[5] << 8) | (frame[6] << 16) | (frame[7] << 24)));
        // next_offset (LE)
        Assert.Equal(100u, (uint)(frame[8] | (frame[9] << 8) | (frame[10] << 16) | (frame[11] << 24)));

        // payload
        Assert.Equal(payload[0], frame[12]);
        Assert.Equal(payload[1], frame[13]);
        Assert.Equal(payload[2], frame[14]);
        Assert.Equal(payload[3], frame[15]);
    }

    [Fact]
    public void RejectsZeroTransferID()
    {
        Assert.Throws<ProtocolException>(() =>
            AssetChunkEncoder.Encode(0, 0, new byte[] { 1 }));
    }

    [Fact]
    public void RejectsEmptyPayload()
    {
        Assert.Throws<ProtocolException>(() =>
            AssetChunkEncoder.Encode(1, 0, Array.Empty<byte>()));
    }

    [Fact]
    public void RejectsOversizedPayload()
    {
        Assert.Throws<ProtocolException>(() =>
            AssetChunkEncoder.Encode(1, 0, new byte[AssetChunkEncoder.MaximumPayloadLength + 1]));
    }

    [Fact]
    public void RejectsInvalidSHA256()
    {
        Assert.False(ReplacementCommandEncoder.IsValidSHA256("short"));
        Assert.False(ReplacementCommandEncoder.IsValidSHA256("XYZ"));
        Assert.True(ReplacementCommandEncoder.IsValidSHA256(
            "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"));
    }
}
