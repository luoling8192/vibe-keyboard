using System;
using VibeBoardKit.Protocol;

namespace VibeBoardKit.Audio;

/// <summary>
/// Discriminator for <see cref="AudioRecordingState"/>, mirroring the cases of
/// the Swift <c>AudioRecordingState</c> enum.
/// </summary>
public enum AudioRecordingStateKind
{
    Ready,
    Recording,
    Finalizing,
    Completed,
    Cancelled,
    Failed,
}

/// <summary>
/// Error categories raised by <see cref="AudioRecordingSession"/>, mirroring the
/// Swift <c>AudioRecordingError</c> cases. Each factory captures the associated
/// values used in value equality.
/// </summary>
public enum AudioRecordingErrorKind
{
    AlreadyCompleted,
    Cancelled,
    MissingFirstFlag,
    InvalidFirstSequence,
    UnsupportedFlags,
    EmptyAudioPacket,
    AudioPacketTooLarge,
    NoAudio,
    SessionChanged,
    SequenceGap,
    DuplicateSequence,
    SequenceRegression,
    SequenceOverflow,
    Output,
}

/// <summary>
/// Immutable, value-equal description of a recording error. Ports the associated
/// values of Swift's <c>AudioRecordingError</c>.
/// </summary>
public sealed class AudioRecordingError : IEquatable<AudioRecordingError?>
{
    public AudioRecordingErrorKind Kind { get; }
    public byte Flags { get; }
    public uint Sequence { get; }
    public uint Session { get; }
    public uint Expected { get; }
    public uint Actual { get; }
    public int ActualSize { get; }
    public int Maximum { get; }
    public string? Detail { get; }

    private AudioRecordingError(
        AudioRecordingErrorKind kind,
        byte flags,
        uint sequence,
        uint session,
        uint expected,
        uint actual,
        int actualSize,
        int maximum,
        string? detail)
    {
        Kind = kind;
        Flags = flags;
        Sequence = sequence;
        Session = session;
        Expected = expected;
        Actual = actual;
        ActualSize = actualSize;
        Maximum = maximum;
        Detail = detail;
    }

    public static AudioRecordingError AlreadyCompleted => new(AudioRecordingErrorKind.AlreadyCompleted, 0, 0, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError Cancelled => new(AudioRecordingErrorKind.Cancelled, 0, 0, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError SequenceOverflow => new(AudioRecordingErrorKind.SequenceOverflow, 0, 0, 0, 0, 0, 0, 0, null);

    public static AudioRecordingError MissingFirstFlag(byte flags) => new(AudioRecordingErrorKind.MissingFirstFlag, flags, 0, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError InvalidFirstSequence(uint sequence) => new(AudioRecordingErrorKind.InvalidFirstSequence, 0, sequence, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError UnsupportedFlags(byte flags) => new(AudioRecordingErrorKind.UnsupportedFlags, flags, 0, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError EmptyAudioPacket(uint sequence) => new(AudioRecordingErrorKind.EmptyAudioPacket, 0, sequence, 0, 0, 0, 0, 0, null);
    public static AudioRecordingError AudioPacketTooLarge(uint sequence, int actual, int maximum) => new(AudioRecordingErrorKind.AudioPacketTooLarge, 0, sequence, 0, 0, 0, actual, maximum, null);
    public static AudioRecordingError NoAudio(uint session) => new(AudioRecordingErrorKind.NoAudio, 0, 0, session, 0, 0, 0, 0, null);
    public static AudioRecordingError SessionChanged(uint expected, uint actual) => new(AudioRecordingErrorKind.SessionChanged, 0, 0, 0, expected, actual, 0, 0, null);
    public static AudioRecordingError SequenceGap(uint expected, uint actual) => new(AudioRecordingErrorKind.SequenceGap, 0, 0, 0, expected, actual, 0, 0, null);
    public static AudioRecordingError DuplicateSequence(uint actual) => new(AudioRecordingErrorKind.DuplicateSequence, 0, 0, 0, 0, actual, 0, 0, null);
    public static AudioRecordingError SequenceRegression(uint expected, uint actual) => new(AudioRecordingErrorKind.SequenceRegression, 0, 0, 0, expected, actual, 0, 0, null);
    public static AudioRecordingError Output(string detail) => new(AudioRecordingErrorKind.Output, 0, 0, 0, 0, 0, 0, 0, detail);

    public bool Equals(AudioRecordingError? other)
    {
        if (other is null)
            return false;
        return Kind == other.Kind
            && Flags == other.Flags
            && Sequence == other.Sequence
            && Session == other.Session
            && Expected == other.Expected
            && Actual == other.Actual
            && ActualSize == other.ActualSize
            && Maximum == other.Maximum
            && Detail == other.Detail;
    }

    public override bool Equals(object? obj) => Equals(obj as AudioRecordingError);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        hash.Add(Kind);
        hash.Add(Flags);
        hash.Add(Sequence);
        hash.Add(Session);
        hash.Add(Expected);
        hash.Add(Actual);
        hash.Add(ActualSize);
        hash.Add(Maximum);
        hash.Add(Detail);
        return hash.ToHashCode();
    }

    public override string ToString()
    {
        return Kind switch
        {
            AudioRecordingErrorKind.MissingFirstFlag => $"{Kind}(flags={Flags})",
            AudioRecordingErrorKind.InvalidFirstSequence => $"{Kind}(sequence={Sequence})",
            AudioRecordingErrorKind.UnsupportedFlags => $"{Kind}(flags={Flags})",
            AudioRecordingErrorKind.EmptyAudioPacket => $"{Kind}(sequence={Sequence})",
            AudioRecordingErrorKind.AudioPacketTooLarge => $"{Kind}(sequence={Sequence}, actual={ActualSize}, maximum={Maximum})",
            AudioRecordingErrorKind.NoAudio => $"{Kind}(session={Session})",
            AudioRecordingErrorKind.SessionChanged => $"{Kind}(expected={Expected}, actual={Actual})",
            AudioRecordingErrorKind.SequenceGap => $"{Kind}(expected={Expected}, actual={Actual})",
            AudioRecordingErrorKind.DuplicateSequence => $"{Kind}(actual={Actual})",
            AudioRecordingErrorKind.SequenceRegression => $"{Kind}(expected={Expected}, actual={Actual})",
            AudioRecordingErrorKind.Output => $"{Kind}({Detail})",
            _ => Kind.ToString(),
        };
    }
}

/// <summary>
/// Raised by <see cref="AudioRecordingSession.Consume"/>. Carries the typed
/// <see cref="Error"/> so callers (and tests) can inspect the failure category.
/// </summary>
public sealed class AudioRecordingException : Exception
{
    public AudioRecordingError Error { get; }

    public AudioRecordingException(AudioRecordingError error)
        : base(error.ToString())
    {
        Error = error;
    }
}

/// <summary>
/// Immutable recording state. Ports the Swift <c>AudioRecordingState</c> enum as a
/// discriminated struct: <see cref="Kind"/> plus the relevant payload fields.
/// </summary>
public readonly struct AudioRecordingState : IEquatable<AudioRecordingState>
{
    public AudioRecordingStateKind Kind { get; }
    public uint Session { get; }
    public uint NextSequence { get; }
    public uint PacketCount { get; }
    public AudioRecordingError? Error { get; }

    private AudioRecordingState(
        AudioRecordingStateKind kind,
        uint session,
        uint nextSequence,
        uint packetCount,
        AudioRecordingError? error)
    {
        Kind = kind;
        Session = session;
        NextSequence = nextSequence;
        PacketCount = packetCount;
        Error = error;
    }

    public static AudioRecordingState Ready => new(AudioRecordingStateKind.Ready, 0, 0, 0, null);
    public static AudioRecordingState Cancelled => new(AudioRecordingStateKind.Cancelled, 0, 0, 0, null);

    public static AudioRecordingState Recording(uint session, uint nextSequence, uint packetCount)
        => new(AudioRecordingStateKind.Recording, session, nextSequence, packetCount, null);

    public static AudioRecordingState Finalizing(uint session)
        => new(AudioRecordingStateKind.Finalizing, session, 0, 0, null);

    public static AudioRecordingState Completed(uint session, uint packetCount)
        => new(AudioRecordingStateKind.Completed, session, 0, packetCount, null);

    public static AudioRecordingState Failed(AudioRecordingError error)
        => new(AudioRecordingStateKind.Failed, 0, 0, 0, error);

    public bool Equals(AudioRecordingState other) =>
        Kind == other.Kind
        && Session == other.Session
        && NextSequence == other.NextSequence
        && PacketCount == other.PacketCount
        && Equals(Error, other.Error);

    public override bool Equals(object? obj) => obj is AudioRecordingState other && Equals(other);

    public override int GetHashCode() => HashCode.Combine(Kind, Session, NextSequence, PacketCount, Error);

    public static bool operator ==(AudioRecordingState left, AudioRecordingState right) => left.Equals(right);
    public static bool operator !=(AudioRecordingState left, AudioRecordingState right) => !left.Equals(right);
}

/// <summary>
/// Drives the Opus-to-Ogg recording state machine. Ports the Swift
/// <c>AudioRecordingSession</c>: validates frame flags, sequence and payload,
/// feeds Opus packets to an <see cref="OggOpusMuxer"/>, and commits on the
/// end-of-stream frame. Frame flag semantics: <c>0x01</c> = first frame
/// (begin), <c>0x02</c> = last frame (end); <c>0x03</c> is illegal.
/// </summary>
public sealed class AudioRecordingSession
{
    /// <summary>Largest accepted Opus packet payload, in bytes.</summary>
    public const int MaximumOpusPacketSize = 220;

    public AudioRecordingState State { get; private set; } = AudioRecordingState.Ready;

    private readonly OggOpusMuxer _muxer;
    private uint? _session;
    private uint _nextSequence;
    private uint _packetCount;

    public AudioRecordingSession(IOggPageSink sink)
    {
        _muxer = new OggOpusMuxer(sink ?? throw new ArgumentNullException(nameof(sink)));
    }

    /// <summary>Validates and absorbs one <see cref="AudioFrame"/> into the stream.</summary>
    public void Consume(AudioFrame frame)
    {
        switch (State.Kind)
        {
            case AudioRecordingStateKind.Completed:
                throw Except(AudioRecordingError.AlreadyCompleted);
            case AudioRecordingStateKind.Cancelled:
                throw Except(AudioRecordingError.Cancelled);
            case AudioRecordingStateKind.Failed:
                throw Except(State.Error ?? AudioRecordingError.Output("unknown"));
            case AudioRecordingStateKind.Finalizing:
                throw Except(AudioRecordingError.AlreadyCompleted);
            case AudioRecordingStateKind.Ready:
            case AudioRecordingStateKind.Recording:
                break;
            default:
                throw Except(AudioRecordingError.Output($"Unexpected state: {State.Kind}"));
        }

        try
        {
            ValidateKnownFlags(frame.Flags);
            ValidatePayload(frame);
            if (_session is null)
                Begin(frame);
            else
                ConsumeInSession(frame);
        }
        catch (AudioRecordingException ex)
        {
            State = AudioRecordingState.Failed(ex.Error);
            _muxer.Cancel();
            throw;
        }
        catch (Exception ex)
        {
            var wrapped = AudioRecordingError.Output(ex.ToString());
            State = AudioRecordingState.Failed(wrapped);
            _muxer.Cancel();
            throw Except(wrapped);
        }
    }

    /// <summary>Aborts the recording. No-op when already completed or cancelled.</summary>
    public void Cancel()
    {
        switch (State.Kind)
        {
            case AudioRecordingStateKind.Completed:
            case AudioRecordingStateKind.Cancelled:
                return;
            default:
                _muxer.Cancel();
                State = AudioRecordingState.Cancelled;
                break;
        }
    }

    private static AudioRecordingException Except(AudioRecordingError error) => new(error);

    private static void ValidateKnownFlags(byte flags)
    {
        // Only bits 0x01 and 0x02 are meaningful, and they must not both be set.
        if ((flags & ~((byte)0x03)) != 0 || flags == 0x03)
            throw Except(AudioRecordingError.UnsupportedFlags(flags));
    }

    private static void ValidatePayload(AudioFrame frame)
    {
        if (frame.Payload.Length > MaximumOpusPacketSize)
            throw Except(AudioRecordingError.AudioPacketTooLarge(frame.Sequence, frame.Payload.Length, MaximumOpusPacketSize));
    }

    private void Begin(AudioFrame frame)
    {
        if ((frame.Flags & 0x01) == 0)
        {
            if ((frame.Flags & 0x02) != 0 && frame.Payload.Length == 0)
                throw Except(AudioRecordingError.NoAudio(frame.Session));
            throw Except(AudioRecordingError.MissingFirstFlag(frame.Flags));
        }

        if (frame.Sequence != 0)
            throw Except(AudioRecordingError.InvalidFirstSequence(frame.Sequence));

        if (frame.Payload.Length == 0)
            throw Except(AudioRecordingError.EmptyAudioPacket(frame.Sequence));

        _session = frame.Session;
        _muxer.Append(frame.Payload);
        _packetCount = 1;
        _nextSequence = 1;
        State = AudioRecordingState.Recording(frame.Session, _nextSequence, _packetCount);
    }

    private void ConsumeInSession(AudioFrame frame)
    {
        uint session = _session!.Value;

        if (frame.Session != session)
            throw Except(AudioRecordingError.SessionChanged(session, frame.Session));

        ValidateSequence(frame.Sequence);

        // End-of-stream frame: finalise and commit.
        if ((frame.Flags & 0x02) != 0)
        {
            State = AudioRecordingState.Finalizing(session);
            if (frame.Payload.Length == 0)
            {
                _muxer.Finish();
            }
            else
            {
                _muxer.Append(frame.Payload, isLast: true);
                IncrementPacketCount();
            }
            _muxer.Commit();
            State = AudioRecordingState.Completed(session, _packetCount);
            return;
        }

        // Ordinary frame: cannot carry the begin flag again, and must be non-empty.
        if ((frame.Flags & 0x01) != 0)
            throw Except(AudioRecordingError.UnsupportedFlags(frame.Flags));

        if (frame.Payload.Length == 0)
            throw Except(AudioRecordingError.EmptyAudioPacket(frame.Sequence));

        _muxer.Append(frame.Payload);
        IncrementPacketCount();
        IncrementSequence();
        State = AudioRecordingState.Recording(session, _nextSequence, _packetCount);
    }

    private void ValidateSequence(uint actual)
    {
        if (actual == _nextSequence)
            return;
        if (_nextSequence > 0 && actual == _nextSequence - 1)
            throw Except(AudioRecordingError.DuplicateSequence(actual));
        if (actual < _nextSequence)
            throw Except(AudioRecordingError.SequenceRegression(_nextSequence, actual));
        throw Except(AudioRecordingError.SequenceGap(_nextSequence, actual));
    }

    private void IncrementSequence()
    {
        uint next;
        try
        {
            next = checked(_nextSequence + 1);
        }
        catch (OverflowException)
        {
            throw Except(AudioRecordingError.SequenceOverflow);
        }
        _nextSequence = next;
    }

    private void IncrementPacketCount()
    {
        uint next;
        try
        {
            next = checked(_packetCount + 1);
        }
        catch (OverflowException)
        {
            throw Except(AudioRecordingError.SequenceOverflow);
        }
        _packetCount = next;
    }
}
