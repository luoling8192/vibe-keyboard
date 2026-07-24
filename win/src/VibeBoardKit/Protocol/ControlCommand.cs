using System;
using System.Text;
using System.Text.Json;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Voice key selection for the device microphone capture.
/// </summary>
public enum VoiceKey
{
    None,
    K1,
    K2,
    K3,
    K4
}

/// <summary>
/// Interaction mode for voice input on the device.
/// </summary>
public enum InteractionMode
{
    HoldToTalk,
    ClickToTalk
}

/// <summary>
/// UI state the host pushes to the device.
/// </summary>
public enum DeviceUIState
{
    Ready,
    Thinking,
    Listening,
    Processing,
    Error
}

/// <summary>
/// Commands the host can send to the device over the ordinary JSON state channel.
/// </summary>
public abstract class ControlCommand
{
    private ControlCommand() { }

    public sealed class AnnounceUSBTransport : ControlCommand { }
    public sealed class GetDeviceInfo : ControlCommand { }
    public sealed class UIState : ControlCommand
    {
        public DeviceUIState State { get; }
        public string Text { get; }
        public UIState(DeviceUIState state, string text) { State = state; Text = text; }
    }
    public sealed class Ping : ControlCommand { }
    public sealed class InteractionModeCmd : ControlCommand
    {
        public InteractionMode Mode { get; }
        public InteractionModeCmd(InteractionMode mode) { Mode = mode; }
    }
    public sealed class VoiceKeyCmd : ControlCommand
    {
        public VoiceKey Key { get; }
        public VoiceKeyCmd(VoiceKey key) { Key = key; }
    }
}

/// <summary>
/// Encodes <see cref="ControlCommand"/> instances into binary frames for USB transmission.
/// </summary>
public static class FrameEncoder
{
    public static byte[] Encode(ControlCommand command)
    {
        string body = command switch
        {
            ControlCommand.AnnounceUSBTransport => "{\"event\":\"transport\",\"kind\":\"usb\"}",
            ControlCommand.GetDeviceInfo => "{\"event\":\"get_device_info\"}",
            ControlCommand.UIState ui => $"{{\"event\":\"ui_state\",\"state\":\"{UIStateString(ui.State)}\",\"text\":\"{JsonEscape(ui.Text)}\"}}",
            ControlCommand.Ping => "{\"event\":\"ping\"}",
            ControlCommand.InteractionModeCmd im => $"{{\"event\":\"interaction_mode\",\"mode\":\"{InteractionModeString(im.Mode)}\"}}",
            ControlCommand.VoiceKeyCmd vk => $"{{\"event\":\"voice_key\",\"key\":\"{VoiceKeyString(vk.Key)}\"}}",
            _ => throw new ProtocolException($"Unknown command type: {command.GetType().Name}")
        };

        return EncodeOrdinary(FrameType.State, body);
    }

    public static byte[] EncodeOrdinary(FrameType type, string body)
    {
        if (type != FrameType.State)
            throw new ProtocolException($"Unsupported frame type for ordinary encoding: {type}");

        byte[] bodyBytes = Encoding.UTF8.GetBytes(body);
        if (bodyBytes.Length > ushort.MaxValue)
            throw new ProtocolException($"Body length out of range: {bodyBytes.Length}");

        int totalLength = 4 + bodyBytes.Length;
        if (totalLength > FrameStreamParser.MaximumFrameLength)
            throw new ProtocolException($"Frame too large: actual={totalLength}, max={FrameStreamParser.MaximumFrameLength}");

        var frame = new byte[totalLength];
        frame[0] = FrameStreamParser.ProtocolVersion;
        frame[1] = (byte)type;
        ushort length = (ushort)bodyBytes.Length;
        frame[2] = (byte)(length & 0xFF);
        frame[3] = (byte)((length >> 8) & 0xFF);
        Array.Copy(bodyBytes, 0, frame, 4, bodyBytes.Length);
        return frame;
    }

    public static string JsonEscape(string value)
    {
        var sb = new StringBuilder(value.Length);
        foreach (char c in value)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20)
                        sb.Append($"\\u{(int)c:X4}");
                    else
                        sb.Append(c);
                    break;
            }
        }
        return sb.ToString();
    }

    public static string UIStateString(DeviceUIState state) => state switch
    {
        DeviceUIState.Ready => "ready",
        DeviceUIState.Thinking => "thinking",
        DeviceUIState.Listening => "listening",
        DeviceUIState.Processing => "processing",
        DeviceUIState.Error => "error",
        _ => throw new ProtocolException($"Unknown UI state: {state}")
    };

    public static string InteractionModeString(InteractionMode mode) => mode switch
    {
        InteractionMode.HoldToTalk => "hold_to_talk",
        InteractionMode.ClickToTalk => "click_to_talk",
        _ => throw new ProtocolException($"Unknown interaction mode: {mode}")
    };

    public static string VoiceKeyString(VoiceKey key) => key switch
    {
        VoiceKey.None => "none",
        VoiceKey.K1 => "k1",
        VoiceKey.K2 => "k2",
        VoiceKey.K3 => "k3",
        VoiceKey.K4 => "k4",
        _ => throw new ProtocolException($"Unknown voice key: {key}")
    };
}
