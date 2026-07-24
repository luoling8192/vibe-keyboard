using Xunit;
using VibeBoardKit.Protocol;
using System.Text;

namespace VibeBoardKit.Tests;

public class FrameStreamParserTests
{
    [Fact]
    public void ParsesSingleStateFrame()
    {
        var parser = new FrameStreamParser();
        var body = Encoding.UTF8.GetBytes("{\"event\":\"ping\"}");

        var frame = new byte[4 + body.Length];
        frame[0] = FrameStreamParser.ProtocolVersion;
        frame[1] = (byte)FrameType.State;
        frame[2] = (byte)(body.Length & 0xFF);
        frame[3] = (byte)((body.Length >> 8) & 0xFF);
        Array.Copy(body, 0, frame, 4, body.Length);

        var events = parser.Append(frame);
        Assert.Single(events);
        Assert.NotNull(events[0].Frame);
        Assert.Equal(FrameType.State, events[0].Frame!.Value.Type);
    }

    [Fact]
    public void DiscardsInvalidVersionByte()
    {
        var parser = new FrameStreamParser();
        var events = parser.Append(new byte[] { 0x02, 0x10, 0x00, 0x00 });
        Assert.Single(events);
        Assert.Equal((byte)0x02, events[0].DiscardedByte);
        Assert.Equal(FrameDiscardReason.InvalidVersion, events[0].Reason);
    }

    [Fact]
    public void ParsesAudioFrame()
    {
        var parser = new FrameStreamParser();

        // Audio frame header: version, type, headerLen=16, session, seq, flags, reserved, payloadLen
        var payload = new byte[] { 0x01, 0x02, 0x03 };
        var frame = new byte[16 + payload.Length];
        frame[0] = FrameStreamParser.ProtocolVersion;   // 0x01
        frame[1] = (byte)FrameType.Audio;                // 0x01
        frame[2] = 16; frame[3] = 0;                     // headerLen = 16
        frame[4] = 1; frame[5] = 0; frame[6] = 0; frame[7] = 0;  // session = 1
        frame[8] = 0; frame[9] = 0; frame[10] = 0; frame[11] = 0; // sequence = 0
        frame[12] = 0x01; frame[13] = 0;                 // flags = begin, reserved = 0
        frame[14] = (byte)payload.Length; frame[15] = 0; // payloadLen
        Array.Copy(payload, 0, frame, 16, payload.Length);

        var events = parser.Append(frame);
        Assert.Single(events);
        Assert.NotNull(events[0].Frame);
        Assert.Equal(FrameType.Audio, events[0].Frame!.Value.Type);
    }

    [Fact]
    public void ParsesMultipleFrames()
    {
        var parser = new FrameStreamParser();

        var body1 = Encoding.UTF8.GetBytes("{\"event\":\"ping\"}");
        var body2 = Encoding.UTF8.GetBytes("{\"event\":\"get_device_info\"}");

        var combined = new byte[(4 + body1.Length) + (4 + body2.Length)];
        // Frame 1
        combined[0] = 0x01;
        combined[1] = (byte)FrameType.State;
        combined[2] = (byte)(body1.Length & 0xFF);
        combined[3] = (byte)((body1.Length >> 8) & 0xFF);
        Array.Copy(body1, 0, combined, 4, body1.Length);
        // Frame 2
        int offset = 4 + body1.Length;
        combined[offset] = 0x01;
        combined[offset + 1] = (byte)FrameType.State;
        combined[offset + 2] = (byte)(body2.Length & 0xFF);
        combined[offset + 3] = (byte)((body2.Length >> 8) & 0xFF);
        Array.Copy(body2, 0, combined, offset + 4, body2.Length);

        var events = parser.Append(combined);
        Assert.Equal(2, events.Count);
        Assert.NotNull(events[0].Frame);
        Assert.NotNull(events[1].Frame);
    }

    [Fact]
    public void HandlesPartialFrame()
    {
        var parser = new FrameStreamParser();

        var body = Encoding.UTF8.GetBytes("{\"event\":\"ping\"}");
        var frame = new byte[4 + body.Length];
        frame[0] = 0x01;
        frame[1] = (byte)FrameType.State;
        frame[2] = (byte)(body.Length & 0xFF);
        frame[3] = (byte)((body.Length >> 8) & 0xFF);
        Array.Copy(body, 0, frame, 4, body.Length);

        // Send only first half
        var events1 = parser.Append(frame[..(frame.Length / 2)]);
        Assert.Empty(events1);

        // Send the rest
        var events2 = parser.Append(frame[(frame.Length / 2)..]);
        Assert.Single(events2);
        Assert.NotNull(events2[0].Frame);
    }
}
