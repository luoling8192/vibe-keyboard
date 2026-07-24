using System;
using System.Collections.Generic;
using System.Text;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Canonical number for widget values: a coefficient with a fixed scale.
/// </summary>
public sealed class ScreenCanonicalNumber
{
    public long Coefficient { get; }
    public byte Scale { get; }

    public ScreenCanonicalNumber(long coefficient, byte scale)
    {
        Coefficient = coefficient;
        Scale = scale;
    }

    /// <summary>
    /// Returns the canonical JSON representation, or null if invalid.
    /// </summary>
    public string? Canonical
    {
        get
        {
            if (Scale > 3) return null;
            return $"{{\"coefficient\":{Coefficient},\"scale\":{Scale}}}";
        }
    }
}

/// <summary>
/// Widget update state types.
/// </summary>
public abstract class WidgetUpdateState
{
    private WidgetUpdateState() { }

    public sealed class FreshText : WidgetUpdateState
    {
        public string Value { get; }
        public FreshText(string value) { Value = value; }
    }

    public sealed class FreshNumber : WidgetUpdateState
    {
        public ScreenCanonicalNumber Value { get; }
        public FreshNumber(ScreenCanonicalNumber value) { Value = value; }
    }

    public sealed class Stale : WidgetUpdateState { }

    public sealed class Error : WidgetUpdateState
    {
        public string? Message { get; }
        public Error(string? message) { Message = message; }
    }
}

/// <summary>
/// Command to update a widget value on the device.
/// </summary>
public sealed class WidgetUpdateCommand
{
    public uint Revision { get; }
    public string WidgetID { get; }
    public uint Sequence { get; }
    public WidgetUpdateState State { get; }

    public WidgetUpdateCommand(uint revision, string widgetID, uint sequence, WidgetUpdateState state)
    {
        Revision = revision;
        WidgetID = widgetID;
        Sequence = sequence;
        State = state;
    }
}

public enum WidgetAppliedState
{
    Fresh,
    Stale,
    Error
}

public sealed class WidgetAppliedEvent
{
    public uint Revision { get; }
    public string WidgetID { get; }
    public uint Sequence { get; }
    public WidgetAppliedState State { get; }

    public WidgetAppliedEvent(uint revision, string widgetID, uint sequence, WidgetAppliedState state)
    {
        Revision = revision;
        WidgetID = widgetID;
        Sequence = sequence;
        State = state;
    }
}

public sealed class WidgetErrorEvent
{
    public string Code { get; }
    public string? WidgetID { get; }
    public uint? Sequence { get; }
    public string? Message { get; }

    public WidgetErrorEvent(string code, string? widgetID, uint? sequence, string? message)
    {
        Code = code;
        WidgetID = widgetID;
        Sequence = sequence;
        Message = message;
    }
}

/// <summary>
/// Widget protocol event: applied or error.
/// </summary>
public abstract class WidgetProtocolEvent
{
    private WidgetProtocolEvent() { }

    public sealed class Applied : WidgetProtocolEvent
    {
        public WidgetAppliedEvent Event { get; }
        public Applied(WidgetAppliedEvent @event) { Event = @event; }
    }

    public sealed class Error : WidgetProtocolEvent
    {
        public WidgetErrorEvent Event { get; }
        public Error(WidgetErrorEvent @event) { Event = @event; }
    }
}

/// <summary>
/// Codec for the widget protocol.
/// </summary>
public static class WidgetProtocolCodec
{
    private static readonly HashSet<string> WidgetErrorCodes = new()
    {
        "not_configured", "wrong_revision", "not_found", "stale_sequence",
        "type_mismatch", "out_of_range", "too_large", "invalid_state", "internal"
    };

    public static byte[] Encode(WidgetUpdateCommand command, ushort maximumValueBytes)
    {
        if (command.Revision == 0 || command.Sequence == 0 || !BoundedJSON.IsValidIdentifier(command.WidgetID))
            throw new ProtocolException("Invalid value: widget_update");

        string suffix;
        switch (command.State)
        {
            case WidgetUpdateState.FreshText ft:
                if (Encoding.UTF8.GetByteCount(ft.Value) > maximumValueBytes)
                    throw new ProtocolException("Limit exceeded: max_widget_value_bytes");
                suffix = $",\"state\":\"fresh\",\"value\":{Quote(ft.Value)}";
                break;

            case WidgetUpdateState.FreshNumber fn:
                string? canonical = fn.Value.Canonical;
                if (canonical == null)
                    throw new ProtocolException("Invalid value: widget.value");
                suffix = $",\"state\":\"fresh\",\"value\":{canonical}";
                break;

            case WidgetUpdateState.Stale:
                suffix = ",\"state\":\"stale\"";
                break;

            case WidgetUpdateState.Error err:
                if (err.Message != null)
                {
                    if (Encoding.UTF8.GetByteCount(err.Message) > 96)
                        throw new ProtocolException("Limit exceeded: message");
                    suffix = $",\"message\":{Quote(err.Message)},\"state\":\"error\"";
                }
                else
                {
                    suffix = ",\"state\":\"error\"";
                }
                break;

            default:
                throw new ProtocolException($"Unknown widget state: {command.State.GetType().Name}");
        }

        string body = $"{{\"event\":\"vk_widget_update\",\"revision\":{command.Revision},\"sequence\":{command.Sequence}{suffix},\"widget_id\":{Quote(command.WidgetID)}}}";
        return FrameEncoder.EncodeOrdinary(FrameType.State, body);
    }

    public static WidgetProtocolEvent? TryDecode(byte[] body)
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

    public static WidgetProtocolEvent Decode(byte[] data)
    {
        var obj = BoundedJSON.Object(data);
        string? eventName = BoundedJSON.String(obj["event"]);
        if (eventName == null)
            throw new ProtocolException("Invalid value: event");

        if (eventName == "vk_widget_applied")
        {
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "event", "revision", "sequence", "state", "widget_id" }, eventName);
            uint? revision = BoundedJSON.PositiveUInt32(obj["revision"]);
            uint? sequence = BoundedJSON.PositiveUInt32(obj["sequence"]);
            string? widgetID = BoundedJSON.String(obj["widget_id"]);
            string? rawState = BoundedJSON.String(obj["state"]);

            if (revision == null || sequence == null || widgetID == null ||
                !BoundedJSON.IsValidIdentifier(widgetID) || rawState == null)
                throw new ProtocolException($"Invalid value: {eventName}");

            WidgetAppliedState state = rawState switch
            {
                "fresh" => WidgetAppliedState.Fresh,
                "stale" => WidgetAppliedState.Stale,
                "error" => WidgetAppliedState.Error,
                _ => throw new ProtocolException($"Invalid value: state")
            };

            return new WidgetProtocolEvent.Applied(
                new WidgetAppliedEvent(revision.Value, widgetID, sequence.Value, state));
        }

        if (eventName == "vk_error")
        {
            string? operation = BoundedJSON.String(obj["operation"]);
            string? code = BoundedJSON.String(obj["code"]);

            if (operation != "widget" || code == null || !WidgetErrorCodes.Contains(code))
                throw new ProtocolException("Invalid value: vk_error.widget");

            var allowed = new HashSet<string> { "code", "event", "message", "operation", "sequence", "widget_id" };
            var required = new HashSet<string> { "code", "event", "operation" };
            var objKeys = new HashSet<string>(obj.Keys);

            if (!required.IsSubsetOf(objKeys) || !objKeys.IsSubsetOf(allowed))
                throw new ProtocolException("Invalid keys: vk_error.widget");

            string? widgetID = BoundedJSON.String(obj["widget_id"]);
            if (obj["widget_id"] != null && (widgetID == null || !BoundedJSON.IsValidIdentifier(widgetID)))
                throw new ProtocolException("Invalid value: widget_id");

            uint? sequence = null;
            if (obj["sequence"] != null)
            {
                uint? val = BoundedJSON.PositiveUInt32(obj["sequence"]);
                if (val == null)
                    throw new ProtocolException("Invalid value: sequence");
                sequence = val;
            }

            string? message = BoundedJSON.String(obj["message"]);
            if (obj["message"] != null && (message == null || Encoding.UTF8.GetByteCount(message) > 96))
                throw new ProtocolException("Invalid value: message");

            return new WidgetProtocolEvent.Error(
                new WidgetErrorEvent(code, widgetID, sequence, message));
        }

        throw new ProtocolException($"Invalid value: event");
    }

    private static string Quote(string value)
    {
        var sb = new StringBuilder(value.Length + 2);
        sb.Append('"');
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
        sb.Append('"');
        return sb.ToString();
    }
}
