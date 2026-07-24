using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using VibeBoardKit.Protocol;

namespace VibeBoardKit.USB;

/// <summary>
/// Device info obtained during handshake.
/// </summary>
public sealed class USBDeviceInfo
{
    public string RegistryDeviceID { get; }
    public string? FirmwareDeviceID { get; }
    public string Hardware { get; }
    public string? FirmwareVersion { get; }
    public string[]? Buttons { get; }
    public string[]? InteractionModes { get; }
    public string[]? UIStates { get; }

    public USBDeviceInfo(string registryDeviceID, string? firmwareDeviceID, string hardware,
        string? firmwareVersion, string[]? buttons, string[]? interactionModes, string[]? uiStates)
    {
        RegistryDeviceID = registryDeviceID;
        FirmwareDeviceID = firmwareDeviceID;
        Hardware = hardware;
        FirmwareVersion = firmwareVersion;
        Buttons = buttons;
        InteractionModes = interactionModes;
        UIStates = uiStates;
    }

    internal USBDeviceInfo(USBDeviceDescriptor descriptor, StateEvent @event)
    {
        RegistryDeviceID = descriptor.NormalizedDeviceID;
        FirmwareDeviceID = @event.DeviceID;
        Hardware = @event.Hardware ?? "";
        FirmwareVersion = @event.FirmwareVersion;
        Buttons = @event.Buttons;
        InteractionModes = @event.InteractionModes;
        UIStates = @event.UiStates;
    }
}

/// <summary>
/// Session connection state.
/// </summary>
public enum USBSessionState
{
    Disconnected,
    Opening,
    Inspecting,
    AnnouncingUSBTransport,
    RequestingDeviceInfo,
    SynchronizingConfiguration,
    Ready,
    Incompatible,
    Failed
}

/// <summary>
/// USB session errors.
/// </summary>
public class USBSessionException : Exception
{
    public USBSessionState? State { get; }
    public USBSessionException(string message) : base(message) { }
    public USBSessionException(string message, Exception inner) : base(message, inner) { }
}

/// <summary>
/// Events emitted by the USB session.
/// </summary>
public abstract class USBSessionEvent
{
    private USBSessionEvent() { }

    public sealed class StateChanged : USBSessionEvent
    {
        public USBSessionState State { get; }
        public USBDeviceInfo? Info { get; }
        public string? Error { get; }
        public StateChanged(USBSessionState state, USBDeviceInfo? info = null, string? error = null)
        { State = state; Info = info; Error = error; }
    }

    public sealed class StateEvent : USBSessionEvent
    {
        public Protocol.StateEvent Event { get; }
        public StateEvent(Protocol.StateEvent @event) { Event = @event; }
    }

    public sealed class ReplacementCapabilities : USBSessionEvent
    {
        public ReplacementSessionContext Context { get; }
        public ReplacementCapabilities(ReplacementSessionContext context) { Context = context; }
    }

    public sealed class ReplacementEvent : USBSessionEvent
    {
        public ReplacementProtocolEvent Event { get; }
        public ReplacementEvent(ReplacementProtocolEvent @event) { Event = @event; }
    }

    public sealed class AudioFrame : USBSessionEvent
    {
        public Protocol.AudioFrame Frame { get; }
        public AudioFrame(Protocol.AudioFrame frame) { Frame = frame; }
    }

    public sealed class ProtocolDiagnostic : USBSessionEvent
    {
        public byte FrameType { get; }
        public int Length { get; }
        public ProtocolDiagnostic(byte frameType, int length) { FrameType = frameType; Length = length; }
    }

    public sealed class DiscardedByte : USBSessionEvent
    {
        public byte Byte { get; }
        public FrameDiscardReason Reason { get; }
        public DiscardedByte(byte b, FrameDiscardReason reason) { Byte = b; Reason = reason; }
    }
}

/// <summary>
/// Diagnostics info for a session.
/// </summary>
public sealed class USBDiagnostics
{
    public string Text { get; }
    public List<string> Entries { get; }
    public USBDiagnostics(string text, List<string> entries) { Text = text; Entries = entries; }
}

/// <summary>
/// Monotonic clock for handshake timing.
/// </summary>
public interface IUSBMonotonicClock
{
    long NowNanoseconds();
}

public sealed class ContinuousUSBClock : IUSBMonotonicClock
{
    public long NowNanoseconds() =>
        (long)(Stopwatch.GetTimestamp() * (1_000_000_000.0 / Stopwatch.Frequency));
}

/// <summary>
/// Serial port operations abstraction.
/// </summary>
public interface ISerialPortOperations : IDisposable
{
    void Open(string portName);
    void ConfigureRaw();
    byte[] Read(int maxBytes);
    void Write(byte[] data, TimeSpan timeout);
    bool IsOpen { get; }
}

/// <summary>
/// Serial port operations using System.IO.Ports.SerialPort.
/// </summary>
public sealed class SystemIOPortsOperations : ISerialPortOperations
{
    private SerialPort? _port;
    private readonly Lock _writeLock = new();

    public bool IsOpen => _port?.IsOpen ?? false;

    public void Open(string portName)
    {
        _port = new SerialPort(portName)
        {
            // BaudRate is ignored by USB-CDC, but must be set
            BaudRate = 115200,
            DataBits = 8,
            Parity = Parity.None,
            StopBits = StopBits.One,
            Handshake = Handshake.None,
            ReadBufferSize = 8192,
            WriteBufferSize = 8192,
            ReadTimeout = 50,  // short timeout for non-blocking reads
            WriteTimeout = 2000,
            DtrEnable = false,
            RtsEnable = false,
        };
        _port.Open();
    }

    public void ConfigureRaw()
    {
        // USB-CDC doesn't need termios configuration.
        // Nothing to do on Windows - the raw mode is default.
    }

    public byte[] Read(int maxBytes)
    {
        if (_port == null || !_port.IsOpen) return Array.Empty<byte>();

        try
        {
            var buffer = new byte[maxBytes];
            int read = _port.BaseStream.Read(buffer, 0, maxBytes);
            if (read == 0) return Array.Empty<byte>();
            Array.Resize(ref buffer, read);
            return buffer;
        }
        catch (TimeoutException)
        {
            return Array.Empty<byte>();
        }
        catch (InvalidOperationException)
        {
            return Array.Empty<byte>();
        }
    }

    public void Write(byte[] data, TimeSpan timeout)
    {
        if (_port == null || !_port.IsOpen)
            throw new USBSessionException("Port not open");

        lock (_writeLock)
        {
            _port.BaseStream.WriteTimeout = (int)timeout.TotalMilliseconds;
            _port.BaseStream.Write(data, 0, data.Length);
            _port.BaseStream.Flush();
        }
    }

    public void Dispose()
    {
        try { _port?.Close(); _port?.Dispose(); }
        catch { }
    }
}

/// <summary>
/// Represents an active asset transfer authorization.
/// </summary>
public sealed class ActiveAssetTransfer
{
    public uint TransferID { get; }
    public string SHA256 { get; }
    public uint TotalBytes { get; }
    public AssetKind Kind { get; }
    public uint NextOffset { get; }
    public ushort ChunkBytes { get; }
    internal Guid AuthorizationID { get; }
    internal ulong EpochGeneration { get; }
    internal ulong SnapshotGeneration { get; }

    internal ActiveAssetTransfer(uint transferID, string sha256, uint totalBytes,
        AssetKind kind, uint nextOffset, ushort chunkBytes,
        Guid authorizationID, ulong epochGeneration, ulong snapshotGeneration)
    {
        TransferID = transferID;
        SHA256 = sha256;
        TotalBytes = totalBytes;
        Kind = kind;
        NextOffset = nextOffset;
        ChunkBytes = chunkBytes;
        AuthorizationID = authorizationID;
        EpochGeneration = epochGeneration;
        SnapshotGeneration = snapshotGeneration;
    }
}

/// <summary>
/// Asset transfer outcome.
/// </summary>
public abstract class AssetTransferOutcome
{
    private AssetTransferOutcome() { }

    public abstract uint? TransferID { get; }

    public sealed class Stored : AssetTransferOutcome
    {
        public uint StoredTransferID { get; }
        public string SHA256 { get; }
        public uint TotalBytes { get; }
        public AssetKind Kind { get; }
        public Stored(uint transferID, string sha256, uint totalBytes, AssetKind kind)
        { StoredTransferID = transferID; SHA256 = sha256; TotalBytes = totalBytes; Kind = kind; }
        public override uint? TransferID => StoredTransferID;
    }

    public sealed class Aborted : AssetTransferOutcome
    {
        public uint AbortedTransferID { get; }
        public Aborted(uint transferID) { AbortedTransferID = transferID; }
        public override uint? TransferID => AbortedTransferID;
    }

    public sealed class Rejected : AssetTransferOutcome
    {
        public uint? RejectedTransferID { get; }
        public string Code { get; }
        public uint? NextOffset { get; }
        public string? Message { get; }
        public Rejected(uint? transferID, string code, uint? nextOffset, string? message)
        { RejectedTransferID = transferID; Code = code; NextOffset = nextOffset; Message = message; }
        public override uint? TransferID => RejectedTransferID;
    }

    public sealed class Invalidated : AssetTransferOutcome
    {
        public uint InvalidatedTransferID { get; }
        public Invalidated(uint transferID) { InvalidatedTransferID = transferID; }
        public override uint? TransferID => InvalidatedTransferID;
    }
}

/// <summary>
/// The core USB session for communicating with a VibeBoard device.
/// Owns the serial port, frame parser, handshake state machine, heartbeat,
/// and dispatches typed events to consumers.
/// </summary>
public sealed class USBSession : IDisposable
{
    public USBDeviceDescriptor Descriptor { get; }
    public USBSessionState CurrentState { get; private set; } = USBSessionState.Disconnected;

    private readonly ISerialPortOperations _operations;
    private readonly IUSBMonotonicClock _clock;
    private readonly int _readChunkSize;
    private readonly int _diagnosticByteLimit;
    private readonly int _diagnosticEntryLimit;
    private readonly TimeSpan _heartbeatInterval;

    private readonly FrameStreamParser _parser;
    private Thread? _readThread;
    private Timer? _heartbeatTimer;
    private CancellationTokenSource _cts = new();
    private readonly object _stateLock = new();
    private readonly BlockingCollection<USBSessionEvent> _eventQueue =
        new(new ConcurrentQueue<USBSessionEvent>(), 256);

    // Handshake state
    private StateEvent? _receivedDeviceInfo;
    private ReplacementCapabilitySnapshot? _receivedCapabilities;
    private StateEvent? _pendingReplacementDeviceInfo;
    private ulong _epochGeneration;
    private ulong _snapshotGeneration;
    private ReplacementSessionContext? _replacementContext;

    // Asset transfer state
    private ActiveAssetTransfer? _activeTransfer;
    private readonly Dictionary<uint, AssetTransferOutcome> _transferOutcomes = new();

    // Screen state
    private uint? _currentScreenRevision;
    private bool _currentScreenConfigured;
    private ScreenMode? _currentScreenMode;

    // Diagnostics
    private readonly List<byte> _diagnosticBytes = new();
    private readonly List<string> _diagnosticEntries = new();

    // Event consumer callbacks
    private readonly Dictionary<Guid, Func<LEDProtocolEvent, long, Task>> _ledConsumers = new();
    private readonly Dictionary<Guid, Func<WidgetProtocolEvent, Task>> _widgetConsumers = new();

    // Write serialization
    private readonly Lock _writeLock = new();

    // Capabilities identity for validation
    private sealed class CapabilityIdentity
    {
        public ushort? ProtocolVersion { get; }
        public CapabilityDisplay? Display { get; }
        public CapabilityIdentity(ushort? protocolVersion, CapabilityDisplay? display)
        { ProtocolVersion = protocolVersion; Display = display; }
    }
    private CapabilityIdentity? _acceptedCapabilityIdentity;
    private bool _sawUnpairedCapabilities;

    public USBSession(
        USBDeviceDescriptor descriptor,
        ISerialPortOperations? operations = null,
        IUSBMonotonicClock? clock = null,
        int readChunkSize = 4096,
        int receiveBufferLimit = 4096,
        int diagnosticByteLimit = 2048,
        int diagnosticEntryLimit = 128,
        TimeSpan? heartbeatInterval = null)
    {
        Descriptor = descriptor;
        _operations = operations ?? new SystemIOPortsOperations();
        _clock = clock ?? new ContinuousUSBClock();
        _readChunkSize = readChunkSize;
        _diagnosticByteLimit = diagnosticByteLimit;
        _diagnosticEntryLimit = diagnosticEntryLimit;
        _heartbeatInterval = heartbeatInterval ?? TimeSpan.FromSeconds(2);
        _parser = new FrameStreamParser(receiveBufferLimit);
    }

    /// <summary>
    /// Events from this session. Consumed via enumerable.
    /// </summary>
    public IEnumerable<USBSessionEvent> Events => _eventQueue.GetConsumingEnumerable(_cts.Token);

    /// <summary>
    /// Current replacement context, or null if not negotiated.
    /// </summary>
    public ReplacementSessionContext? CurrentReplacementContext => _replacementContext;

    /// <summary>
    /// Connect to the device: open port, announce USB transport, request device info,
    /// and wait for the handshake to complete.
    /// </summary>
    public async Task<USBDeviceInfo> Connect(TimeSpan? handshakeTimeout = null)
    {
        handshakeTimeout ??= TimeSpan.FromSeconds(10);

        lock (_stateLock)
        {
            if (CurrentState != USBSessionState.Disconnected)
                throw new USBSessionException("Session already open");
            SetState(USBSessionState.Opening);
        }

        _operations.Open(Descriptor.PortName);
        _operations.ConfigureRaw();

        // Start read thread
        _readThread = new Thread(ReadLoop) { IsBackground = true, Name = "USBSession-Read" };
        _readThread.Start();

        try
        {
            // Step 1: announce USB transport
            SetState(USBSessionState.AnnouncingUSBTransport);
            await SendCommand(new ControlCommand.AnnounceUSBTransport());

            // Step 2: request device info
            SetState(USBSessionState.RequestingDeviceInfo);
            await SendCommand(new ControlCommand.GetDeviceInfo());

            // Step 3: set UI state to ready
            SetState(USBSessionState.SynchronizingConfiguration);
            await SendCommand(new ControlCommand.UIState(DeviceUIState.Ready, ""));

            // Wait for device_info (and capabilities if replacement)
            var start = _clock.NowNanoseconds();
            var deadline = start + (long)handshakeTimeout.Value.TotalMilliseconds * 1_000_000;
            var nextInfoRequest = start + 1_500_000_000;
            var nextPing = start + 2_000_000_000;

            while (_clock.NowNanoseconds() < deadline)
            {
                _cts.Token.ThrowIfCancellationRequested();

                lock (_stateLock)
                {
                    if (CurrentState == USBSessionState.Failed)
                        throw new USBSessionException("Session failed during handshake");
                }

                if (_receivedDeviceInfo != null)
                {
                    var @event = _receivedDeviceInfo;
                    if (@event.Hardware != "vibe_keyboard")
                    {
                        SetState(USBSessionState.Incompatible);
                        throw new USBSessionException($"Incompatible hardware: {@event.Hardware}");
                    }

                    if (@event.ReplacementProtocol.HasValue)
                    {
                        if (@event.ReplacementProtocol.Value != 1)
                            throw new USBSessionException("Unsupported replacement protocol");

                        if (_receivedCapabilities == null)
                        {
                            await Task.Delay(25, _cts.Token);
                            continue;
                        }

                        var caps = _receivedCapabilities;
                        if (caps.ProtocolVersion != 1 ||
                            caps.Display.Width != 428 || caps.Display.Height != 142 ||
                            caps.Display.Format != "rgb565")
                        {
                            throw new USBSessionException("Invalid replacement capabilities");
                        }
                    }
                    else if (_sawUnpairedCapabilities)
                    {
                        throw new USBSessionException("Replacement capabilities without discriminator");
                    }

                    var info = new USBDeviceInfo(Descriptor, @event);
                    SetState(USBSessionState.Ready, info);
                    StartHeartbeat();
                    return info;
                }

                var now = _clock.NowNanoseconds();
                if (now >= nextInfoRequest)
                {
                    await SendCommand(new ControlCommand.GetDeviceInfo());
                    nextInfoRequest = now + 1_500_000_000;
                }
                if (now >= nextPing)
                {
                    await SendCommand(new ControlCommand.Ping());
                    nextPing = now + 2_000_000_000;
                }

                await Task.Delay(25, _cts.Token);
            }

            throw new USBSessionException("Handshake timed out");
        }
        catch (OperationCanceledException)
        {
            Terminate(USBSessionState.Disconnected);
            throw new USBSessionException("Cancelled");
        }
        catch (USBSessionException)
        {
            throw;
        }
        catch (Exception ex)
        {
            Terminate(USBSessionState.Failed);
            throw new USBSessionException($"Protocol failure: {ex.Message}", ex);
        }
    }

    /// <summary>
    /// Open the port for inspection only (no handshake).
    /// </summary>
    public void OpenForInspection()
    {
        SetState(USBSessionState.Inspecting);
        _operations.Open(Descriptor.PortName);
        _operations.ConfigureRaw();
        _readThread = new Thread(ReadLoop) { IsBackground = true, Name = "USBSession-Read" };
        _readThread.Start();
    }

    /// <summary>
    /// Send a control command.
    /// </summary>
    public async Task Send(ControlCommand command, TimeSpan? timeout = null)
    {
        timeout ??= TimeSpan.FromSeconds(2);
        var frame = FrameEncoder.Encode(command);
        await Task.Run(() =>
        {
            lock (_writeLock)
            {
                _operations.Write(frame, timeout.Value);
            }
        });
    }

    /// <summary>
    /// Send an asset command (requires valid replacement context).
    /// </summary>
    public async Task SendAssetCommand(AssetCommand command, TimeSpan? timeout = null)
    {
        var context = RequireReplacementContext();
        _ = RequireAssets(context);
        var frame = ReplacementCommandEncoder.Encode(command);

        // Register pending operations based on command type
        if (command is AssetCommand.Begin begin)
        {
            // Will be matched against vk_asset_ready
        }

        await Task.Run(() =>
        {
            lock (_writeLock) { _operations.Write(frame, timeout ?? TimeSpan.FromSeconds(2)); }
        });
    }

    /// <summary>
    /// Send a binary asset chunk (type 0x40).
    /// </summary>
    public async Task SendAssetChunk(byte[] payload, ActiveAssetTransfer authorization, TimeSpan? timeout = null)
    {
        var context = RequireReplacementContext();
        var assets = RequireAssets(context);

        if (authorization.NextOffset >= authorization.TotalBytes)
            throw new USBSessionException("Invalid asset transfer authorization: offset >= total");

        int remaining = (int)(authorization.TotalBytes - authorization.NextOffset);
        int limit = Math.Min(Math.Min(authorization.ChunkBytes, assets.ChunkBytes),
            Math.Min(AssetChunkEncoder.MaximumPayloadLength, remaining));

        if (payload.Length < 1 || payload.Length > limit)
            throw new USBSessionException("Invalid asset chunk size");

        var frame = AssetChunkEncoder.Encode(
            authorization.TransferID, authorization.NextOffset, payload);

        await Task.Run(() =>
        {
            lock (_writeLock) { _operations.Write(frame, timeout ?? TimeSpan.FromSeconds(2)); }
        });
    }

    /// <summary>
    /// Send a screen command.
    /// </summary>
    public async Task SendScreenCommand(ScreenCommand command, TimeSpan? timeout = null)
    {
        var context = RequireReplacementContext();
        _ = RequireScreen(context);
        _ = RequireAssets(context);

        var frame = ReplacementCommandEncoder.Encode(command);
        await Task.Run(() =>
        {
            lock (_writeLock) { _operations.Write(frame, timeout ?? TimeSpan.FromSeconds(2)); }
        });
    }

    /// <summary>
    /// Send an LED command.
    /// </summary>
    public async Task SendLEDCommand(LEDCommand command, TimeSpan? timeout = null)
    {
        var context = RequireReplacementContext();
        if (context.Snapshot.LED == null)
            throw new USBSessionException("LED capability absent");

        var frame = LEDProtocolCodec.Encode(command);
        await Task.Run(() =>
        {
            lock (_writeLock) { _operations.Write(frame, timeout ?? TimeSpan.FromSeconds(2)); }
        });
    }

    /// <summary>
    /// Send a widget update.
    /// </summary>
    public async Task SendWidgetUpdate(WidgetUpdateCommand command, TimeSpan? timeout = null)
    {
        var context = RequireReplacementContext();
        var screen = RequireScreen(context);
        _ = RequireAssets(context);

        var frame = WidgetProtocolCodec.Encode(command, screen.MaxWidgetValueBytes);
        await Task.Run(() =>
        {
            lock (_writeLock) { _operations.Write(frame, timeout ?? TimeSpan.FromSeconds(2)); }
        });
    }

    /// <summary>
    /// Get current diagnostics.
    /// </summary>
    public USBDiagnostics GetDiagnostics()
    {
        var text = new StringBuilder();
        foreach (byte b in _diagnosticBytes)
        {
            if (b == 0x09 || b == 0x0a || b == 0x0d || (b >= 0x20 && b <= 0x7e))
                text.Append((char)b);
            else
                text.Append('.');
        }
        return new USBDiagnostics(text.ToString(), new List<string>(_diagnosticEntries));
    }

    /// <summary>
    /// Disconnect from the device.
    /// </summary>
    public void Disconnect()
    {
        Terminate(USBSessionState.Disconnected);
    }

    /// <summary>
    /// Add a consumer for LED events.
    /// </summary>
    public Guid AddLEDServiceConsumer(Func<LEDProtocolEvent, long, Task> consumer)
    {
        var id = Guid.NewGuid();
        lock (_ledConsumers) { _ledConsumers[id] = consumer; }
        return id;
    }

    /// <summary>
    /// Remove an LED consumer.
    /// </summary>
    public void RemoveLEDServiceConsumer(Guid id)
    {
        lock (_ledConsumers) { _ledConsumers.Remove(id); }
    }

    /// <summary>
    /// Add a consumer for widget events.
    /// </summary>
    public Guid AddWidgetConsumer(Func<WidgetProtocolEvent, Task> consumer)
    {
        var id = Guid.NewGuid();
        lock (_widgetConsumers) { _widgetConsumers[id] = consumer; }
        return id;
    }

    /// <summary>
    /// Current active asset transfer, if any.
    /// </summary>
    public ActiveAssetTransfer? CurrentActiveAssetTransfer => _activeTransfer;

    /// <summary>
    /// Get the outcome of a completed transfer.
    /// </summary>
    public AssetTransferOutcome? GetAssetTransferOutcome(uint transferID)
    {
        return _transferOutcomes.TryGetValue(transferID, out var outcome) ? outcome : null;
    }

    // --- Internal implementation ---

    private void SetState(USBSessionState state, USBDeviceInfo? info = null, string? error = null)
    {
        lock (_stateLock)
        {
            CurrentState = state;
        }
        EnqueueEvent(new USBSessionEvent.StateChanged(state, info, error));
    }

    private void StartHeartbeat()
    {
        _heartbeatTimer?.Dispose();
        _heartbeatTimer = new Timer(async _ =>
        {
            try { await Send(new ControlCommand.Ping()); }
            catch { /* heartbeat failure will be caught by read loop */ }
        }, null, _heartbeatInterval, _heartbeatInterval);
    }

    private void ReadLoop()
    {
        while (!_cts.IsCancellationRequested)
        {
            try
            {
                byte[] data = _operations.Read(_readChunkSize);
                if (data.Length == 0)
                {
                    // Short sleep to avoid busy-looping when no data
                    Thread.Sleep(10);
                    continue;
                }

                // Add to diagnostic bytes
                lock (_diagnosticBytes)
                {
                    _diagnosticBytes.AddRange(data);
                    if (_diagnosticBytes.Count > _diagnosticByteLimit)
                        _diagnosticBytes.RemoveRange(0, _diagnosticBytes.Count - _diagnosticByteLimit);
                }

                // Parse frames
                var events = _parser.Append(data);
                foreach (var evt in events)
                {
                    if (evt.Frame.HasValue)
                    {
                        ConsumeFrame(evt.Frame.Value);
                    }
                    else if (evt.DiscardedByte.HasValue)
                    {
                        EnqueueEvent(new USBSessionEvent.DiscardedByte(
                            evt.DiscardedByte.Value, evt.Reason!.Value));
                    }
                }
            }
            catch (USBSessionException)
            {
                break;
            }
            catch (Exception)
            {
                // Transient read error, continue
            }
        }

        Terminate(USBSessionState.Disconnected);
    }

    private void ConsumeFrame(RawFrame frame)
    {
        // Record diagnostic entry
        var diagEntry = $"frame type=0x{frame.Type:X2} length={frame.Bytes.Length}";
        lock (_diagnosticEntries)
        {
            _diagnosticEntries.Add(diagEntry);
            if (_diagnosticEntries.Count > _diagnosticEntryLimit)
                _diagnosticEntries.RemoveAt(0);
        }

        EnqueueEvent(new USBSessionEvent.ProtocolDiagnostic((byte)frame.Type, frame.Bytes.Length));

        if (frame.Type == FrameType.State)
        {
            ConsumeStateFrame(frame);
        }
        else
        {
            try
            {
                var parsed = FrameDecoder.Decode(frame);
                if (parsed is ParsedFrame.Audio audio)
                {
                    EnqueueEvent(new USBSessionEvent.AudioFrame(audio.Frame));
                }
            }
            catch (ProtocolException)
            {
                // Skip unparseable frames
            }
        }
    }

    private void ConsumeStateFrame(RawFrame frame)
    {
        byte[] body;
        try
        {
            body = FrameDecoder.DecodeStateBody(frame.Bytes);
        }
        catch (ProtocolException)
        {
            return;
        }

        // Parse to get the event name
        using var doc = JsonDocument.Parse(body);
        var root = doc.RootElement;
        if (!root.TryGetProperty("event", out var eventProp))
            return;
        string eventName = eventProp.GetString() ?? "";

        // Check if this is a replacement protocol event
        bool isKnownReplacement = eventName == "vk_error" ||
            eventName == "vk_storage_formatted" ||
            eventName.StartsWith("vk_asset_") ||
            eventName.StartsWith("vk_screen_") ||
            eventName == "vk_led_state" ||
            eventName == "vk_widget_applied" ||
            eventName == "vk_capabilities";

        if (eventName == "vk_capabilities")
        {
            try
            {
                var snapshot = ReplacementCapabilitySnapshot.Decode(body);
                ConsumeCapabilities(snapshot);
                EnqueueEvent(new USBSessionEvent.ReplacementCapabilities(_replacementContext!));
            }
            catch (ProtocolException) { }
            return;
        }

        if (isKnownReplacement)
        {
            try
            {
                var replacementEvent = ReplacementEventDecoder.Decode(body);

                // Handle LED events
                if (replacementEvent is ReplacementProtocolEvent.LED ledEvent)
                {
                    EnqueueEvent(new USBSessionEvent.ReplacementEvent(replacementEvent));
                    long responseTime = _clock.NowNanoseconds();
                    lock (_ledConsumers)
                    {
                        foreach (var consumer in _ledConsumers.Values)
                        {
                            _ = consumer(ledEvent.Event, responseTime);
                        }
                    }
                    return;
                }

                // Handle widget events
                if (replacementEvent is ReplacementProtocolEvent.Widget widgetEvent)
                {
                    EnqueueEvent(new USBSessionEvent.ReplacementEvent(replacementEvent));
                    lock (_widgetConsumers)
                    {
                        foreach (var consumer in _widgetConsumers.Values)
                        {
                            _ = consumer(widgetEvent.Event);
                        }
                    }
                    return;
                }

                // Handle asset/screen events
                ConsumeReplacementEvent(replacementEvent);
                EnqueueEvent(new USBSessionEvent.ReplacementEvent(replacementEvent));
            }
            catch (ProtocolException) { }
            return;
        }

        // Non-replacement event: device_info, button events, etc.
        try
        {
            var stateEvent = FrameDecoder.DecodeState(frame.Bytes);
            if (stateEvent is ParsedFrame.State state)
            {
                ConsumeHandshakeEvent(state.Event);
                EnqueueEvent(new USBSessionEvent.StateEvent(state.Event));
            }
        }
        catch (ProtocolException) { }
    }

    private void ConsumeHandshakeEvent(StateEvent @event)
    {
        if (@event.Event == "device_info")
        {
            if (@event.ReplacementProtocol.HasValue)
            {
                // Replacement device: wait for capabilities
                _pendingReplacementDeviceInfo = @event;
                _replacementContext = null;
                _epochGeneration++;
            }
            else
            {
                _receivedDeviceInfo = @event;
            }
        }
        else if (_pendingReplacementDeviceInfo != null && @event.Event != "vk_capabilities")
        {
            // Non-capabilities event while waiting for capabilities
            // This is allowed for some events; only flag unpaired if it's a replacement event
        }
    }

    private void ConsumeCapabilities(ReplacementCapabilitySnapshot snapshot)
    {
        // Validate identity consistency
        var identity = new CapabilityIdentity(snapshot.ProtocolVersion, snapshot.Display);
        if (_acceptedCapabilityIdentity != null)
        {
            // Must match previously accepted identity
            if (identity.ProtocolVersion != _acceptedCapabilityIdentity.ProtocolVersion ||
                identity.Display != _acceptedCapabilityIdentity.Display)
            {
                _sawUnpairedCapabilities = true;
                return;
            }
        }
        _acceptedCapabilityIdentity = identity;
        _sawUnpairedCapabilities = false;

        _snapshotGeneration++;
        _receivedCapabilities = snapshot;
        _replacementContext = new ReplacementSessionContext(
            _epochGeneration, _snapshotGeneration, snapshot);

        // Invalidate asset transfers and screen state
        _activeTransfer = null;
        _transferOutcomes.Clear();

        // Initialize screen state from capability
        if (snapshot.Screen is FeatureAvailability<ScreenCapability>.Available screenAvail)
        {
            _currentScreenRevision = screenAvail.Capability.Revision;
            _currentScreenConfigured = screenAvail.Capability.Configured;
            _currentScreenMode = screenAvail.Capability.Configured && screenAvail.Capability.Modes.Length > 0
                ? ScreenModeExtensions.FromWire(screenAvail.Capability.Modes[0])
                : null;
        }
    }

    private void ConsumeReplacementEvent(ReplacementProtocolEvent evt)
    {
        switch (evt)
        {
            case ReplacementProtocolEvent.Asset assetEvt:
                ConsumeAssetEvent(assetEvt.Event);
                break;

            case ReplacementProtocolEvent.Screen screenEvt:
                ConsumeScreenEvent(screenEvt.Event);
                break;
        }
    }

    private void ConsumeAssetEvent(ReplacementAssetEvent evt)
    {
        switch (evt)
        {
            case ReplacementAssetEvent.Ready ready:
                _activeTransfer = new ActiveAssetTransfer(
                    ready.TransferID, ready.SHA256, ready.TotalBytes, ready.Kind,
                    ready.NextOffset, ready.ChunkBytes, Guid.NewGuid(),
                    _epochGeneration, _snapshotGeneration);
                break;

            case ReplacementAssetEvent.Stored stored:
                _transferOutcomes[stored.TransferID] = new AssetTransferOutcome.Stored(
                    stored.TransferID, stored.SHA256, stored.TotalBytes, stored.Kind);
                _activeTransfer = null;
                break;

            case ReplacementAssetEvent.Aborted aborted:
                _transferOutcomes[aborted.TransferID] = new AssetTransferOutcome.Aborted(aborted.TransferID);
                _activeTransfer = null;
                break;

            case ReplacementAssetEvent.Progress progress:
                if (_activeTransfer != null && _activeTransfer.TransferID == progress.TransferID)
                {
                    _activeTransfer = new ActiveAssetTransfer(
                        _activeTransfer.TransferID, _activeTransfer.SHA256,
                        _activeTransfer.TotalBytes, _activeTransfer.Kind,
                        progress.NextOffset, _activeTransfer.ChunkBytes,
                        _activeTransfer.AuthorizationID,
                        _activeTransfer.EpochGeneration, _activeTransfer.SnapshotGeneration);
                }
                break;

            case ReplacementAssetEvent.Invalidated invalidated:
                _transferOutcomes[invalidated.TransferID] = new AssetTransferOutcome.Invalidated(invalidated.TransferID);
                _activeTransfer = null;
                break;
        }
    }

    private void ConsumeScreenEvent(ReplacementScreenEvent evt)
    {
        switch (evt)
        {
            case ReplacementScreenEvent.State state:
                _currentScreenRevision = state.Revision;
                _currentScreenConfigured = state.Configured;
                _currentScreenMode = state.Mode;
                break;

            case ReplacementScreenEvent.Committed committed:
                _currentScreenRevision = committed.Revision;
                _currentScreenConfigured = true;
                break;
        }
    }

    private ReplacementSessionContext RequireReplacementContext()
    {
        if (_replacementContext == null)
            throw new USBSessionException("No replacement context available");
        return _replacementContext;
    }

    private AssetsCapability RequireAssets(ReplacementSessionContext context)
    {
        if (context.Snapshot.Assets is not FeatureAvailability<AssetsCapability>.Available assets)
            throw new USBSessionException("Assets capability not available");
        return assets.Capability;
    }

    private ScreenCapability RequireScreen(ReplacementSessionContext context)
    {
        if (context.Snapshot.Screen is not FeatureAvailability<ScreenCapability>.Available screen)
            throw new USBSessionException("Screen capability not available");
        return screen.Capability;
    }

    private void EnqueueEvent(USBSessionEvent evt)
    {
        try { _eventQueue.Add(evt, _cts.Token); }
        catch { /* queue full or cancelled, drop */ }
    }

    private void Terminate(USBSessionState finalState)
    {
        _heartbeatTimer?.Dispose();
        _heartbeatTimer = null;
        _cts.Cancel();
        _operations.Dispose();
        SetState(finalState);
        try { _eventQueue.CompleteAdding(); } catch { }
    }

    public void Dispose()
    {
        Terminate(USBSessionState.Disconnected);
        _cts.Dispose();
        _eventQueue.Dispose();
    }
}
