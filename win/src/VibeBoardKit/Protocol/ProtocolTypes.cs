using System;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Frame type identifiers used in the binary USB protocol envelope.
/// </summary>
public enum FrameType : byte
{
    Audio = 0x01,
    State = 0x10,
    OtaBegin = 0x20,
    OtaData = 0x21,
    OtaFinish = 0x22,
    OtaCancel = 0x23,
    OtaState = 0x30,
}

/// <summary>
/// A raw decoded frame with its type and complete byte payload.
/// </summary>
public readonly struct RawFrame : IEquatable<RawFrame>
{
    public FrameType Type { get; }
    public byte[] Bytes { get; }

    public RawFrame(FrameType type, byte[] bytes)
    {
        Type = type;
        Bytes = bytes;
    }

    public bool Equals(RawFrame other) => Type == other.Type && Bytes.AsSpan().SequenceEqual(other.Bytes);
    public override bool Equals(object? obj) => obj is RawFrame other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Type, Bytes.Length);
}

/// <summary>
/// Decoded Opus audio frame from the device.
/// </summary>
public readonly struct AudioFrame : IEquatable<AudioFrame>
{
    public uint Session { get; }
    public uint Sequence { get; }
    public byte Flags { get; }
    public byte[] Payload { get; }

    public AudioFrame(uint session, uint sequence, byte flags, byte[] payload)
    {
        Session = session;
        Sequence = sequence;
        Flags = flags;
        Payload = payload;
    }

    public bool Equals(AudioFrame other) =>
        Session == other.Session && Sequence == other.Sequence &&
        Flags == other.Flags && Payload.AsSpan().SequenceEqual(other.Payload);
    public override bool Equals(object? obj) => obj is AudioFrame other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Session, Sequence, Flags);
}

/// <summary>
/// Display capability from the replacement protocol.
/// </summary>
public sealed class CapabilityDisplay : IEquatable<CapabilityDisplay>
{
    public ushort Width { get; }
    public ushort Height { get; }
    public string Format { get; }

    public CapabilityDisplay(ushort width, ushort height, string format)
    {
        Width = width;
        Height = height;
        Format = format;
    }

    public bool Equals(CapabilityDisplay? other) =>
        other is not null && Width == other.Width && Height == other.Height && Format == other.Format;
    public override bool Equals(object? obj) => obj is CapabilityDisplay other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Width, Height, Format);
}

/// <summary>
/// A single feature availability block from device capabilities.
/// </summary>
public sealed class CapabilityFeature
{
    public ushort Version { get; }
    public bool Available { get; }
    public string? Reason { get; }

    public CapabilityFeature(ushort version, bool available, string? reason)
    {
        Version = version;
        Available = available;
        Reason = reason;
    }
}

/// <summary>
/// The features block from a device_info event.
/// </summary>
public sealed class CapabilityFeatures
{
    public CapabilityFeature? Assets { get; }
    public CapabilityFeature? Screen { get; }
    public CapabilityFeature? Update { get; }

    public CapabilityFeatures(CapabilityFeature? assets, CapabilityFeature? screen, CapabilityFeature? update)
    {
        Assets = assets;
        Screen = screen;
        Update = update;
    }
}

/// <summary>
/// A decoded JSON state event from the device.
/// Fields are nullable because different event types carry different subsets.
/// </summary>
public sealed class StateEvent : IEquatable<StateEvent>
{
    public string Event { get; }
    public string? Button { get; }
    public uint? SessionID { get; }
    public uint? DurationMS { get; }
    public string? Hardware { get; }
    public string? FirmwareVersion { get; }
    public string[]? Buttons { get; }
    public string[]? UiStates { get; }
    public string[]? InteractionModes { get; }
    public string? Message { get; }
    public string? Operation { get; }
    public string? Code { get; }
    public string? DeviceID { get; }
    public bool? Provisioned { get; }
    public ushort? ReplacementProtocol { get; }
    public ushort? ProtocolVersion { get; }
    public CapabilityDisplay? Display { get; }
    public CapabilityFeatures? Features { get; }

    public StateEvent(
        string @event,
        string? button = null,
        uint? sessionID = null,
        uint? durationMS = null,
        string? hardware = null,
        string? firmwareVersion = null,
        string[]? buttons = null,
        string[]? uiStates = null,
        string[]? interactionModes = null,
        string? message = null,
        string? operation = null,
        string? code = null,
        string? deviceID = null,
        bool? provisioned = null,
        ushort? replacementProtocol = null,
        ushort? protocolVersion = null,
        CapabilityDisplay? display = null,
        CapabilityFeatures? features = null)
    {
        Event = @event;
        Button = button;
        SessionID = sessionID;
        DurationMS = durationMS;
        Hardware = hardware;
        FirmwareVersion = firmwareVersion;
        Buttons = buttons;
        UiStates = uiStates;
        InteractionModes = interactionModes;
        Message = message;
        Operation = operation;
        Code = code;
        DeviceID = deviceID;
        Provisioned = provisioned;
        ReplacementProtocol = replacementProtocol;
        ProtocolVersion = protocolVersion;
        Display = display;
        Features = features;
    }

    public bool Equals(StateEvent? other)
    {
        if (other is null) return false;
        return Event == other.Event && Button == other.Button &&
               SessionID == other.SessionID && DurationMS == other.DurationMS &&
               Hardware == other.Hardware && FirmwareVersion == other.FirmwareVersion &&
               Message == other.Message && Operation == other.Operation &&
               Code == other.Code && DeviceID == other.DeviceID &&
               Provisioned == other.Provisioned &&
               ReplacementProtocol == other.ReplacementProtocol &&
               ProtocolVersion == other.ProtocolVersion;
    }
    public override bool Equals(object? obj) => obj is StateEvent other && Equals(other);
    public override int GetHashCode() => Event.GetHashCode();
}

/// <summary>
/// A successfully parsed frame: either audio or state.
/// </summary>
public abstract class ParsedFrame
{
    private ParsedFrame() { }

    public sealed class Audio : ParsedFrame
    {
        public AudioFrame Frame { get; }
        public Audio(AudioFrame frame) => Frame = frame;
    }

    public sealed class State : ParsedFrame
    {
        public StateEvent Event { get; }
        public State(StateEvent @event) => Event = @event;
    }
}

/// <summary>
/// Protocol-level errors for framing and model violations.
/// </summary>
public class ProtocolException : Exception
{
    public ProtocolException(string message) : base(message) { }
}

public enum FrameDiscardReason
{
    InvalidVersion,
    UnknownType,
    FrameTooLarge
}

/// <summary>
/// An event from the stream parser: either a completed frame or a discarded byte.
/// </summary>
public readonly struct FrameStreamEvent
{
    public RawFrame? Frame { get; }
    public byte? DiscardedByte { get; }
    public FrameDiscardReason? Reason { get; }

    private FrameStreamEvent(RawFrame? frame, byte? discardedByte, FrameDiscardReason? reason)
    {
        Frame = frame;
        DiscardedByte = discardedByte;
        Reason = reason;
    }

    public static FrameStreamEvent FrameEvent(RawFrame frame) => new(frame, null, null);
    public static FrameStreamEvent Discard(byte b, FrameDiscardReason reason) => new(null, b, reason);
}
