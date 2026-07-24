using System;
using System.CommandLine;
using System.Threading.Tasks;
using VibeBoardKit.USB;
using VibeBoardKit.Protocol;

namespace VibeBoardDiagnostic;

class Program
{
    static async Task<int> Main(string[] args)
    {
        var rootCommand = new RootCommand("VibeBoard diagnostic CLI");

        var listCommand = new Command("list", "List matching USB devices");
        listCommand.SetHandler(() =>
        {
            var devices = USBDeviceDiscovery.FindDevices();
            if (devices.Count == 0)
            {
                Console.WriteLine("No matching devices found.");
                return;
            }
            foreach (var d in devices)
            {
                Console.WriteLine($"  Device ID: {d.NormalizedDeviceID}");
                Console.WriteLine($"  Port: {d.PortName}");
                Console.WriteLine($"  Serial: {d.SerialNumber}");
                Console.WriteLine($"  PNP ID: {d.DeviceID}");
                Console.WriteLine();
            }
        });
        rootCommand.Add(listCommand);

        var inspectCommand = new Command("inspect", "Open device for inspection");
        inspectCommand.SetHandler(async () =>
        {
            try
            {
                var device = USBDeviceDiscovery.SelectOne();
                Console.WriteLine($"Opening {device.PortName} ({device.NormalizedDeviceID})");
                using var session = new USBSession(device);
                session.OpenForInspection();

                Console.WriteLine("Listening for frames (Ctrl+C to stop)...");
                foreach (var evt in session.Events)
                {
                    PrintEvent(evt);
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Error: {ex.Message}");
            }
        });
        rootCommand.Add(inspectCommand);

        var handshakeCommand = new Command("handshake", "Connect and perform USB handshake");
        handshakeCommand.SetHandler(async () =>
        {
            try
            {
                var device = USBDeviceDiscovery.SelectOne();
                Console.WriteLine($"Connecting to {device.PortName} ({device.NormalizedDeviceID})");
                using var session = new USBSession(device);

                var info = await session.Connect();
                Console.WriteLine($"Connected: {info.Hardware} / {info.FirmwareVersion ?? "unknown"}");
                Console.WriteLine($"Device ID: {info.RegistryDeviceID}");
                if (info.Buttons != null)
                    Console.WriteLine($"Buttons: {string.Join(", ", info.Buttons)}");
                if (info.InteractionModes != null)
                    Console.WriteLine($"Interaction modes: {string.Join(", ", info.InteractionModes)}");

                Console.WriteLine("Listening for events (Ctrl+C to stop)...");
                foreach (var evt in session.Events)
                {
                    PrintEvent(evt);
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Error: {ex.Message}");
            }
        });
        rootCommand.Add(handshakeCommand);

        return await rootCommand.InvokeAsync(args);
    }

    static void PrintEvent(USBSessionEvent evt)
    {
        switch (evt)
        {
            case USBSessionEvent.StateChanged sc:
                Console.WriteLine($"[State] {sc.State}" + (sc.Error != null ? $" — {sc.Error}" : ""));
                break;
            case USBSessionEvent.StateEvent se:
                Console.WriteLine($"[Event] {se.Event.Event}" +
                    (se.Event.Button != null ? $" button={se.Event.Button}" : "") +
                    (se.Event.SessionID.HasValue ? $" session={se.Event.SessionID}" : "") +
                    (se.Event.Hardware != null ? $" hardware={se.Event.Hardware}" : ""));
                break;
            case USBSessionEvent.ReplacementCapabilities rc:
                Console.WriteLine($"[Capabilities] protocol={rc.Context.Snapshot.ProtocolVersion}" +
                    $" display={rc.Context.Snapshot.Display.Width}x{rc.Context.Snapshot.Display.Height}");
                break;
            case USBSessionEvent.ReplacementEvent re:
                Console.WriteLine($"[Replacement] {re.Event.GetType().Name}");
                break;
            case USBSessionEvent.AudioFrame af:
                Console.WriteLine($"[Audio] session={af.Frame.Session} seq={af.Frame.Sequence} flags=0x{af.Frame.Flags:X2} len={af.Frame.Payload.Length}");
                break;
            case USBSessionEvent.ProtocolDiagnostic pd:
                Console.WriteLine($"[Diag] type=0x{pd.FrameType:X2} length={pd.Length}");
                break;
            case USBSessionEvent.DiscardedByte db:
                Console.WriteLine($"[Discard] byte=0x{db.Byte:X2} reason={db.Reason}");
                break;
        }
    }
}
