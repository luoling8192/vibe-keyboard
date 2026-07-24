using System;
using System.Text;
using System.Text.Json;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Decodes raw frames into typed <see cref="ParsedFrame"/> instances.
/// </summary>
public static class FrameDecoder
{
    public static ParsedFrame Decode(RawFrame frame)
    {
        return frame.Type switch
        {
            FrameType.Audio => new ParsedFrame.Audio(DecodeAudio(frame.Bytes)),
            FrameType.State => new ParsedFrame.State(DecodeState(frame.Bytes)),
            _ => throw new ProtocolException($"Unsupported frame type: {frame.Type}")
        };
    }

    public static byte[] DecodeStateBody(byte[] data)
    {
        if (data.Length < 4)
            throw new ProtocolException($"Truncated frame: required=4, actual={data.Length}");
        if (data[0] != FrameStreamParser.ProtocolVersion)
            throw new ProtocolException($"Unsupported version: {data[0]}");
        if (data[1] != (byte)FrameType.State)
            throw new ProtocolException($"Unsupported frame type: {data[1]}");

        int bodyLength = FrameStreamParser.ReadUInt16LE(data, 2);
        int totalLength = 4 + bodyLength;
        if (totalLength > FrameStreamParser.MaximumFrameLength)
            throw new ProtocolException($"Frame too large: actual={totalLength}, max={FrameStreamParser.MaximumFrameLength}");
        if (data.Length < totalLength)
            throw new ProtocolException($"Truncated frame: required={totalLength}, actual={data.Length}");

        var body = new byte[bodyLength];
        Array.Copy(data, 4, body, 0, bodyLength);
        return body;
    }

    public static StateEvent DecodeState(byte[] data)
    {
        byte[] body = DecodeStateBody(data);
        using var doc = JsonDocument.Parse(body);
        var root = doc.RootElement;

        string? @event = root.TryGetProperty("event", out var eventProp)
            ? eventProp.GetString()
            : null;
        if (@event == null)
            throw new ProtocolException("Missing required field: event");

        // Parse all known fields
        string? button = root.TryGetProperty("button", out var b) ? b.GetString() : null;
        uint? sessionID = root.TryGetProperty("session_id", out var sid) && sid.TryGetUInt32(out var sval) ? sval : null;
        uint? durationMS = root.TryGetProperty("duration_ms", out var dm) && dm.TryGetUInt32(out var dval) ? dval : null;
        string? hardware = root.TryGetProperty("hardware", out var hw) ? hw.GetString() : null;
        string? firmwareVersion = root.TryGetProperty("firmware_version", out var fv) ? fv.GetString() : null;

        string[]? buttons = null;
        if (root.TryGetProperty("buttons", out var btns) && btns.ValueKind == JsonValueKind.Array)
        {
            var list = new System.Collections.Generic.List<string>();
            foreach (var item in btns.EnumerateArray())
                list.Add(item.GetString() ?? "");
            buttons = list.ToArray();
        }

        string[]? uiStates = null;
        if (root.TryGetProperty("ui_states", out var us) && us.ValueKind == JsonValueKind.Array)
        {
            var list = new System.Collections.Generic.List<string>();
            foreach (var item in us.EnumerateArray())
                list.Add(item.GetString() ?? "");
            uiStates = list.ToArray();
        }

        string[]? interactionModes = null;
        if (root.TryGetProperty("interaction_modes", out var im) && im.ValueKind == JsonValueKind.Array)
        {
            var list = new System.Collections.Generic.List<string>();
            foreach (var item in im.EnumerateArray())
                list.Add(item.GetString() ?? "");
            interactionModes = list.ToArray();
        }

        string? message = root.TryGetProperty("message", out var msg) ? msg.GetString() : null;
        string? operation = root.TryGetProperty("operation", out var op) ? op.GetString() : null;
        string? code = root.TryGetProperty("code", out var c) ? c.GetString() : null;
        string? deviceID = root.TryGetProperty("device_id", out var did) ? did.GetString() : null;
        bool? provisioned = root.TryGetProperty("provisioned", out var prov) && prov.GetBoolean() ? true : (root.TryGetProperty("provisioned", out var prov2) ? prov2.GetBoolean() : null);

        ushort? replacementProtocol = null;
        if (root.TryGetProperty("replacement_protocol", out var rp) && rp.TryGetUInt16(out var rpVal))
            replacementProtocol = rpVal;

        ushort? protocolVersion = null;
        if (root.TryGetProperty("protocol", out var pv) && pv.TryGetUInt16(out var pvVal))
            protocolVersion = pvVal;

        CapabilityDisplay? display = null;
        if (root.TryGetProperty("display", out var disp) && disp.ValueKind == JsonValueKind.Object)
        {
            display = new CapabilityDisplay(
                disp.TryGetProperty("width", out var dw) && dw.TryGetUInt16(out var dwv) ? dwv : (ushort)0,
                disp.TryGetProperty("height", out var dh) && dh.TryGetUInt16(out var dhv) ? dhv : (ushort)0,
                disp.TryGetProperty("format", out var df) ? df.GetString() ?? "" : ""
            );
        }

        CapabilityFeatures? features = null;
        if (root.TryGetProperty("features", out var feat) && feat.ValueKind == JsonValueKind.Object)
        {
            features = new CapabilityFeatures(
                ParseFeature(feat, "assets"),
                ParseFeature(feat, "screen"),
                ParseFeature(feat, "update")
            );
        }

        return new StateEvent(
            @event, button, sessionID, durationMS, hardware, firmwareVersion,
            buttons, uiStates, interactionModes, message, operation, code,
            deviceID, provisioned, replacementProtocol, protocolVersion,
            display, features
        );
    }

    private static CapabilityFeature? ParseFeature(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out var el) || el.ValueKind != JsonValueKind.Object)
            return null;
        ushort version = el.TryGetProperty("version", out var v) && v.TryGetUInt16(out var vv) ? vv : (ushort)0;
        bool available = el.TryGetProperty("available", out var a) && a.GetBoolean();
        string? reason = el.TryGetProperty("reason", out var r) ? r.GetString() : null;
        return new CapabilityFeature(version, available, reason);
    }

    public static AudioFrame DecodeAudio(byte[] data)
    {
        if (data.Length < 16)
            throw new ProtocolException($"Truncated frame: required=16, actual={data.Length}");
        if (data[0] != FrameStreamParser.ProtocolVersion)
            throw new ProtocolException($"Unsupported version: {data[0]}");
        if (data[1] != (byte)FrameType.Audio)
            throw new ProtocolException($"Unsupported frame type: {data[1]}");

        ushort headerLength = FrameStreamParser.ReadUInt16LE(data, 2);
        if (headerLength != 16)
            throw new ProtocolException($"Invalid audio header length: {headerLength}");

        int payloadLength = FrameStreamParser.ReadUInt16LE(data, 14);
        int totalLength = 16 + payloadLength;
        if (totalLength > FrameStreamParser.MaximumFrameLength)
            throw new ProtocolException($"Frame too large: actual={totalLength}, max={FrameStreamParser.MaximumFrameLength}");
        if (data.Length < totalLength)
            throw new ProtocolException($"Truncated frame: required={totalLength}, actual={data.Length}");

        uint session = FrameStreamParser.ReadUInt32LE(data, 4);
        uint sequence = FrameStreamParser.ReadUInt32LE(data, 8);
        byte flags = data[12];

        var payload = new byte[payloadLength];
        Array.Copy(data, 16, payload, 0, payloadLength);

        return new AudioFrame(session, sequence, flags, payload);
    }
}


