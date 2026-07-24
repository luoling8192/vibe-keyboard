using System;
using System.Threading.Tasks;
using VibeBoardKit.Protocol;

namespace VibeBoardKit.LED;

/// <summary>
/// LED state event from the device.
/// </summary>
public sealed class LEDStateEvent
{
    public bool Available { get; }
    public string Source { get; }
    public uint RequestID { get; }
    public string? Reason { get; }
    public bool? Enabled { get; }
    public byte? Brightness { get; }
    public string? Effective { get; }

    public LEDStateEvent(bool available, string source, uint requestID,
        string? reason, bool? enabled, byte? brightness, string? effective)
    {
        Available = available;
        Source = source;
        RequestID = requestID;
        Reason = reason;
        Enabled = enabled;
        Brightness = brightness;
        Effective = effective;
    }
}

/// <summary>
/// Service for LED control operations.
/// Manages query/config commands with epoch validation and acknowledgment tracking.
/// </summary>
public sealed class LEDService
{
    private readonly Func<LEDCommand, Task> _commandWriter;
    private uint _nextRequestID = 1;
    private LEDStateEvent? _lastState;
    private uint? _pendingRequestID;
    private DateTime? _pendingDeadline;

    public LEDStateEvent? CurrentState => _lastState;

    public LEDService(Func<LEDCommand, Task> commandWriter)
    {
        _commandWriter = commandWriter;
    }

    /// <summary>
    /// Query the current LED state.
    /// </summary>
    public async Task Query()
    {
        var requestID = GetNextRequestID();
        _pendingRequestID = requestID;
        _pendingDeadline = DateTime.UtcNow.AddSeconds(1);
        await _commandWriter(new LEDCommand.Query(requestID));
    }

    /// <summary>
    /// Configure LED: enable/disable and set brightness.
    /// </summary>
    public async Task Configure(bool enabled, byte brightness)
    {
        var requestID = GetNextRequestID();
        _pendingRequestID = requestID;
        _pendingDeadline = DateTime.UtcNow.AddSeconds(1);
        await _commandWriter(new LEDCommand.Config(requestID, enabled, brightness));
    }

    /// <summary>
    /// Consume an LED protocol event from the device.
    /// </summary>
    public Task Consume(LEDProtocolEvent evt, long responseTimeNanos)
    {
        if (evt is LEDProtocolEvent.State state)
        {
            // Check if this matches our pending request
            if (_pendingRequestID.HasValue && state.RequestID == _pendingRequestID.Value)
            {
                // Check deadline
                if (_pendingDeadline.HasValue && DateTime.UtcNow <= _pendingDeadline.Value)
                {
                    if (state.Source == "applied" || state.Source == "query")
                    {
                        _lastState = new LEDStateEvent(
                            state.Available, state.Source, state.RequestID,
                            state.Reason, state.Enabled, state.Brightness, state.Effective);
                        _pendingRequestID = null;
                        _pendingDeadline = null;
                    }
                }
                else
                {
                    // Deadline expired
                    _pendingRequestID = null;
                    _pendingDeadline = null;
                }
            }
            else if (state.Source == "applied")
            {
                // Late applied response may update observed state
                _lastState = new LEDStateEvent(
                    state.Available, state.Source, state.RequestID,
                    state.Reason, state.Enabled, state.Brightness, state.Effective);
            }
        }
        return Task.CompletedTask;
    }

    /// <summary>
    /// Synchronize LED state with the current replacement context.
    /// </summary>
    public Task Synchronize(ReplacementSessionContext? context)
    {
        // Reset state when context changes
        if (context == null)
        {
            _lastState = null;
            _pendingRequestID = null;
            _pendingDeadline = null;
        }
        return Task.CompletedTask;
    }

    private uint GetNextRequestID()
    {
        var id = _nextRequestID;
        _nextRequestID++;
        if (_nextRequestID == 0) _nextRequestID = 1; // never zero
        return id;
    }
}

public class LEDServiceException : Exception
{
    public LEDServiceException(string message) : base(message) { }
}
