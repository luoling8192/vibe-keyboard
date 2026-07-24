using Xunit;
using VibeBoardKit.Protocol;
using System.Text;

namespace VibeBoardKit.Tests;

public class FrameEncoderTests
{
    [Fact]
    public void EncodesPingCommand()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.Ping());

        Assert.Equal(0x01, frame[0]);                    // protocol version
        Assert.Equal((byte)FrameType.State, frame[1]);  // frame type
        // body length
        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        Assert.Equal(16, bodyLen); // {"event":"ping"}

        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"ping\"}", body);
    }

    [Fact]
    public void EncodesAnnounceUSBTransport()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.AnnounceUSBTransport());

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"transport\",\"kind\":\"usb\"}", body);
    }

    [Fact]
    public void EncodesGetDeviceInfo()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.GetDeviceInfo());

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"get_device_info\"}", body);
    }

    [Fact]
    public void EncodesUIState()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.UIState(DeviceUIState.Ready, ""));

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"ui_state\",\"state\":\"ready\",\"text\":\"\"}", body);
    }

    [Fact]
    public void EscapesJsonInUIStateText()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.UIState(DeviceUIState.Listening, "He said \"hi\""));

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Contains("\\\"", body);
        Assert.Contains("listening", body);
    }

    [Fact]
    public void EncodesInteractionMode()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.InteractionModeCmd(InteractionMode.HoldToTalk));

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"interaction_mode\",\"mode\":\"hold_to_talk\"}", body);
    }

    [Fact]
    public void EncodesVoiceKey()
    {
        var frame = FrameEncoder.Encode(new ControlCommand.VoiceKeyCmd(VoiceKey.K2));

        ushort bodyLen = (ushort)(frame[2] | (frame[3] << 8));
        var body = Encoding.UTF8.GetString(frame, 4, bodyLen);
        Assert.Equal("{\"event\":\"voice_key\",\"key\":\"k2\"}", body);
    }
}
