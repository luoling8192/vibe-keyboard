using System;
using System.Collections.Generic;
using System.Linq;
using System.Management;
using System.Text.RegularExpressions;

namespace VibeBoardKit.USB;

/// <summary>
/// Describes a discovered USB device matching the target VID/PID.
/// </summary>
public sealed class USBDeviceDescriptor : IEquatable<USBDeviceDescriptor>
{
    public string DeviceID { get; }
    public ushort VendorID { get; }
    public ushort ProductID { get; }
    public string SerialNumber { get; }
    public string NormalizedDeviceID { get; }
    public string PortName { get; }

    public USBDeviceDescriptor(string deviceID, ushort vendorID, ushort productID,
        string serialNumber, string normalizedDeviceID, string portName)
    {
        DeviceID = deviceID;
        VendorID = vendorID;
        ProductID = productID;
        SerialNumber = serialNumber;
        NormalizedDeviceID = normalizedDeviceID;
        PortName = portName;
    }

    public bool Equals(USBDeviceDescriptor? other) =>
        other is not null && DeviceID == other.DeviceID;
    public override bool Equals(object? obj) => obj is USBDeviceDescriptor other && Equals(other);
    public override int GetHashCode() => DeviceID.GetHashCode();
}

/// <summary>
/// Errors during USB device discovery.
/// </summary>
public class USBDiscoveryException : Exception
{
    public USBDiscoveryException(string message) : base(message) { }
}

/// <summary>
/// Discovers USB serial devices matching the target VID/PID on Windows.
/// Uses WMI (System.Management) to enumerate PnP entities.
/// </summary>
public static class USBDeviceDiscovery
{
    public const ushort TargetVendorID = 0x303a;
    public const ushort TargetProductID = 0x1001;

    /// <summary>
    /// Normalize a serial number to a 12-hex-character device ID.
    /// Uppercase, keep only hex digits, take the last 12.
    /// </summary>
    public static string NormalizeDeviceID(string serialNumber)
    {
        string hex = serialNumber.ToUpperInvariant();
        // Keep only hex characters
        var sb = new System.Text.StringBuilder();
        foreach (char c in hex)
        {
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
                sb.Append(c);
        }
        string result = sb.ToString();
        if (result.Length < 12)
            throw new USBDiscoveryException($"Invalid serial number (need >= 12 hex chars): {serialNumber}");
        return result[^12..];
    }

    /// <summary>
    /// Find all matching USB serial devices currently connected.
    /// </summary>
    public static List<USBDeviceDescriptor> FindDevices()
    {
        var result = new List<USBDeviceDescriptor>();
        var seen = new HashSet<string>();

        // Use WMI to find PnP devices with matching VID/PID
        // The DeviceID field looks like: USB\VID_303A&PID_1001\...
        using var searcher = new ManagementObjectSearcher(
            "SELECT * FROM Win32_PnPEntity WHERE PNPDeviceID LIKE '%VID_303A&PID_1001%'");

        foreach (var obj in searcher.Get())
        {
            var pnpDeviceID = obj["PNPDeviceID"] as string;
            if (pnpDeviceID == null) continue;

            // Extract serial number from PnP device ID
            // Format: USB\VID_303A&PID_1001\<serial>
            var parts = pnpDeviceID.Split('\\');
            if (parts.Length < 3) continue;
            string serial = parts[2];

            // Find the COM port name associated with this device
            string? portName = FindComPortForDevice(pnpDeviceID);
            if (portName == null) continue;

            string normalizedID;
            try
            {
                normalizedID = NormalizeDeviceID(serial);
            }
            catch (USBDiscoveryException)
            {
                continue;
            }

            if (seen.Contains(portName)) continue;
            seen.Add(portName);

            result.Add(new USBDeviceDescriptor(
                pnpDeviceID, TargetVendorID, TargetProductID,
                serial, normalizedID, portName));
        }

        // Sort by device ID for deterministic ordering
        result.Sort((a, b) => string.Compare(a.DeviceID, b.DeviceID, StringComparison.Ordinal));
        return result;
    }

    /// <summary>
    /// Find the COM port name (e.g., "COM3") associated with a PnP device.
    /// </summary>
    private static string? FindComPortForDevice(string pnpDeviceID)
    {
        // Query Win32_SerialPort or use registry to find COM port
        // On Windows, we can also check Win32_PnPEntity for devices that have "COM" in their caption
        using var searcher = new ManagementObjectSearcher(
            "SELECT * FROM Win32_PnPEntity WHERE PNPDeviceID LIKE '%VID_303A&PID_1001%'");

        foreach (var obj in searcher.Get())
        {
            var id = obj["PNPDeviceID"] as string;
            if (id != pnpDeviceID) continue;

            // Check for COM port in the caption or name
            var caption = obj["Caption"] as string ?? "";
            var name = obj["Name"] as string ?? "";

            // Look for (COMx) pattern
            var match = Regex.Match(caption + " " + name, @"\((COM\d+)\)");
            if (match.Success)
                return match.Groups[1].Value;

            // Sometimes the port is in a different property
            // Try to find it via registry
            return FindComPortViaRegistry(pnpDeviceID);
        }

        return null;
    }

    /// <summary>
    /// Fallback: find COM port via registry lookup.
    /// </summary>
    private static string? FindComPortViaRegistry(string pnpDeviceID)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Enum\" + pnpDeviceID);
            if (key == null) return null;

            foreach (var subKeyName in key.GetSubKeyNames())
            {
                using var subKey = key.OpenSubKey(subKeyName);
                if (subKey == null) continue;

                using var deviceParams = subKey.OpenSubKey("Device Parameters");
                if (deviceParams?.GetValue("PortName") is string portName)
                    return portName;
            }
        }
        catch { }

        return null;
    }

    /// <summary>
    /// Select exactly one device, throwing if none or multiple found.
    /// </summary>
    public static USBDeviceDescriptor SelectOne()
    {
        var devices = FindDevices();
        if (devices.Count == 0)
            throw new USBDiscoveryException("No matching device found");
        if (devices.Count > 1)
            throw new USBDiscoveryException($"Multiple devices found: {devices.Count}");
        return devices[0];
    }
}

/// <summary>
/// Monitors for USB device attach/detach events via polling.
/// </summary>
public sealed class USBDeviceMonitor : IDisposable
{
    private readonly TimeSpan _interval;
    private System.Threading.Timer? _timer;
    private List<USBDeviceDescriptor> _known = new();
    private bool _disposed;

    /// <summary>
    /// Raised when a device is attached. Passes the descriptor.
    /// </summary>
    public event Action<USBDeviceDescriptor>? DeviceAttached;

    /// <summary>
    /// Raised when a device is detached. Passes the device ID.
    /// </summary>
    public event Action<string>? DeviceDetached;

    /// <summary>
    /// Raised on discovery errors.
    /// </summary>
    public event Action<Exception>? Error;

    public USBDeviceMonitor(TimeSpan? interval = null)
    {
        _interval = interval ?? TimeSpan.FromSeconds(1);
    }

    public void Start()
    {
        _timer?.Dispose();
        _timer = new System.Threading.Timer(Poll, null, TimeSpan.Zero, _interval);
    }

    public void Stop()
    {
        _timer?.Dispose();
        _timer = null;
    }

    private void Poll(object? state)
    {
        try
        {
            var current = USBDeviceDiscovery.FindDevices();
            var currentIds = new HashSet<string>(current.Select(d => d.DeviceID));

            // Find newly attached
            foreach (var desc in current)
            {
                if (!_known.Any(k => k.DeviceID == desc.DeviceID))
                    DeviceAttached?.Invoke(desc);
            }

            // Find detached
            foreach (var old in _known)
            {
                if (!currentIds.Contains(old.DeviceID))
                    DeviceDetached?.Invoke(old.DeviceID);
            }

            _known = current;
        }
        catch (Exception ex)
        {
            Error?.Invoke(ex);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Stop();
    }
}
