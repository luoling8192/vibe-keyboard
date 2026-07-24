using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.ObjectModel;
using VibeBoardKit.Protocol;
using VibeBoardKit.USB;
using VibeBoardKit.Assets;
using VibeBoardKit.LED;
using VibeBoardKit.Input;

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
                if (sc.State == USBSessionState.Failed || sc.State == USBSessionState.Incompatible)
                {
                    Connection = sc.State == USBSessionState.Failed
                        ? AppConnectionState.Failed
                        : AppConnectionState.Incompatible;
                    DiagnosticMessage = sc.Error;
                }
                break;

            case USBSessionEvent.ReplacementCapabilities caps:
                UpdateCapabilities(caps.Context);
                break;

            case USBSessionEvent.AudioFrame audio:
                // Audio handling is done in the audio subsystem
                break;

            case USBSessionEvent.StateEvent state:
                // Handle button events
                if (state.Event.Event == "button" && state.Event.Button != null)
                {
                    // Route to key action router
                }
                break;
        }
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
        if (_session?.Descriptor != null)
        {
            Disconnect();
            Attach(_session.Descriptor);
        }
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
