using System;
using System.Collections.Generic;

namespace VibeBoardKit.Input;

/// <summary>Reasons for which gesture routing fails.</summary>
public enum GestureErrorReason
{
    InvalidPolicy,
    DuplicateDown,
    UpWithoutDown,
    TimestampRegression
}

/// <summary>
/// Thrown when a <see cref="GestureRouter"/> receives an invalid event sequence or policy.
/// Mirrors the macOS <c>GestureError</c>.
/// </summary>
public sealed class GestureException : Exception
{
    public GestureErrorReason Reason { get; }
    public CanonicalKey? Key { get; }
    public string? Detail { get; }

    public GestureException(GestureErrorReason reason, CanonicalKey? key = null, string? detail = null)
        : base(BuildMessage(reason, key, detail))
    {
        Reason = reason;
        Key = key;
        Detail = detail;
    }

    public static GestureException InvalidPolicy(string field) =>
        new(GestureErrorReason.InvalidPolicy, detail: field);

    public static GestureException DuplicateDown(CanonicalKey key) =>
        new(GestureErrorReason.DuplicateDown, key);

    public static GestureException UpWithoutDown(CanonicalKey key) =>
        new(GestureErrorReason.UpWithoutDown, key);

    public static GestureException TimestampRegression() =>
        new(GestureErrorReason.TimestampRegression);

    private static string BuildMessage(GestureErrorReason reason, CanonicalKey? key, string? detail) =>
        reason switch
        {
            GestureErrorReason.InvalidPolicy => $"Invalid gesture policy: {detail}",
            GestureErrorReason.DuplicateDown => $"Duplicate down for key {key}",
            GestureErrorReason.UpWithoutDown => $"Up without down for key {key}",
            GestureErrorReason.TimestampRegression => "Timestamp regression",
            _ => reason.ToString()
        };
}

/// <summary>
/// Configures how a <see cref="GestureRouter"/> derives double-clicks and long-presses.
/// Time thresholds default to 350 ms (double-click window) and 500 ms (long-press threshold).
/// </summary>
public sealed class GesturePolicy
{
    /// <summary>The maximum interval between two clicks for them to count as a double click.</summary>
    public long DoubleClickWindowMilliseconds { get; }

    /// <summary>The minimum press duration for a press to count as a long press.</summary>
    public long LongPressThresholdMilliseconds { get; }

    /// <summary>Whether the router derives double-clicks from firmware click events.</summary>
    public bool DerivesDoubleClick { get; }

    /// <summary>Whether the router derives long-presses from down/up timing.</summary>
    public bool DerivesLongPress { get; }

    public GesturePolicy(
        long doubleClickWindowMilliseconds = 350,
        long longPressThresholdMilliseconds = 500,
        bool derivesDoubleClick = true,
        bool derivesLongPress = true)
    {
        if (doubleClickWindowMilliseconds <= 0)
            throw GestureException.InvalidPolicy("doubleClickWindowMilliseconds");
        if (longPressThresholdMilliseconds <= 0)
            throw GestureException.InvalidPolicy("longPressThresholdMilliseconds");

        DoubleClickWindowMilliseconds = doubleClickWindowMilliseconds;
        LongPressThresholdMilliseconds = longPressThresholdMilliseconds;
        DerivesDoubleClick = derivesDoubleClick;
        DerivesLongPress = derivesLongPress;
    }
}

/// <summary>
/// A raw key event from the device. A discriminated union mirroring the macOS
/// <c>DeviceKeyEvent</c>: a key-down, a key-up, a firmware click, or a disconnect.
/// </summary>
public abstract class DeviceKeyEvent
{
    private DeviceKeyEvent() { }

    /// <summary>The key was pressed down.</summary>
    public sealed class Down : DeviceKeyEvent
    {
        public CanonicalKey Key { get; }
        public Down(CanonicalKey key) => Key = key;
    }

    /// <summary>The key was released.</summary>
    public sealed class Up : DeviceKeyEvent
    {
        public CanonicalKey Key { get; }
        public long? DurationMilliseconds { get; }
        public Up(CanonicalKey key, long? durationMilliseconds = null)
        {
            Key = key;
            DurationMilliseconds = durationMilliseconds;
        }
    }

    /// <summary>A firmware click (the device already decided this was a click).</summary>
    public sealed class Click : DeviceKeyEvent
    {
        public CanonicalKey Key { get; }
        public long? DurationMilliseconds { get; }
        public Click(CanonicalKey key, long? durationMilliseconds = null)
        {
            Key = key;
            DurationMilliseconds = durationMilliseconds;
        }
    }

    /// <summary>The device disconnected; all pending state should be cleared.</summary>
    public sealed class Disconnect : DeviceKeyEvent { }
}

/// <summary>A gesture that the router has decided to emit for a key.</summary>
public readonly struct RoutedGesture : IEquatable<RoutedGesture>
{
    public CanonicalKey Key { get; }
    public KeyGesture Gesture { get; }

    public RoutedGesture(CanonicalKey key, KeyGesture gesture)
    {
        Key = key;
        Gesture = gesture;
    }

    public bool Equals(RoutedGesture other) => Key == other.Key && Gesture == other.Gesture;
    public override bool Equals(object? obj) => obj is RoutedGesture other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Key, Gesture);
    public static bool operator ==(RoutedGesture left, RoutedGesture right) => left.Equals(right);
    public static bool operator !=(RoutedGesture left, RoutedGesture right) => !left.Equals(right);
}

/// <summary>
/// Translates a stream of <see cref="DeviceKeyEvent"/>s into <see cref="RoutedGesture"/>s
/// using a <see cref="GesturePolicy"/>. This is a precise port of the macOS
/// <c>GestureRouter</c> struct.
/// </summary>
public sealed class GestureRouter
{
    private readonly GesturePolicy _policy;
    private readonly Dictionary<CanonicalKey, long> _downTimestamps = new();
    private readonly Dictionary<CanonicalKey, long> _pendingClicks = new();
    private readonly HashSet<CanonicalKey> _suppressedClicks = new();
    private long? _lastTimestamp;

    public GestureRouter(GesturePolicy policy)
    {
        _policy = policy ?? throw new ArgumentNullException(nameof(policy));
    }

    /// <summary>
    /// Routes a single device event, returning any gestures that are now determined.
    /// <paramref name="timestampMs"/> must be monotonically non-decreasing, except that
    /// a <see cref="DeviceKeyEvent.Disconnect"/> resets the clock and may carry an
    /// earlier timestamp.
    /// </summary>
    public List<RoutedGesture> Handle(DeviceKeyEvent @event, long timestampMs)
    {
        if (@event is null) throw new ArgumentNullException(nameof(@event));

        // Disconnect resets the session and bypasses timestamp validation so a new
        // session can start its clock from zero.
        if (@event is DeviceKeyEvent.Disconnect)
        {
            ResetSession();
            return new List<RoutedGesture>();
        }

        ValidateTimestamp(timestampMs);
        var output = FlushExpired(timestampMs);

        switch (@event)
        {
            case DeviceKeyEvent.Down down:
                if (_downTimestamps.ContainsKey(down.Key))
                    throw GestureException.DuplicateDown(down.Key);
                _suppressedClicks.Remove(down.Key);
                _downTimestamps[down.Key] = timestampMs;
                break;

            case DeviceKeyEvent.Up up:
                if (!_downTimestamps.Remove(up.Key, out var downTimestamp))
                    throw GestureException.UpWithoutDown(up.Key);
                if (_policy.DerivesLongPress)
                {
                    long measured = timestampMs - downTimestamp;
                    if (measured >= _policy.LongPressThresholdMilliseconds)
                    {
                        _pendingClicks.Remove(up.Key);
                        _suppressedClicks.Add(up.Key);
                        output.Add(new RoutedGesture(up.Key, KeyGesture.Long));
                    }
                }
                break;

            case DeviceKeyEvent.Click click:
                if (_suppressedClicks.Remove(click.Key))
                    break;
                if (_policy.DerivesDoubleClick)
                {
                    if (_pendingClicks.TryGetValue(click.Key, out var pending) &&
                        timestampMs - pending <= _policy.DoubleClickWindowMilliseconds)
                    {
                        _pendingClicks.Remove(click.Key);
                        output.Add(new RoutedGesture(click.Key, KeyGesture.Double));
                    }
                    else
                    {
                        _pendingClicks[click.Key] = timestampMs;
                    }
                }
                else
                {
                    output.Add(new RoutedGesture(click.Key, KeyGesture.Single));
                }
                break;
        }

        return output;
    }

    /// <summary>
    /// Emits single gestures for any pending clicks whose double-click window has expired.
    /// </summary>
    public List<RoutedGesture> Flush(long timestampMs)
    {
        ValidateTimestamp(timestampMs);
        return FlushExpired(timestampMs);
    }

    private void ResetSession()
    {
        _downTimestamps.Clear();
        _pendingClicks.Clear();
        _suppressedClicks.Clear();
        _lastTimestamp = null;
    }

    private void ValidateTimestamp(long timestampMs)
    {
        if (_lastTimestamp is { } last && timestampMs < last)
            throw GestureException.TimestampRegression();
        _lastTimestamp = timestampMs;
    }

    private List<RoutedGesture> FlushExpired(long timestampMs)
    {
        long window = _policy.DoubleClickWindowMilliseconds;

        var expired = new List<(CanonicalKey Key, long Timestamp)>();
        foreach (var kv in _pendingClicks)
        {
            if (timestampMs - kv.Value > window)
                expired.Add((kv.Key, kv.Value));
        }

        // Stable ordering: by pending timestamp, then by canonical key, matching the
        // macOS implementation's sort so emitted singles are deterministic.
        expired.Sort((a, b) =>
        {
            int c = a.Timestamp.CompareTo(b.Timestamp);
            if (c != 0) return c;
            return ((int)a.Key).CompareTo((int)b.Key);
        });

        foreach (var (key, _) in expired)
            _pendingClicks.Remove(key);

        var result = new List<RoutedGesture>(expired.Count);
        foreach (var (key, _) in expired)
            result.Add(new RoutedGesture(key, KeyGesture.Single));
        return result;
    }
}
