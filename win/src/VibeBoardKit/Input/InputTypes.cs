using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using VibeBoardKit.Protocol;

namespace VibeBoardKit.Input;

/// <summary>
/// The canonical, device-independent identity of a physical key.
/// Mirrors the macOS <c>CanonicalKey</c> enum.
/// </summary>
public enum CanonicalKey
{
    K1,
    K2,
    K3,
    K4
}

/// <summary>
/// Extension methods for <see cref="CanonicalKey"/>.
/// </summary>
public static class CanonicalKeyExtensions
{
    /// <summary>
    /// Maps a raw device-reported key string to a <see cref="CanonicalKey"/>.
/// <c>k1</c>/<c>secondary</c> → K1, <c>k2</c> → K2, <c>k3</c> → K3, <c>k4</c>/<c>primary</c> → K4.
    /// Throws <see cref="InputConfigurationException"/> for unknown values.
    /// </summary>
    public static CanonicalKey FromDeviceValue(string deviceValue)
    {
        return deviceValue switch
        {
            "k1" or "secondary" => CanonicalKey.K1,
            "k2" => CanonicalKey.K2,
            "k3" => CanonicalKey.K3,
            "k4" or "primary" => CanonicalKey.K4,
            _ => throw new InputConfigurationException(
                InputConfigurationReason.UnknownKey, deviceValue)
        };
    }

    /// <summary>The lowercase wire string used as a JSON mapping key (<c>k1</c>..<c>k4</c>).</summary>
    public static string WireKey(this CanonicalKey key) => key switch
    {
        CanonicalKey.K1 => "k1",
        CanonicalKey.K2 => "k2",
        CanonicalKey.K3 => "k3",
        CanonicalKey.K4 => "k4",
        _ => throw new InvalidOperationException($"Unknown canonical key: {key}")
    };

    /// <summary>Parses a JSON mapping key (<c>k1</c>..<c>k4</c>) into a <see cref="CanonicalKey"/>.</summary>
    public static CanonicalKey ParseKey(string wire) => wire switch
    {
        "k1" => CanonicalKey.K1,
        "k2" => CanonicalKey.K2,
        "k3" => CanonicalKey.K3,
        "k4" => CanonicalKey.K4,
        _ => throw new JsonException($"Unknown canonical key wire value: {wire}")
    };
}

/// <summary>
/// The kind of gesture recognized on a key: a single press, a double press, or a long press.
/// </summary>
public enum KeyGesture
{
    Single,
    Double,
    Long
}

/// <summary>
/// A keyboard shortcut composed of modifier keys and a single key.
/// The <c>key</c> is normalized to a trimmed, lowercased form. Modifiers are restricted to
/// the canonical set <c>command</c>, <c>control</c>, <c>function</c>, <c>option</c>, <c>shift</c>.
/// </summary>
public sealed class KeyboardShortcut : IEquatable<KeyboardShortcut>
{
    private static readonly HashSet<string> AllowedModifiers = new(StringComparer.Ordinal)
    {
        "command", "control", "function", "option", "shift"
    };

    /// <summary>The set of modifier key names (validated against the canonical set).</summary>
    public IReadOnlySet<string> Modifiers { get; }

    /// <summary>The normalized (trimmed, lowercased) key.</summary>
    public string Key { get; }

    public KeyboardShortcut(IEnumerable<string>? modifiers, string key)
    {
        if (key is null)
            throw new InputConfigurationException(
                InputConfigurationReason.MissingAssociatedValue, "custom_shortcut");

        var normalizedKey = key.Trim().ToLowerInvariant();
        if (normalizedKey.Length == 0)
            throw new InputConfigurationException(
                InputConfigurationReason.MissingAssociatedValue, "custom_shortcut");
        if (normalizedKey.Any(char.IsWhiteSpace))
            throw new InputConfigurationException(
                InputConfigurationReason.InvalidShortcutKey, key);

        var set = new HashSet<string>(StringComparer.Ordinal);
        if (modifiers is not null)
        {
            foreach (var m in modifiers)
            {
                if (m is null || !AllowedModifiers.Contains(m))
                    throw new InputConfigurationException(
                        InputConfigurationReason.InvalidShortcutKey,
                        m is null ? "null" : m);
                set.Add(m);
            }
        }

        Modifiers = set;
        Key = normalizedKey;
    }

    public bool Equals(KeyboardShortcut? other) =>
        other is not null && Key == other.Key && Modifiers.SetEquals(other.Modifiers);
    public override bool Equals(object? obj) => Equals(obj as KeyboardShortcut);
    public override int GetHashCode() => HashCode.Combine(Key);
}

/// <summary>
/// A host command to execute: an absolute executable path, literal arguments, and a bounded timeout.
/// </summary>
public sealed class CommandSpecification : IEquatable<CommandSpecification>
{
    private const uint MaxTimeoutMilliseconds = 300_000;

    /// <summary>The absolute path to the executable.</summary>
    public string Executable { get; }

    /// <summary>The literal argument vector. Never null.</summary>
    public string[] Arguments { get; }

    /// <summary>The execution timeout in milliseconds, in the range [1, 300000].</summary>
    public uint TimeoutMilliseconds { get; }

    public CommandSpecification(string executable, uint timeoutMilliseconds)
        : this(executable, null, timeoutMilliseconds) { }

    public CommandSpecification(string executable, string[]? arguments, uint timeoutMilliseconds)
    {
        if (executable is null)
            throw new InputConfigurationException(
                InputConfigurationReason.MissingAssociatedValue, "custom_command");

        var normalizedExecutable = executable.Trim();
        if (normalizedExecutable.Length == 0)
            throw new InputConfigurationException(
                InputConfigurationReason.MissingAssociatedValue, "custom_command");
        // The macOS client requires a Unix-absolute path (hasPrefix "/"). On Windows the
        // equivalent semantic check is Path.IsPathRooted, which also accepts Unix paths.
        if (!Path.IsPathRooted(normalizedExecutable))
            throw new InputConfigurationException(
                InputConfigurationReason.CommandExecutableMustBeAbsolute, normalizedExecutable);
        if (timeoutMilliseconds < 1 || timeoutMilliseconds > MaxTimeoutMilliseconds)
            throw new InputConfigurationException(
                InputConfigurationReason.InvalidCommandTimeout, timeoutMilliseconds.ToString());

        Executable = normalizedExecutable;
        Arguments = arguments ?? Array.Empty<string>();
        TimeoutMilliseconds = timeoutMilliseconds;
    }

    public bool Equals(CommandSpecification? other) =>
        other is not null && Executable == other.Executable &&
        TimeoutMilliseconds == other.TimeoutMilliseconds &&
        Arguments.SequenceEqual(other.Arguments);
    public override bool Equals(object? obj) => Equals(obj as CommandSpecification);
    public override int GetHashCode() => Executable.GetHashCode();
}

/// <summary>
/// An action the host performs in response to a key gesture. A discriminated union serialized
/// with a <c>"type"</c> discriminator, mirroring the macOS <c>HostAction</c> enum.
/// </summary>
public abstract class HostAction : IEquatable<HostAction>
{
    private HostAction() { }

    public bool Equals(HostAction? other)
    {
        if (ReferenceEquals(this, other)) return true;
        if (other is null) return false;
        if (GetType() != other.GetType()) return false;
        return PayloadEquals(other);
    }
    public override bool Equals(object? obj) => Equals(obj as HostAction);
    public override int GetHashCode() => GetType().GetHashCode();

    /// <summary>Re-validates associated values. Throws if a payload is invalid.</summary>
    public virtual void Validate() { }

    protected virtual bool PayloadEquals(HostAction other) => true;

    /// <summary>Throws if the value is null/blank, returning the original value otherwise.</summary>
    protected internal static void RequireNonempty(string value, string action)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InputConfigurationException(
                InputConfigurationReason.MissingAssociatedValue, action);
    }

    public sealed class None : HostAction { }
    public sealed class VoiceInput : HostAction { }
    public sealed class SendEnter : HostAction { }
    public sealed class SystemCopy : HostAction { }
    public sealed class InterruptControlC : HostAction { }
    public sealed class WakeApplication : HostAction { }

    public sealed class PasteText : HostAction
    {
        public string Text { get; }
        public PasteText(string text) =>
            Text = text ?? throw new ArgumentNullException(nameof(text));
        protected override bool PayloadEquals(HostAction other) => Text == ((PasteText)other).Text;
        public override int GetHashCode() => HashCode.Combine(GetType(), Text);
        public override void Validate() => RequireNonempty(Text, "paste_text");
    }

    public sealed class CustomShortcut : HostAction
    {
        public KeyboardShortcut Shortcut { get; }
        public CustomShortcut(KeyboardShortcut shortcut) =>
            Shortcut = shortcut ?? throw new ArgumentNullException(nameof(shortcut));
        protected override bool PayloadEquals(HostAction other) =>
            Shortcut.Equals(((CustomShortcut)other).Shortcut);
        public override int GetHashCode() => HashCode.Combine(GetType(), Shortcut);
    }

    public sealed class CustomCommand : HostAction
    {
        public CommandSpecification Command { get; }
        public CustomCommand(CommandSpecification command) =>
            Command = command ?? throw new ArgumentNullException(nameof(command));
        protected override bool PayloadEquals(HostAction other) =>
            Command.Equals(((CustomCommand)other).Command);
        public override int GetHashCode() => HashCode.Combine(GetType(), Command);
    }

    public sealed class LaunchApplication : HostAction
    {
        public string BundleIdentifier { get; }
        public LaunchApplication(string bundleIdentifier) =>
            BundleIdentifier = bundleIdentifier ?? throw new ArgumentNullException(nameof(bundleIdentifier));
        protected override bool PayloadEquals(HostAction other) =>
            BundleIdentifier == ((LaunchApplication)other).BundleIdentifier;
        public override int GetHashCode() => HashCode.Combine(GetType(), BundleIdentifier);
        public override void Validate() => RequireNonempty(BundleIdentifier, "launch_application");
    }

    public sealed class ScreenModeAction : HostAction
    {
        public ScreenMode Mode { get; }
        public ScreenModeAction(ScreenMode mode) => Mode = mode;
        protected override bool PayloadEquals(HostAction other) =>
            Mode == ((ScreenModeAction)other).Mode;
    }

    public sealed class DashboardNextPage : HostAction { }
    public sealed class DashboardNextStocks : HostAction { }

    public sealed class PetInteraction : HostAction
    {
        public string Interaction { get; }
        public PetInteraction(string interaction) =>
            Interaction = interaction ?? throw new ArgumentNullException(nameof(interaction));
        protected override bool PayloadEquals(HostAction other) =>
            Interaction == ((PetInteraction)other).Interaction;
        public override int GetHashCode() => HashCode.Combine(GetType(), Interaction);
        public override void Validate() => RequireNonempty(Interaction, "pet_interaction");
    }
}

/// <summary>
/// The three actions bound to a single key, addressable by <see cref="KeyGesture"/>.
/// </summary>
public sealed class KeyBindings : IEquatable<KeyBindings>
{
    /// <summary>The action for a single press. Never null.</summary>
    public HostAction Single { get; set; }

    /// <summary>The action for a double press. Never null.</summary>
    public HostAction Double { get; set; }

    /// <summary>The action for a long press. Never null.</summary>
    public HostAction Long { get; set; }

    public KeyBindings(HostAction? single = null, HostAction? @double = null, HostAction? @long = null)
    {
        Single = single ?? new HostAction.None();
        Double = @double ?? new HostAction.None();
        Long = @long ?? new HostAction.None();
    }

    /// <summary>Accesses the action for a given gesture.</summary>
    public HostAction this[KeyGesture gesture]
    {
        get => gesture switch
        {
            KeyGesture.Single => Single,
            KeyGesture.Double => Double,
            KeyGesture.Long => Long,
            _ => throw new ArgumentOutOfRangeException(nameof(gesture))
        };
        set
        {
            switch (gesture)
            {
                case KeyGesture.Single: Single = value; break;
                case KeyGesture.Double: Double = value; break;
                case KeyGesture.Long: Long = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(gesture));
            }
        }
    }

    /// <summary>Validates all three bound actions.</summary>
    public void Validate()
    {
        Single.Validate();
        Double.Validate();
        Long.Validate();
    }

    public bool Equals(KeyBindings? other) =>
        other is not null && Single.Equals(other.Single) &&
        Double.Equals(other.Double) && Long.Equals(other.Long);
    public override bool Equals(object? obj) => Equals(obj as KeyBindings);
    public override int GetHashCode() => HashCode.Combine(Single, Double, Long);
}

/// <summary>
/// The full key-mapping profile: a schema version and a binding for every canonical key.
/// </summary>
public sealed class KeyMappingProfile : IEquatable<KeyMappingProfile>
{
    /// <summary>The only schema version currently supported.</summary>
    public const int CurrentSchemaVersion = 1;

    /// <summary>The schema version. Always <see cref="CurrentSchemaVersion"/>.</summary>
    public int SchemaVersion { get; }

    /// <summary>The per-key bindings. Always contains all four canonical keys.</summary>
    public Dictionary<CanonicalKey, KeyBindings> Mappings { get; }

    public KeyMappingProfile(int schemaVersion, Dictionary<CanonicalKey, KeyBindings> mappings)
    {
        if (schemaVersion != CurrentSchemaVersion)
            throw new InputConfigurationException(
                InputConfigurationReason.UnsupportedSchemaVersion, schemaVersion.ToString());
        if (mappings is null)
            throw new InputConfigurationException(
                InputConfigurationReason.IncompleteMapping, "mappings is null");

        foreach (var key in AllKeys)
        {
            if (!mappings.ContainsKey(key))
            {
                var missing = AllKeys.Where(k => !mappings.ContainsKey(k))
                    .OrderBy(k => (int)k).Select(k => k.WireKey());
                throw new InputConfigurationException(
                    InputConfigurationReason.IncompleteMapping, string.Join(",", missing));
            }
        }

        foreach (var bindings in mappings.Values)
            bindings.Validate();

        SchemaVersion = schemaVersion;
        Mappings = mappings;
    }

    private static readonly CanonicalKey[] AllKeys =
    {
        CanonicalKey.K1, CanonicalKey.K2, CanonicalKey.K3, CanonicalKey.K4
    };

    /// <summary>Re-validates the whole profile.</summary>
    public void Validate()
    {
        if (SchemaVersion != CurrentSchemaVersion)
            throw new InputConfigurationException(
                InputConfigurationReason.UnsupportedSchemaVersion, SchemaVersion.ToString());
        foreach (var key in AllKeys)
        {
            if (!Mappings.ContainsKey(key))
                throw new InputConfigurationException(
                    InputConfigurationReason.IncompleteMapping, key.WireKey());
        }
        foreach (var bindings in Mappings.Values)
            bindings.Validate();
    }

    /// <summary>The vendor-supplied default profile.</summary>
    public static KeyMappingProfile VendorDefault() => new KeyMappingProfile(
        CurrentSchemaVersion,
        new Dictionary<CanonicalKey, KeyBindings>
        {
            { CanonicalKey.K1, new KeyBindings(new HostAction.WakeApplication()) },
            { CanonicalKey.K2, new KeyBindings(new HostAction.PasteText("\u7ee7\u7eed")) },
            { CanonicalKey.K3, new KeyBindings(new HostAction.InterruptControlC()) },
            { CanonicalKey.K4, new KeyBindings(new HostAction.VoiceInput(), new HostAction.SendEnter()) },
        });

    public bool Equals(KeyMappingProfile? other)
    {
        if (other is null) return false;
        if (SchemaVersion != other.SchemaVersion) return false;
        if (Mappings.Count != other.Mappings.Count) return false;
        foreach (var kv in Mappings)
        {
            if (!other.Mappings.TryGetValue(kv.Key, out var b) || !kv.Value.Equals(b))
                return false;
        }
        return true;
    }
    public override bool Equals(object? obj) => Equals(obj as KeyMappingProfile);
    public override int GetHashCode() => SchemaVersion;
}

/// <summary>Reasons for which an input configuration is invalid.</summary>
public enum InputConfigurationReason
{
    UnknownKey,
    UnsupportedSchemaVersion,
    IncompleteMapping,
    MissingAssociatedValue,
    InvalidShortcutKey,
    CommandExecutableMustBeAbsolute,
    InvalidCommandTimeout,
    PersistenceRead,
    PersistenceWrite,
    InvalidStoredConfiguration
}

/// <summary>
/// Thrown when an input configuration is invalid or cannot be persisted.
/// </summary>
public sealed class InputConfigurationException : Exception
{
    /// <summary>The structured reason for the failure.</summary>
    public InputConfigurationReason Reason { get; }

    /// <summary>Optional detail (e.g. the offending value).</summary>
    public string? Detail { get; }

    public InputConfigurationException(InputConfigurationReason reason, string? detail = null)
        : base(detail is null ? reason.ToString() : $"{reason}: {detail}")
    {
        Reason = reason;
        Detail = detail;
    }
}

/// <summary>
/// Shared <see cref="JsonSerializerOptions"/> for the input subsystem. HostAction and
/// KeyMappingProfile are serialized with a type discriminator; this options instance
/// registers the converters required for correct (de)serialization.
/// </summary>
public static class InputJson
{
    /// <summary>The options to use when serializing or deserializing input types.</summary>
    public static JsonSerializerOptions Options { get; } = CreateOptions();

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions
        {
            WriteIndented = false,
        };
        options.Converters.Add(new HostActionJsonConverter());
        options.Converters.Add(new KeyboardShortcutJsonConverter());
        options.Converters.Add(new CommandSpecificationJsonConverter());
        options.Converters.Add(new KeyBindingsJsonConverter());
        options.Converters.Add(new KeyMappingProfileJsonConverter());
        return options;
    }

    internal static string RequireString(JsonElement root, string name)
    {
        if (!root.TryGetProperty(name, out var prop))
            throw new JsonException($"Missing required property '{name}'");
        var value = prop.ValueKind == JsonValueKind.String ? prop.GetString() : null;
        if (value is null)
            throw new JsonException($"Property '{name}' must be a non-null string");
        return value;
    }
}

internal sealed class HostActionJsonConverter : JsonConverter<HostAction>
{
    public override HostAction Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("Expected object for HostAction");

        using var doc = JsonDocument.ParseValue(ref reader);
        var root = doc.RootElement;
        if (!root.TryGetProperty("type", out var typeProp))
            throw new JsonException("Missing 'type' discriminator");
        var type = typeProp.ValueKind == JsonValueKind.String ? typeProp.GetString() : null;
        if (type is null)
            throw new JsonException("'type' discriminator must be a string");

        switch (type)
        {
            case "none": return new HostAction.None();
            case "voice_input": return new HostAction.VoiceInput();
            case "send_enter": return new HostAction.SendEnter();
            case "system_copy": return new HostAction.SystemCopy();
            case "interrupt_ctrl_c": return new HostAction.InterruptControlC();
            case "wake_application": return new HostAction.WakeApplication();
            case "paste_text":
                var text = InputJson.RequireString(root, "text");
                HostAction.RequireNonempty(text, "paste_text");
                return new HostAction.PasteText(text);
            case "custom_shortcut":
                return new HostAction.CustomShortcut(
                    Deserialize<KeyboardShortcut>(root, "shortcut", options));
            case "custom_command":
                return new HostAction.CustomCommand(
                    Deserialize<CommandSpecification>(root, "command", options));
            case "launch_application":
                var bundle = InputJson.RequireString(root, "bundle_identifier");
                HostAction.RequireNonempty(bundle, "launch_application");
                return new HostAction.LaunchApplication(bundle);
            case "screen_mode":
                var mode = ScreenModeExtensions.FromWire(InputJson.RequireString(root, "mode"));
                return new HostAction.ScreenModeAction(mode);
            case "dashboard_next_page": return new HostAction.DashboardNextPage();
            case "dashboard_next_stocks": return new HostAction.DashboardNextStocks();
            case "pet_interaction":
                var interaction = InputJson.RequireString(root, "interaction");
                HostAction.RequireNonempty(interaction, "pet_interaction");
                return new HostAction.PetInteraction(interaction);
            default:
                throw new JsonException($"Unknown HostAction type: {type}");
        }
    }

    public override void Write(Utf8JsonWriter writer, HostAction value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        switch (value)
        {
            case HostAction.None:
                writer.WriteString("type", "none");
                break;
            case HostAction.VoiceInput:
                writer.WriteString("type", "voice_input");
                break;
            case HostAction.SendEnter:
                writer.WriteString("type", "send_enter");
                break;
            case HostAction.SystemCopy:
                writer.WriteString("type", "system_copy");
                break;
            case HostAction.InterruptControlC:
                writer.WriteString("type", "interrupt_ctrl_c");
                break;
            case HostAction.WakeApplication:
                writer.WriteString("type", "wake_application");
                break;
            case HostAction.PasteText p:
                HostAction.RequireNonempty(p.Text, "paste_text");
                writer.WriteString("type", "paste_text");
                writer.WriteString("text", p.Text);
                break;
            case HostAction.CustomShortcut cs:
                writer.WriteString("type", "custom_shortcut");
                writer.WritePropertyName("shortcut");
                JsonSerializer.Serialize(writer, cs.Shortcut, options);
                break;
            case HostAction.CustomCommand cc:
                writer.WriteString("type", "custom_command");
                writer.WritePropertyName("command");
                JsonSerializer.Serialize(writer, cc.Command, options);
                break;
            case HostAction.LaunchApplication la:
                HostAction.RequireNonempty(la.BundleIdentifier, "launch_application");
                writer.WriteString("type", "launch_application");
                writer.WriteString("bundle_identifier", la.BundleIdentifier);
                break;
            case HostAction.ScreenModeAction sm:
                writer.WriteString("type", "screen_mode");
                writer.WriteString("mode", sm.Mode.WireString());
                break;
            case HostAction.DashboardNextPage:
                writer.WriteString("type", "dashboard_next_page");
                break;
            case HostAction.DashboardNextStocks:
                writer.WriteString("type", "dashboard_next_stocks");
                break;
            case HostAction.PetInteraction pi:
                HostAction.RequireNonempty(pi.Interaction, "pet_interaction");
                writer.WriteString("type", "pet_interaction");
                writer.WriteString("interaction", pi.Interaction);
                break;
            default:
                throw new JsonException($"Unknown HostAction subtype: {value.GetType().Name}");
        }
        writer.WriteEndObject();
    }

    private static T Deserialize<T>(JsonElement root, string name, JsonSerializerOptions options)
    {
        if (!root.TryGetProperty(name, out var prop))
            throw new JsonException($"Missing required property '{name}'");
        return prop.Deserialize<T>(options)
            ?? throw new JsonException($"Property '{name}' must not be null");
    }
}

internal sealed class KeyboardShortcutJsonConverter : JsonConverter<KeyboardShortcut>
{
    public override KeyboardShortcut Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("Expected object for KeyboardShortcut");

        using var doc = JsonDocument.ParseValue(ref reader);
        var root = doc.RootElement;

        var modifiers = new List<string>();
        if (!root.TryGetProperty("modifiers", out var modsProp) || modsProp.ValueKind != JsonValueKind.Array)
            throw new JsonException("Missing 'modifiers' array");
        foreach (var item in modsProp.EnumerateArray())
        {
            var s = item.ValueKind == JsonValueKind.String ? item.GetString() : null;
            if (s is null)
                throw new JsonException("modifier must be a string");
            modifiers.Add(s);
        }

        var key = InputJson.RequireString(root, "key");
        return new KeyboardShortcut(modifiers, key);
    }

    public override void Write(Utf8JsonWriter writer, KeyboardShortcut value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        writer.WritePropertyName("modifiers");
        writer.WriteStartArray();
        foreach (var m in value.Modifiers.OrderBy(x => x, StringComparer.Ordinal))
            writer.WriteStringValue(m);
        writer.WriteEndArray();
        writer.WriteString("key", value.Key);
        writer.WriteEndObject();
    }
}

internal sealed class CommandSpecificationJsonConverter : JsonConverter<CommandSpecification>
{
    public override CommandSpecification Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("Expected object for CommandSpecification");

        using var doc = JsonDocument.ParseValue(ref reader);
        var root = doc.RootElement;

        var executable = InputJson.RequireString(root, "executable");
        string[] arguments;
        if (!root.TryGetProperty("arguments", out var argsProp))
            throw new JsonException("Missing 'arguments' array");
        if (argsProp.ValueKind != JsonValueKind.Array)
            throw new JsonException("'arguments' must be an array");
        var list = new List<string>();
        foreach (var item in argsProp.EnumerateArray())
        {
            var s = item.ValueKind == JsonValueKind.String ? item.GetString() : null;
            if (s is null)
                throw new JsonException("argument must be a string");
            list.Add(s);
        }
        arguments = list.ToArray();

        if (!root.TryGetProperty("timeout_ms", out var toProp) || toProp.ValueKind != JsonValueKind.Number)
            throw new JsonException("Missing 'timeout_ms' number");
        var timeout = toProp.GetUInt32();

        return new CommandSpecification(executable, arguments, timeout);
    }

    public override void Write(Utf8JsonWriter writer, CommandSpecification value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        writer.WriteString("executable", value.Executable);
        writer.WritePropertyName("arguments");
        writer.WriteStartArray();
        foreach (var a in value.Arguments)
            writer.WriteStringValue(a);
        writer.WriteEndArray();
        writer.WriteNumber("timeout_ms", value.TimeoutMilliseconds);
        writer.WriteEndObject();
    }
}

internal sealed class KeyBindingsJsonConverter : JsonConverter<KeyBindings>
{
    public override KeyBindings Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("Expected object for KeyBindings");

        using var doc = JsonDocument.ParseValue(ref reader);
        var root = doc.RootElement;

        var single = DeserializeAction(root, "single", options);
        var @double = DeserializeAction(root, "double", options);
        var @long = DeserializeAction(root, "long", options);
        return new KeyBindings(single, @double, @long);
    }

    public override void Write(Utf8JsonWriter writer, KeyBindings value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        writer.WritePropertyName("single");
        JsonSerializer.Serialize(writer, value.Single, options);
        writer.WritePropertyName("double");
        JsonSerializer.Serialize(writer, value.Double, options);
        writer.WritePropertyName("long");
        JsonSerializer.Serialize(writer, value.Long, options);
        writer.WriteEndObject();
    }

    private static HostAction DeserializeAction(JsonElement root, string name, JsonSerializerOptions options)
    {
        if (!root.TryGetProperty(name, out var prop))
            throw new JsonException($"Missing required property '{name}'");
        return prop.Deserialize<HostAction>(options)
            ?? throw new JsonException($"Property '{name}' must not be null");
    }
}

internal sealed class KeyMappingProfileJsonConverter : JsonConverter<KeyMappingProfile>
{
    public override KeyMappingProfile Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("Expected object for KeyMappingProfile");

        using var doc = JsonDocument.ParseValue(ref reader);
        var root = doc.RootElement;

        if (!root.TryGetProperty("schema_version", out var svProp) || svProp.ValueKind != JsonValueKind.Number)
            throw new JsonException("Missing 'schema_version' number");
        var schemaVersion = svProp.GetInt32();

        if (!root.TryGetProperty("mappings", out var mapsProp) || mapsProp.ValueKind != JsonValueKind.Object)
            throw new JsonException("Missing 'mappings' object");

        var mappings = new Dictionary<CanonicalKey, KeyBindings>();
        foreach (var prop in mapsProp.EnumerateObject())
        {
            var key = CanonicalKeyExtensions.ParseKey(prop.Name);
            var bindings = prop.Value.Deserialize<KeyBindings>(options)
                ?? throw new JsonException($"bindings for '{prop.Name}' must not be null");
            mappings[key] = bindings;
        }

        return new KeyMappingProfile(schemaVersion, mappings);
    }

    public override void Write(Utf8JsonWriter writer, KeyMappingProfile value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        writer.WriteNumber("schema_version", value.SchemaVersion);
        writer.WritePropertyName("mappings");
        writer.WriteStartObject();
        foreach (var kv in value.Mappings.OrderBy(k => (int)k.Key))
        {
            writer.WritePropertyName(kv.Key.WireKey());
            JsonSerializer.Serialize(writer, kv.Value, options);
        }
        writer.WriteEndObject();
        writer.WriteEndObject();
    }
}
