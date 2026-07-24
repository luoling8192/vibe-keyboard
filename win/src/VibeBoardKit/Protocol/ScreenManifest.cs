using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Screen mode for the replacement protocol.
/// </summary>
public enum ScreenMode
{
    Image,
    Pet,
    Dashboard,
    Custom
}

public static class ScreenModeExtensions
{
    public static string WireString(this ScreenMode mode) => mode switch
    {
        ScreenMode.Image => "image",
        ScreenMode.Pet => "pet",
        ScreenMode.Dashboard => "dashboard",
        ScreenMode.Custom => "custom",
        _ => throw new ProtocolException($"Unknown screen mode: {mode}")
    };

    public static ScreenMode FromWire(string s) => s switch
    {
        "image" => ScreenMode.Image,
        "pet" => ScreenMode.Pet,
        "dashboard" => ScreenMode.Dashboard,
        "custom" => ScreenMode.Custom,
        _ => throw new ProtocolException($"Unknown screen mode: {s}")
    };
}

/// <summary>
/// A font reference in a screen layout.
/// </summary>
public sealed class ScreenFontReference
{
    public string ID { get; }
    public ushort Version { get; }
    public ScreenFontReference(string id, ushort version) { ID = id; Version = version; }
}

/// <summary>
/// A reference to an uploaded asset.
/// </summary>
public sealed class ScreenAssetReference
{
    public uint Bytes { get; }
    public AssetKind Kind { get; }
    public string SHA256 { get; }
    public ScreenAssetReference(uint bytes, AssetKind kind, string sha256)
    { Bytes = bytes; Kind = kind; SHA256 = sha256; }
}

/// <summary>
/// Screen layout mode for the commit payload.
/// </summary>
public enum ScreenLayoutMode
{
    Dashboard,
    Custom
}

/// <summary>
/// A screen layout to commit to the device.
/// </summary>
public sealed class ScreenLayout
{
    public enum Mode { Dashboard, Custom }

    public uint BackgroundRGB888 { get; }
    public Mode LayoutMode { get; }
    public uint Revision { get; }
    public List<ScreenObjectPlacement> Objects { get; } = new();
    public List<ScreenWidget> Widgets { get; } = new();

    public ScreenLayout(uint backgroundRGB888, Mode mode, uint revision)
    {
        BackgroundRGB888 = backgroundRGB888;
        LayoutMode = mode;
        Revision = revision;
    }
}

/// <summary>
/// Placement of a screen object at x,y coordinates.
/// </summary>
public sealed class ScreenObjectPlacement
{
    public int X { get; }
    public int Y { get; }
    public ScreenObjectNode Node { get; }
    public ScreenObjectPlacement(int x, int y, ScreenObjectNode node)
    { X = x; Y = y; Node = node; }
}

/// <summary>
/// A node in a screen layout tree.
/// </summary>
public abstract class ScreenObjectNode
{
    private ScreenObjectNode() { }

    public sealed class Base
    {
        public string ID { get; }
        public int Width { get; }
        public int Height { get; }
        public int Z { get; }
        public bool Clip { get; }
        public bool Visible { get; }
        public Base(string id, int width, int height, int z, bool clip, bool visible)
        { ID = id; Width = width; Height = height; Z = z; Clip = clip; Visible = visible; }
    }

    public sealed class StaticLabel : ScreenObjectNode
    {
        public Base Info { get; }
        public string Align { get; }
        public uint ColorRGB888 { get; }
        public ScreenFontReference Font { get; }
        public string Text { get; }
        public StaticLabel(Base info, string align, uint colorRGB888, ScreenFontReference font, string text)
        { Info = info; Align = align; ColorRGB888 = colorRGB888; Font = font; Text = text; }
    }

    public sealed class DynamicLabel : ScreenObjectNode
    {
        public Base Info { get; }
        public string Align { get; }
        public uint ColorRGB888 { get; }
        public ScreenFontReference Font { get; }
        public string WidgetID { get; }
        public DynamicLabel(Base info, string align, uint colorRGB888, ScreenFontReference font, string widgetID)
        { Info = info; Align = align; ColorRGB888 = colorRGB888; Font = font; WidgetID = widgetID; }
    }

    public sealed class StaticImage : ScreenObjectNode
    {
        public Base Info { get; }
        public uint BackgroundRGB888 { get; }
        public string Fit { get; }
        public string SHA256 { get; }
        public StaticImage(Base info, uint backgroundRGB888, string fit, string sha256)
        { Info = info; BackgroundRGB888 = backgroundRGB888; Fit = fit; SHA256 = sha256; }
    }

    public sealed class DynamicImage : ScreenObjectNode
    {
        public Base Info { get; }
        public uint BackgroundRGB888 { get; }
        public string Fit { get; }
        public string WidgetID { get; }
        public DynamicImage(Base info, uint backgroundRGB888, string fit, string widgetID)
        { Info = info; BackgroundRGB888 = backgroundRGB888; Fit = fit; WidgetID = widgetID; }
    }
}

/// <summary>
/// A widget definition in a screen layout.
/// </summary>
public abstract class ScreenWidget
{
    private ScreenWidget() { }

    public sealed class Text : ScreenWidget
    {
        public string ID { get; }
        public string Target { get; }
        public string Fallback { get; }
        public Text(string id, string target, string fallback)
        { ID = id; Target = target; Fallback = fallback; }
    }
}

/// <summary>
/// Screen commit limits for validation.
/// </summary>
public sealed class ScreenCommitLimits
{
    public ushort DisplayWidth { get; }
    public ushort DisplayHeight { get; }
    public ScreenCapability Screen { get; }
    public ScreenCommitLimits(ushort displayWidth, ushort displayHeight, ScreenCapability screen)
    { DisplayWidth = displayWidth; DisplayHeight = displayHeight; Screen = screen; }
}

/// <summary>
/// Screen commit payload types.
/// </summary>
public abstract class ScreenCommitPayload
{
    private ScreenCommitPayload() { }

    public sealed class Image : ScreenCommitPayload
    {
        public uint BackgroundRGB888 { get; }
        public string Fit { get; }
        public string SHA256 { get; }
        public Image(uint backgroundRGB888, string fit, string sha256)
        { BackgroundRGB888 = backgroundRGB888; Fit = fit; SHA256 = sha256; }
    }

    public sealed class Pet : ScreenCommitPayload
    {
        public uint BackgroundRGB888 { get; }
        public string Fit { get; }
        public string SHA256 { get; }
        public Pet(uint backgroundRGB888, string fit, string sha256)
        { BackgroundRGB888 = backgroundRGB888; Fit = fit; SHA256 = sha256; }
    }

    public sealed class Dashboard : ScreenCommitPayload
    {
        public ScreenLayout Layout { get; }
        public Dashboard(ScreenLayout layout) { Layout = layout; }
    }

    public sealed class Custom : ScreenCommitPayload
    {
        public ScreenLayout Layout { get; }
        public Custom(ScreenLayout layout) { Layout = layout; }
    }
}

/// <summary>
/// A screen commit command payload.
/// </summary>
public sealed class ScreenCommit
{
    public uint ExpectedRevision { get; }
    public uint Revision { get; }
    public List<ScreenAssetReference> Assets { get; }
    public ScreenCommitPayload Payload { get; }
    public ScreenCommitLimits Limits { get; }

    public ScreenCommit(uint expectedRevision, uint revision,
        List<ScreenAssetReference> assets, ScreenCommitPayload payload, ScreenCommitLimits limits)
    {
        ExpectedRevision = expectedRevision;
        Revision = revision;
        Assets = assets;
        Payload = payload;
        Limits = limits;
    }

    /// <summary>
    /// Produces the canonical JSON body for this screen commit.
    /// </summary>
    public string CanonicalBody()
    {
        var sb = new StringBuilder();
        sb.Append('{');
        sb.Append("\"event\":\"vk_screen_commit\"");
        sb.Append(",\"expected_revision\":").Append(ExpectedRevision);
        sb.Append(",\"revision\":").Append(Revision);

        // Assets manifest
        sb.Append(",\"assets\":[");
        for (int i = 0; i < Assets.Count; i++)
        {
            if (i > 0) sb.Append(',');
            var a = Assets[i];
            sb.Append('{');
            sb.Append("\"bytes\":").Append(a.Bytes);
            sb.Append(",\"kind\":\"").Append(a.Kind.WireString()).Append('\"');
            sb.Append(",\"sha256\":\"").Append(a.SHA256).Append('\"');
            sb.Append('}');
        }
        sb.Append(']');

        // Screen manifest payload
        sb.Append(",\"screen\":");
        AppendScreenManifest(sb, Payload);

        sb.Append('}');
        return sb.ToString();
    }

    private static void AppendScreenManifest(StringBuilder sb, ScreenCommitPayload payload)
    {
        switch (payload)
        {
            case ScreenCommitPayload.Image img:
                sb.Append('{');
                sb.Append("\"background_rgb888\":").Append(img.BackgroundRGB888);
                sb.Append(",\"fit\":\"").Append(img.Fit).Append('\"');
                sb.Append(",\"sha256\":\"").Append(img.SHA256).Append('\"');
                sb.Append('}');
                break;

            case ScreenCommitPayload.Pet pet:
                sb.Append('{');
                sb.Append("\"background_rgb888\":").Append(pet.BackgroundRGB888);
                sb.Append(",\"fit\":\"").Append(pet.Fit).Append('\"');
                sb.Append(",\"sha256\":\"").Append(pet.SHA256).Append('\"');
                sb.Append('}');
                break;

            case ScreenCommitPayload.Dashboard dash:
                AppendLayout(sb, dash.Layout);
                break;

            case ScreenCommitPayload.Custom custom:
                AppendLayout(sb, custom.Layout);
                break;
        }
    }

    private static void AppendLayout(StringBuilder sb, ScreenLayout layout)
    {
        sb.Append('{');
        sb.Append("\"background_rgb888\":").Append(layout.BackgroundRGB888);
        sb.Append(",\"mode\":\"").Append(layout.LayoutMode == ScreenLayout.Mode.Dashboard ? "dashboard" : "custom").Append('\"');
        sb.Append(",\"revision\":").Append(layout.Revision);
        sb.Append(",\"objects\":[");
        for (int i = 0; i < layout.Objects.Count; i++)
        {
            if (i > 0) sb.Append(',');
            AppendObject(sb, layout.Objects[i]);
        }
        sb.Append(']');
        sb.Append(",\"widgets\":[");
        for (int i = 0; i < layout.Widgets.Count; i++)
        {
            if (i > 0) sb.Append(',');
            AppendWidget(sb, layout.Widgets[i]);
        }
        sb.Append(']');
        sb.Append('}');
    }

    private static void AppendObject(StringBuilder sb, ScreenObjectPlacement placement)
    {
        sb.Append('{');
        sb.Append("\"x\":").Append(placement.X);
        sb.Append(",\"y\":").Append(placement.Y);
        sb.Append(",\"object\":");
        AppendNode(sb, placement.Node);
        sb.Append('}');
    }

    private static void AppendNode(StringBuilder sb, ScreenObjectNode node)
    {
        switch (node)
        {
            case ScreenObjectNode.StaticLabel label:
                AppendBase(sb, label.Info, "static_label");
                sb.Append(",\"align\":\"").Append(label.Align).Append('\"');
                sb.Append(",\"color_rgb888\":").Append(label.ColorRGB888);
                AppendFont(sb, label.Font);
                sb.Append(",\"text\":\"").Append(FrameEncoder.JsonEscape(label.Text)).Append('\"');
                sb.Append('}');
                break;

            case ScreenObjectNode.DynamicLabel label:
                AppendBase(sb, label.Info, "dynamic_label");
                sb.Append(",\"align\":\"").Append(label.Align).Append('\"');
                sb.Append(",\"color_rgb888\":").Append(label.ColorRGB888);
                AppendFont(sb, label.Font);
                sb.Append(",\"widget_id\":\"").Append(label.WidgetID).Append('\"');
                sb.Append('}');
                break;

            case ScreenObjectNode.StaticImage image:
                AppendBase(sb, image.Info, "static_image");
                sb.Append(",\"background_rgb888\":").Append(image.BackgroundRGB888);
                sb.Append(",\"fit\":\"").Append(image.Fit).Append('\"');
                sb.Append(",\"sha256\":\"").Append(image.SHA256).Append('\"');
                sb.Append('}');
                break;

            case ScreenObjectNode.DynamicImage image:
                AppendBase(sb, image.Info, "dynamic_image");
                sb.Append(",\"background_rgb888\":").Append(image.BackgroundRGB888);
                sb.Append(",\"fit\":\"").Append(image.Fit).Append('\"');
                sb.Append(",\"widget_id\":\"").Append(image.WidgetID).Append('\"');
                sb.Append('}');
                break;
        }
    }

    private static void AppendBase(StringBuilder sb, ScreenObjectNode.Base info, string type)
    {
        sb.Append('{');
        sb.Append("\"id\":\"").Append(info.ID).Append('\"');
        sb.Append(",\"width\":").Append(info.Width);
        sb.Append(",\"height\":").Append(info.Height);
        sb.Append(",\"z\":").Append(info.Z);
        sb.Append(",\"clip\":").Append(info.Clip ? "true" : "false");
        sb.Append(",\"visible\":").Append(info.Visible ? "true" : "false");
        sb.Append(",\"type\":\"").Append(type).Append('\"');
    }

    private static void AppendFont(StringBuilder sb, ScreenFontReference font)
    {
        sb.Append(",\"font\":{");
        sb.Append("\"id\":\"").Append(font.ID).Append('\"');
        sb.Append(",\"version\":").Append(font.Version);
        sb.Append('}');
    }

    private static void AppendWidget(StringBuilder sb, ScreenWidget widget)
    {
        if (widget is ScreenWidget.Text text)
        {
            sb.Append('{');
            sb.Append("\"id\":\"").Append(text.ID).Append('\"');
            sb.Append(",\"target\":\"").Append(text.Target).Append('\"');
            sb.Append(",\"fallback\":\"").Append(FrameEncoder.JsonEscape(text.Fallback)).Append('\"');
            sb.Append(",\"type\":\"text\"");
            sb.Append('}');
        }
    }
}
