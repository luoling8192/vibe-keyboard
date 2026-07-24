using System;
using System.Collections.Generic;
using System.Text;

namespace VibeBoardKit.Protocol;

/// <summary>
/// LED effective state reported by the device.
/// </summary>
public enum LEDEffectiveState
{
    Off,
    Connected,
    Recording,
    Mutation
}

public static class LEDEffectiveStateExtensions
{
    public static string WireString(this LEDEffectiveState state) => state switch
    {
        LEDEffectiveState.Off => "off",
        LEDEffectiveState.Connected => "connected",
        LEDEffectiveState.Recording => "recording",
        LEDEffectiveState.Mutation => "mutation",
        _ => throw new ProtocolException($"Unknown LED effective state: {state}")
    };

    public static LEDEffectiveState FromWire(string s) => s switch
    {
        "off" => LEDEffectiveState.Off,
        "connected" => LEDEffectiveState.Connected,
        "recording" => LEDEffectiveState.Recording,
        "mutation" => LEDEffectiveState.Mutation,
        _ => throw new ProtocolException($"Unknown LED effective state: {s}")
    };
}

public enum LEDStateSource
{
    Query,
    Applied
}

public static class LEDStateSourceExtensions
{
    public static string WireString(this LEDStateSource src) => src switch
    {
        LEDStateSource.Query => "query",
        LEDStateSource.Applied => "applied",
        _ => throw new ProtocolException($"Unknown LED state source: {src}")
    };

    public static LEDStateSource FromWire(string s) => s switch
    {
        "query" => LEDStateSource.Query,
        "applied" => LEDStateSource.Applied,
        _ => throw new ProtocolException($"Unknown LED state source: {s}")
    };
}

/// <summary>
/// LED state event from the device.
/// </summary>
public sealed class LEDStateEvent
{
    public uint RequestID { get; }
    public LEDStateSource Source { get; }
    public bool Available { get; }
    public string? Reason { get; }
    public bool? Enabled { get; }
    public byte? Brightness { get; }
    public LEDEffectiveState? Effective { get; }

    public LEDStateEvent(uint requestID, LEDStateSource source, bool available,
        string? reason, bool? enabled, byte? brightness, LEDEffectiveState? effective)
    {
        RequestID = requestID;
        Source = source;
        Available = available;
        Reason = reason;
        Enabled = enabled;
        Brightness = brightness;
        Effective = effective;
    }
}

public sealed class LEDErrorEvent
{
    public uint? RequestID { get; }
    public string Code { get; }
    public string? Message { get; }

    public LEDErrorEvent(uint? requestID, string code, string? message)
    {
        RequestID = requestID;
        Code = code;
        Message = message;
    }
}

/// <summary>
/// LED protocol event: either a state event or an error.
/// </summary>
public abstract class LEDProtocolEvent
{
    private LEDProtocolEvent() { }

    public sealed class State : LEDProtocolEvent
    {
        public LEDStateEvent Event { get; }
        public State(LEDStateEvent @event) { Event = @event; }
    }

    public sealed class Error : LEDProtocolEvent
    {
        public LEDErrorEvent Event { get; }
        public Error(LEDErrorEvent @event) { Event = @event; }
    }
}

/// <summary>
/// LED commands: query or configure.
/// </summary>
public abstract class LEDCommand
{
    private LEDCommand() { }

    public sealed class Query : LEDCommand
    {
        public uint RequestID { get; }
        public Query(uint requestID) { RequestID = requestID; }
    }

    public sealed class Config : LEDCommand
    {
        public uint RequestID { get; }
        public bool Enabled { get; }
        public byte Brightness { get; }
        public Config(uint requestID, bool enabled, byte brightness)
        { RequestID = requestID; Enabled = enabled; Brightness = brightness; }
    }
}

/// <summary>
/// Codec for LED protocol: encodes commands and decodes events.
/// </summary>
public static class LEDProtocolCodec
{
    private static readonly HashSet<string> LedErrorCodes = new()
    {
        "invalid_request", "wrong_epoch", "unavailable", "busy",
        "queue_overflow", "hardware_failed", "tainted"
    };

    public static byte[] Encode(LEDCommand command)
    {
        string body;
        switch (command)
        {
            case LEDCommand.Query q:
                if (q.RequestID == 0) throw new ProtocolException("Invalid value: request_id");
                body = $"{{\"event\":\"vk_led_query\",\"request_id\":{q.RequestID}}}";
                break;
            case LEDCommand.Config c:
                if (c.RequestID == 0) throw new ProtocolException("Invalid value: request_id");
                body = $"{{\"brightness\":{c.Brightness},\"enabled\":{(c.Enabled ? "true" : "false")},\"event\":\"vk_led_config\",\"request_id\":{c.RequestID}}}";
                break;
            default:
                throw new ProtocolException($"Unknown LED command: {command.GetType().Name}");
        }
        return FrameEncoder.EncodeOrdinary(FrameType.State, body);
    }

    public static LEDProtocolEvent? TryDecode(byte[] body)
    {
        try
        {
            return Decode(body);
        }
        catch (ProtocolException)
        {
            return null;
        }
    }

    public static LEDProtocolEvent Decode(byte[] data)
    {
        var obj = BoundedJSON.Object(data);
        string? eventName = BoundedJSON.String(obj["event"]);
        if (eventName == null)
            throw new ProtocolException("Invalid value: event");

        if (eventName == "vk_led_state")
            return new LEDProtocolEvent.State(DecodeState(obj));
        if (eventName == "vk_error")
            return new LEDProtocolEvent.Error(DecodeError(obj));

        throw new ProtocolException($"Invalid value: event");
    }

    private static LEDStateEvent DecodeState(Dictionary<string, object?> obj)
    {
        bool? available = BoundedJSON.Bool(obj["available"]);
        string? sourceRaw = BoundedJSON.String(obj["source"]);
        uint? requestID = BoundedJSON.UInt32(obj["request_id"]);

        if (available == null || sourceRaw == null || requestID == null || requestID == 0)
            throw new ProtocolException("Invalid value: vk_led_state");

        LEDStateSource source = LEDStateSourceExtensions.FromWire(sourceRaw);

        if (available == false)
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "event", "reason", "request_id", "source" }, "vk_led_state unavailable");
            string? reason = BoundedJSON.String(obj["reason"]);
            if (reason == null || (reason != "calibration_required" && reason != "hardware_failed" && reason != "tainted"))
                throw new ProtocolException("Invalid value: reason");
            return new LEDStateEvent(requestID.Value, source, false, reason, null, null, null);
        }

        BoundedJSON.ExactKeys(obj, new HashSet<string> { "available", "brightness", "effective", "enabled", "event", "request_id", "source" }, "vk_led_state");

        bool? enabled = BoundedJSON.Bool(obj["enabled"]);
        byte? brightness = BoundedJSON.Byte(obj["brightness"]);
        string? effectiveRaw = BoundedJSON.String(obj["effective"]);

        if (enabled == null || brightness == null || effectiveRaw == null)
            throw new ProtocolException("Invalid value: vk_led_state");

        LEDEffectiveState effective = LEDEffectiveStateExtensions.FromWire(effectiveRaw);

        // (enabled && brightness != 0) || effective == Off
        if (!((enabled.Value && brightness.Value != 0) || effective == LEDEffectiveState.Off))
            throw new ProtocolException("Invalid value: vk_led_state");

        return new LEDStateEvent(requestID.Value, source, true, null, enabled, brightness, effective);
    }

    private static LEDErrorEvent DecodeError(Dictionary<string, object?> obj)
    {
        string? operation = BoundedJSON.String(obj["operation"]);
        string? code = BoundedJSON.String(obj["code"]);

        if (operation != "led" || code == null || !LedErrorCodes.Contains(code))
            throw new ProtocolException("Invalid value: vk_error.led");

        var allowed = new HashSet<string> { "code", "event", "message", "operation", "request_id" };
        var required = new HashSet<string> { "code", "event", "operation" };

        var objKeys = new HashSet<string>(obj.Keys);
        if (!required.IsSubsetOf(objKeys) || !objKeys.IsSubsetOf(allowed))
            throw new ProtocolException("Invalid keys: vk_error.led");

        uint? requestID = null;
        if (obj["request_id"] != null)
        {
            uint? val = BoundedJSON.UInt32(obj["request_id"]);
            if (val == null || val == 0)
                throw new ProtocolException("Invalid value: request_id");
            requestID = val;
        }

        string? message = BoundedJSON.String(obj["message"]);
        if (obj["message"] != null)
        {
            if (message == null || Encoding.UTF8.GetByteCount(message) > 96)
                throw new ProtocolException("Invalid value: message");
        }

        return new LEDErrorEvent(requestID, code, message);
    }
}

public class LEDServiceException : Exception
{
    public LEDServiceException(string message) : base(message) { }
}
