using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.ObjectModel;
using System.IO;
using System.Diagnostics;
using VibeBoardKit.Protocol;
using VibeBoardKit.USB;
using VibeBoardKit.Assets;
using VibeBoardKit.LED;
using VibeBoardKit.Input;
using VibeBoardKit.Audio;
using VibeBoardKit.VKA1;
using VibeKeyboardApp.Input;

namespace VibeKeyboardApp.ViewModels;

/// <summary>
/// Application page selection.
/// </summary>
public enum AppPage
{
    Device,
    Screen,
    Keys,
    Audio,
    Firmware
}

/// <summary>
/// Connection state for UI display.
/// </summary>
public enum AppConnectionState
{
    Disconnected,
    Connecting,
    Ready,
    Incompatible,
    Failed
}

/// <summary>
/// Capability presentation for UI.
/// </summary>
public enum CapabilityPresentation
{
    Absent,
    Unavailable,
    Available
}

/// <summary>
/// Upload state for UI display.
/// </summary>
public enum UploadPresentation
{
    Idle,
    Validating,
    Converting,
    Sending,
    Verifying,
    Active,
    Cancelled,
    Failed
}

/// <summary>
/// The main application view model, coordinating device connection,
/// event handling, and UI state.
/// </summary>
public sealed class AppModel : INotifyPropertyChanged, IDisposable
{
    // --- Connection state ---
    private AppConnectionState _connection = AppConnectionState.Disconnected;
    public AppConnectionState Connection
    {
        get => _connection;
        private set { _connection = value; OnPropertyChanged(); OnPropertyChanged(nameof(IsConnected)); OnPropertyChanged(nameof(ConnectionTitle)); }
    }

    public bool IsConnected => _connection == AppConnectionState.Ready;
    public string ConnectionTitle => _connection switch
    {
        AppConnectionState.Disconnected => "Disconnected",
        AppConnectionState.Connecting => "Connecting",
        AppConnectionState.Ready => "Connected",
        AppConnectionState.Incompatible => "Incompatible",
        AppConnectionState.Failed => "Failed",
        _ => "Unknown"
    };

    // --- Device info ---
    private string _deviceID = "";
    public string DeviceID
    {
        get => _deviceID;
        private set { _deviceID = value; OnPropertyChanged(); }
    }

    private string _firmwareVersion = "Unknown";
    public string FirmwareVersion
    {
        get => _firmwareVersion;
        private set { _firmwareVersion = value; OnPropertyChanged(); }
    }

    // --- Capabilities ---
    private CapabilityPresentation _assetsCapability = CapabilityPresentation.Absent;
    public CapabilityPresentation AssetsCapability
    {
        get => _assetsCapability;
        private set { _assetsCapability = value; OnPropertyChanged(); OnPropertyChanged(nameof(CanUploadAssets)); }
    }

    private CapabilityPresentation _screenCapability = CapabilityPresentation.Absent;
    public CapabilityPresentation ScreenCapability
    {
        get => _screenCapability;
        private set { _screenCapability = value; OnPropertyChanged(); }
    }

    private CapabilityPresentation _ledCapability = CapabilityPresentation.Absent;
    public CapabilityPresentation LEDCapability
    {
        get => _ledCapability;
        private set { _ledCapability = value; OnPropertyChanged(); }
    }

    private CapabilityPresentation _updateCapability = CapabilityPresentation.Absent;
    public CapabilityPresentation UpdateCapability
    {
        get => _updateCapability;
        private set { _updateCapability = value; OnPropertyChanged(); }
    }

    // --- Upload state ---
    private UploadPresentation _upload = UploadPresentation.Idle;
    public UploadPresentation Upload
    {
        get => _upload;
        private set { _upload = value; OnPropertyChanged(); OnPropertyChanged(nameof(UploadLabel)); }
    }

    public string UploadLabel => _upload switch
    {
        UploadPresentation.Idle => "Idle",
        UploadPresentation.Validating => "Validating",
        UploadPresentation.Converting => "Converting",
        UploadPresentation.Sending => "Sending",
        UploadPresentation.Verifying => "Verifying",
        UploadPresentation.Active => "Active",
        UploadPresentation.Cancelled => "Cancelled",
        UploadPresentation.Failed => "Failed",
        _ => "Unknown"
    };

    private double _uploadProgress;
    public double UploadProgress
    {
        get => _uploadProgress;
        private set { _uploadProgress = value; OnPropertyChanged(); }
    }

    // --- Key mapping ---
    public KeyMappingProfile KeyProfile { get; private set; } = KeyMappingProfile.VendorDefault();
    private CanonicalKey _selectedKey = CanonicalKey.K1;
    public CanonicalKey SelectedKey
    {
        get => _selectedKey;
        set { _selectedKey = value; OnPropertyChanged(); }
    }

    // --- Audio state ---
    private string _lastRecording = "None";
    public string LastRecording
    {
        get => _lastRecording;
        private set { _lastRecording = value; OnPropertyChanged(); }
    }

    private bool _saveRecordings;
    public bool SaveRecordings
    {
        get => _saveRecordings;
        set { _saveRecordings = value; OnPropertyChanged(); }
    }

    // --- Screen mode ---
    private ScreenMode _screenMode = ScreenMode.Image;
    public ScreenMode ScreenMode
    {
        get => _screenMode;
        set { _screenMode = value; OnPropertyChanged(); }
    }

    // --- Interaction mode ---
    private InteractionMode _interactionMode = InteractionMode.HoldToTalk;
    public InteractionMode InteractionMode
    {
        get => _interactionMode;
        set { _interactionMode = value; OnPropertyChanged(); }
    }

    // --- Diagnostic ---
    private string? _diagnosticMessage;
    public string? DiagnosticMessage
    {
        get => _diagnosticMessage;
        private set { _diagnosticMessage = value; OnPropertyChanged(); }
    }

    // --- Page selection ---
    private AppPage _selectedPage = AppPage.Device;
    public AppPage SelectedPage
    {
        get => _selectedPage;
        set { _selectedPage = value; OnPropertyChanged(); }
    }

    // --- Dashboard ---
    public string LayoutTitle { get; set; } = "Vibe Dashboard";
    public string WidgetText { get; set; } = "Ready";
    public string StockSymbols { get; set; } = "sh000001";

    // --- Capability flags ---
    public bool CanUploadAssets => _assetsCapability == CapabilityPresentation.Available;

    // --- Private state ---
    private readonly USBDeviceMonitor _monitor;
    private USBSession? _session;
    private AssetTransferService? _assetTransfer;
    private ScreenConfigurationService? _screenConfig;
    private LEDService? _ledService;
    private readonly KeyMappingRepository _keyMappingRepository;
    private Thread? _eventThread;
    private CancellationTokenSource _cts = new();
    private bool _disposed;

    public AppModel()
    {
        _monitor = new USBDeviceMonitor();
        var storePath = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "VibeKeyboard", "key-mappings.json");
        _keyMappingRepository = new KeyMappingRepository(
            new FileConfigurationDataStore(storePath));
        _monitor.DeviceAttached += OnDeviceAttached;
        _monitor.DeviceDetached += OnDeviceDetached;
    }

    /// <summary>
    /// Start monitoring for USB devices.
    /// </summary>
    public void Start()
    {
        // Load key mapping profile
        try
        {
            KeyProfile = _keyMappingRepository.Load();
            OnPropertyChanged(nameof(KeyProfile));
        }
        catch (Exception ex)
        {
            DiagnosticMessage = $"Key mapping load failed: {ex.Message}";
        }

        _monitor.Start();
    }

    private void OnDeviceAttached(USBDeviceDescriptor descriptor)
    {
        System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
        {
            if (_session != null) return;
            Attach(descriptor);
        });
    }

    private void OnDeviceDetached(string deviceId)
    {
        System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
        {
            Disconnect();
        });
    }

    private void Attach(USBDeviceDescriptor descriptor)
    {
        Connection = AppConnectionState.Connecting;
        DeviceID = descriptor.NormalizedDeviceID;

        Task.Run(async () =>
        {
            try
            {
                _session = new USBSession(descriptor);
                _assetTransfer = new AssetTransferService(_session);
                _screenConfig = new ScreenConfigurationService(_session);
                _ledService = new LEDService(cmd => _session.SendLEDCommand(cmd));

                // Register LED consumer
                _session.AddLEDServiceConsumer(async (evt, time) =>
                {
                    await _ledService.Consume(evt, time);
                });

                // Start event consumption thread
                _cts = new CancellationTokenSource();
                _eventThread = new Thread(() => ConsumeEvents(_cts.Token))
                { IsBackground = true, Name = "AppModel-Events" };
                _eventThread.Start();

                var info = await _session.Connect();

                System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    Connection = AppConnectionState.Ready;
                    DeviceID = info.RegistryDeviceID;
                    FirmwareVersion = info.FirmwareVersion ?? "Unknown";

                    // Configure input
                    _ = Task.Run(async () =>
                    {
                        try
                        {
                            await _session.Send(new ControlCommand.InteractionModeCmd(InteractionMode));
                            await _session.Send(new ControlCommand.VoiceKeyCmd(VoiceKey.None));
                        }
                        catch { }
                    });
                });
            }
            catch (Exception ex)
            {
                System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    Connection = AppConnectionState.Failed;
                    DiagnosticMessage = ex.Message;
                    _session?.Dispose();
                    _session = null;
                });
            }
        });
    }

    private void ConsumeEvents(CancellationToken ct)
    {
        if (_session == null) return;

        try
        {
            foreach (var evt in _session.Events)
            {
                if (ct.IsCancellationRequested) break;

                System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    ConsumeSessionEvent(evt);
                });
            }
        }
        catch (OperationCanceledException) { }
    }

    private void ConsumeSessionEvent(USBSessionEvent evt)
    {
        switch (evt)
        {
            case USBSessionEvent.StateChanged sc:
                if (sc.State == USBSessionState.Ready)
                {
                    Connection = AppConnectionState.Ready;
                }
                else if (sc.State == USBSessionState.Failed || sc.State == USBSessionState.Incompatible)
                {
                    Connection = sc.State == USBSessionState.Failed
                        ? AppConnectionState.Failed
                        : AppConnectionState.Incompatible;
                    DiagnosticMessage = sc.Error;
                    ClearSessionPresentation();
                }
                break;

            case USBSessionEvent.ReplacementCapabilities caps:
                UpdateCapabilities(caps.Context);
                break;

            case USBSessionEvent.AudioFrame audio:
                ConsumeAudio(audio.Frame);
                break;

            case USBSessionEvent.StateEvent state:
                ConsumeKeyEvent(state.Event);
                break;
        }
    }

    private void ConsumeKeyEvent(StateEvent @event)
    {
        if (@event.Button == null) return;
        CanonicalKey key;
        try { key = CanonicalKeyExtensions.FromDeviceValue(@event.Button); }
        catch { return; }

        // Simple gesture routing: single click for now
        if (@event.Event == "button_click")
        {
            RouteKeyAction(key, KeyGesture.Single);
        }
        else if (@event.Event == "button_down")
        {
            // Could implement long press detection
        }
        else if (@event.Event == "button_up")
        {
            if (@event.DurationMS.HasValue && @event.DurationMS.Value >= 500)
            {
                RouteKeyAction(key, KeyGesture.Long);
            }
        }
    }

    private void RouteKeyAction(CanonicalKey key, KeyGesture gesture)
    {
        if (!KeyProfile.Mappings.TryGetValue(key, out var bindings)) return;

        var action = gesture switch
        {
            KeyGesture.Single => bindings.Single,
            KeyGesture.Double => bindings.Double,
            KeyGesture.Long => bindings.Long,
            _ => new HostAction.None()
        };

        _ = Task.Run(() => ExecuteHostAction(action));
    }

    private async Task ExecuteHostAction(HostAction action)
    {
        switch (action)
        {
            case HostAction.None:
                break;
            case HostAction.VoiceInput:
                // Toggle voice input - handled by UI state
                break;
            case HostAction.SendEnter:
                KeyboardInjector.SendEnter();
                break;
            case HostAction.SystemCopy:
                KeyboardInjector.CopySelection();
                break;
            case HostAction.InterruptControlC:
                KeyboardInjector.InterruptControlC();
                break;
            case HostAction.WakeApplication:
                // Wake the application window
                break;
            case HostAction.PasteText pt:
                KeyboardInjector.PasteFromClipboard(pt.Text);
                break;
            case HostAction.CustomShortcut cs:
                // Build modifier string: ^ = ctrl, + = shift, % = alt
                var modStr = "";
                foreach (var mod in cs.Shortcut.Modifiers)
                    modStr += mod switch
                    {
                        "control" => "^",
                        "shift" => "+",
                        "option" => "%",
                        _ => ""
                    };
                KeyboardInjector.SendShortcut(modStr, cs.Shortcut.Key);
                break;
            case HostAction.CustomCommand cc:
                try
                {
                    var psi = new ProcessStartInfo
                    {
                        FileName = cc.Command.Executable,
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                    };
                    foreach (var arg in cc.Command.Arguments)
                        psi.ArgumentList.Add(arg);
                    using var proc = Process.Start(psi);
                    if (proc != null)
                    {
                        var cts = new CancellationTokenSource((int)cc.Command.TimeoutMilliseconds);
                        cts.Token.Register(() => { try { proc.Kill(); } catch { } });
                        await proc.WaitForExitAsync(cts.Token);
                    }
                }
                catch (Exception ex)
                {
                    DiagnosticMessage = $"Command failed: {ex.Message}";
                }
                break;
            case HostAction.LaunchApplication la:
                try { Process.Start(new ProcessStartInfo(la.BundleIdentifier) { UseShellExecute = true }); }
                catch (Exception ex) { DiagnosticMessage = $"Launch failed: {ex.Message}"; }
                break;
            case HostAction.ScreenModeAction sm:
                ScreenMode = sm.Mode;
                break;
        }
    }

    private AudioRecordingSession? _audioRecorder;
    private DataOggPageSink? _recordingSink;
    private string? _recordingDestination;

    private void ConsumeAudio(AudioFrame frame)
    {
        if (_audioRecorder == null)
        {
            try
            {
                if (SaveRecordings)
                {
                    var dir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                        "VibeKeyboard", "Recordings");
                    Directory.CreateDirectory(dir);
                    var path = Path.Combine(dir, $"recording_{frame.Session}_{DateTime.Now:yyyyMMdd_HHmmss}.ogg");
                    _recordingDestination = path;
                    _audioRecorder = new AudioRecordingSession(new AtomicFileOggPageSink(path));
                }
                else
                {
                    _recordingSink = new DataOggPageSink();
                    _audioRecorder = new AudioRecordingSession(_recordingSink);
                }
            }
            catch (Exception ex)
            {
                DiagnosticMessage = $"Recording setup failed: {ex.Message}";
                return;
            }
        }

        try
        {
            _audioRecorder.Consume(frame);
            if (_audioRecorder.State.Kind == AudioRecordingStateKind.Completed)
            {
                if (_recordingDestination != null)
                    LastRecording = _recordingDestination;
                else if (_recordingSink != null)
                    LastRecording = $"Session {frame.Session}, {_recordingSink.Data?.Length ?? 0} Ogg bytes (not saved)";
                _audioRecorder = null;
                _recordingSink = null;
                _recordingDestination = null;
            }
        }
        catch (Exception ex)
        {
            DiagnosticMessage = $"Audio recording failed: {ex.Message}";
            _audioRecorder = null;
            _recordingSink = null;
            _recordingDestination = null;
        }
    }

    private void ClearSessionPresentation()
    {
        _audioRecorder?.Cancel();
        _audioRecorder = null;
        _recordingSink = null;
        _recordingDestination = null;
        AssetsCapability = CapabilityPresentation.Absent;
        ScreenCapability = CapabilityPresentation.Absent;
        LEDCapability = CapabilityPresentation.Absent;
        UpdateCapability = CapabilityPresentation.Absent;
    }

    private void UpdateCapabilities(ReplacementSessionContext context)
    {
        var snapshot = context.Snapshot;

        AssetsCapability = snapshot.Assets switch
        {
            FeatureAvailability<AssetsCapability>.Available => CapabilityPresentation.Available,
            FeatureAvailability<AssetsCapability>.Unavailable => CapabilityPresentation.Unavailable,
            null => CapabilityPresentation.Absent,
            _ => CapabilityPresentation.Absent
        };

        ScreenCapability = snapshot.Screen switch
        {
            FeatureAvailability<ScreenCapability>.Available => CapabilityPresentation.Available,
            FeatureAvailability<ScreenCapability>.Unavailable => CapabilityPresentation.Unavailable,
            null => CapabilityPresentation.Absent,
            _ => CapabilityPresentation.Absent
        };

        LEDCapability = snapshot.LED switch
        {
            FeatureAvailability<LEDCapability>.Available => CapabilityPresentation.Available,
            FeatureAvailability<LEDCapability>.Unavailable => CapabilityPresentation.Unavailable,
            null => CapabilityPresentation.Absent,
            _ => CapabilityPresentation.Absent
        };

        UpdateCapability = snapshot.Update switch
        {
            FeatureAvailability<UpdateCapability>.Available => CapabilityPresentation.Available,
            FeatureAvailability<UpdateCapability>.Unavailable => CapabilityPresentation.Unavailable,
            null => CapabilityPresentation.Absent,
            _ => CapabilityPresentation.Absent
        };
    }

    public void Reconnect()
    {
        var descriptor = _session?.Descriptor;
        if (descriptor != null)
        {
            Disconnect();
            Attach(descriptor);
        }
    }

    /// <summary>
    /// Import and upload an image or animation asset.
    /// </summary>
    public async void ImportAndUpload(string filePath, bool pet = false)
    {
        if (_assetTransfer == null || _session == null)
        {
            DiagnosticMessage = "Asset upload: device not connected";
            return;
        }

        try
        {
            Upload = UploadPresentation.Validating;
            var data = File.ReadAllBytes(filePath);
            var decoded = AssetSourceDecoder.Decode(data);

            Upload = UploadPresentation.Converting;
            var context = _session.CurrentReplacementContext;
            if (context == null || context.Snapshot.Assets is not FeatureAvailability<AssetsCapability>.Available assets)
            {
                Upload = UploadPresentation.Failed;
                DiagnosticMessage = "Asset capability not available";
                return;
            }

            var limits = new VKA1Limits(
                assets.Capability.MaxFrames,
                assets.Capability.MinFrameMS,
                assets.Capability.MaxFrameMS,
                assets.Capability.MaxAssetBytes,
                assets.Capability.MaxActiveDecodedBytes);

            var container = ConvertedAssetFactory.MakeVKA1(
                decoded, "contain", new AssetRGB888(0, 0, 0),
                pet ? 119 : 428, pet ? 129 : 142, limits);

            var prepared = new PreparedAsset(container, limits);
            if (pet && prepared.Kind != AssetKind.Animation)
            {
                Upload = UploadPresentation.Failed;
                DiagnosticMessage = "Pet must be an animation";
                return;
            }

            Upload = UploadPresentation.Sending;
            var transferID = (uint)Random.Shared.Next(1, int.MaxValue);
            var result = await _assetTransfer.Upload(prepared, transferID);
            UploadProgress = result.Fraction;
            Upload = UploadPresentation.Active;
        }
        catch (Exception ex)
        {
            Upload = UploadPresentation.Failed;
            DiagnosticMessage = $"Upload failed: {ex.Message}";
        }
    }

    /// <summary>
    /// Cancel an in-progress upload.
    /// </summary>
    public async void CancelUpload()
    {
        if (_assetTransfer == null || _session == null) return;
        Upload = UploadPresentation.Cancelled;
    }

    /// <summary>
    /// Query device screen state.
    /// </summary>
    public async void QueryScreen()
    {
        if (_screenConfig == null) return;
        try { await _screenConfig.Query(); }
        catch (Exception ex) { DiagnosticMessage = $"Screen query failed: {ex.Message}"; }
    }

    /// <summary>
    /// Save key mappings to disk and configure device input.
    /// </summary>
    public async void SaveMappings()
    {
        try
        {
            _keyMappingRepository.Save(KeyProfile);
            if (_session != null)
            {
                var voiceKey = GetDeviceVoiceKey(KeyProfile);
                await _session.Send(new ControlCommand.InteractionModeCmd(InteractionMode));
                await _session.Send(new ControlCommand.VoiceKeyCmd(voiceKey));
            }
        }
        catch (Exception ex)
        {
            DiagnosticMessage = $"Save failed: {ex.Message}";
        }
    }

    private static VoiceKey GetDeviceVoiceKey(KeyMappingProfile profile)
    {
        foreach (var (key, bindings) in profile.Mappings)
        {
            if (bindings.Single is HostAction.VoiceInput ||
                bindings.Double is HostAction.VoiceInput ||
                bindings.Long is HostAction.VoiceInput)
            {
                return key switch
                {
                    CanonicalKey.K1 => VoiceKey.K1,
                    CanonicalKey.K2 => VoiceKey.K2,
                    CanonicalKey.K3 => VoiceKey.K3,
                    CanonicalKey.K4 => VoiceKey.K4,
                    _ => VoiceKey.None
                };
            }
        }
        return VoiceKey.None;
    }

    public void Disconnect()
    {
        _cts.Cancel();
        _session?.Dispose();
        _session = null;
        _assetTransfer = null;
        _screenConfig = null;
        _ledService = null;
        Connection = AppConnectionState.Disconnected;
        AssetsCapability = CapabilityPresentation.Absent;
        ScreenCapability = CapabilityPresentation.Absent;
        LEDCapability = CapabilityPresentation.Absent;
        UpdateCapability = CapabilityPresentation.Absent;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cts.Cancel();
        _monitor.Dispose();
        _session?.Dispose();
        _cts.Dispose();
    }
}
