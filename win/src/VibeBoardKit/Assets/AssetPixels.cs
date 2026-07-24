using System;
using System.Collections.Generic;

namespace VibeBoardKit.Assets;

/// <summary>
/// RGB888 pixel.
/// </summary>
public readonly struct AssetRGB888
{
    public byte Red { get; }
    public byte Green { get; }
    public byte Blue { get; }
    public AssetRGB888(byte r, byte g, byte b) { Red = r; Green = g; Blue = b; }
}

/// <summary>
/// RGBA8 pixel.
/// </summary>
public readonly struct AssetRGBA8
{
    public byte Red { get; }
    public byte Green { get; }
    public byte Blue { get; }
    public byte Alpha { get; }
    public AssetRGBA8(byte r, byte g, byte b, byte a)
    { Red = r; Green = g; Blue = b; Alpha = a; }
}

/// <summary>
/// A raster of RGBA8 pixels.
/// </summary>
public sealed class AssetRaster
{
    public int Width { get; }
    public int Height { get; }
    public AssetRGBA8[] Pixels { get; }

    public AssetRaster(int width, int height, AssetRGBA8[] pixels)
    {
        Width = width;
        Height = height;
        Pixels = pixels;
    }
}

/// <summary>
/// Asset conversion limits for source images.
/// </summary>
public sealed class AssetConversionLimits
{
    public int MaxSourceWidth { get; } = 8192;
    public int MaxSourceHeight { get; } = 8192;
    public int MaxSourcePixels { get; } = 16 * 1024 * 1024;
    public int MaxFrames { get; } = 256;
    public long MaxDecodedBytes { get; } = 128 * 1024 * 1024;
}

/// <summary>
/// EXIF orientation values.
/// </summary>
public enum AssetEXIFOrientation : byte
{
    Up = 1,
    UpMirrored = 2,
    Down = 3,
    DownMirrored = 4,
    LeftMirrored = 5,
    Left = 6,
    RightMirrored = 7,
    Right = 8
}

/// <summary>
/// Pixel conversion utilities for asset processing.
/// </summary>
public static class AssetPixelConverter
{
    /// <summary>
    /// Convert an RGBA raster to RGB565 uint16 pixels.
    /// </summary>
    public static ushort[] Convert(AssetRaster raster, int targetWidth, int targetHeight,
        string fit, AssetRGB888 background)
    {
        if (targetWidth < 1 || targetWidth > 428 || targetHeight < 1 || targetHeight > 142)
            throw new ArgumentException("Invalid target dimensions");

        // Composite RGBA over background
        var composited = CompositeOverBackground(raster, background);

        // Scale to target dimensions
        var scaled = fit switch
        {
            "contain" => ScaleContain(composited, raster.Width, raster.Height, targetWidth, targetHeight),
            "cover" => ScaleCover(composited, raster.Width, raster.Height, targetWidth, targetHeight),
            "stretch" => ScaleStretch(composited, raster.Width, raster.Height, targetWidth, targetHeight),
            "center" => ScaleCenter(composited, raster.Width, raster.Height, targetWidth, targetHeight),
            _ => ScaleContain(composited, raster.Width, raster.Height, targetWidth, targetHeight)
        };

        // Convert to RGB565
        var result = new ushort[targetWidth * targetHeight];
        for (int i = 0; i < scaled.Length; i++)
        {
            var p = scaled[i];
            result[i] = RGB565(p.Red, p.Green, p.Blue);
        }
        return result;
    }

    public static ushort RGB565(byte r, byte g, byte b)
    {
        return (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    /// <summary>
    /// Apply EXIF orientation to a raster.
    /// </summary>
    public static AssetRaster ApplyOrientation(AssetRaster raster, AssetEXIFOrientation orientation)
    {
        if (orientation == AssetEXIFOrientation.Up)
            return raster;

        int w = raster.Width;
        int h = raster.Height;
        var src = raster.Pixels;

        AssetRGBA8[] dst;
        int newW, newH;

        switch (orientation)
        {
            case AssetEXIFOrientation.UpMirrored:
                dst = new AssetRGBA8[src.Length];
                newW = w; newH = h;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        dst[y * w + (w - 1 - x)] = src[y * w + x];
                break;

            case AssetEXIFOrientation.Down:
                dst = new AssetRGBA8[src.Length];
                newW = w; newH = h;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        dst[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
                break;

            case AssetEXIFOrientation.DownMirrored:
                dst = new AssetRGBA8[src.Length];
                newW = w; newH = h;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        dst[(h - 1 - y) * w + x] = src[y * w + x];
                break;

            case AssetEXIFOrientation.Left:
            case AssetEXIFOrientation.RightMirrored:
                dst = new AssetRGBA8[src.Length];
                newW = h; newH = w;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        int nx = orientation == AssetEXIFOrientation.Left ? (h - 1 - y) : y;
                        int ny = orientation == AssetEXIFOrientation.Left ? x : (w - 1 - x);
                        dst[ny * newW + nx] = src[y * w + x];
                    }
                break;

            case AssetEXIFOrientation.Right:
            case AssetEXIFOrientation.LeftMirrored:
                dst = new AssetRGBA8[src.Length];
                newW = h; newH = w;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        int nx = orientation == AssetEXIFOrientation.Right ? y : (h - 1 - y);
                        int ny = orientation == AssetEXIFOrientation.Right ? (w - 1 - x) : x;
                        dst[ny * newW + nx] = src[y * w + x];
                    }
                break;

            default:
                return raster;
        }

        return new AssetRaster(newW, newH, dst);
    }

    private static AssetRGB888[] CompositeOverBackground(AssetRaster raster, AssetRGB888 background)
    {
        var result = new AssetRGB888[raster.Pixels.Length];
        for (int i = 0; i < raster.Pixels.Length; i++)
        {
            var p = raster.Pixels[i];
            if (p.Alpha == 255)
            {
                result[i] = new AssetRGB888(p.Red, p.Green, p.Blue);
            }
            else if (p.Alpha == 0)
            {
                result[i] = background;
            }
            else
            {
                float a = p.Alpha / 255f;
                byte r = (byte)(p.Red * a + background.Red * (1 - a));
                byte g = (byte)(p.Green * a + background.Green * (1 - a));
                byte b = (byte)(p.Blue * a + background.Blue * (1 - a));
                result[i] = new AssetRGB888(r, g, b);
            }
        }
        return result;
    }

    private static AssetRGB888[] ScaleContain(AssetRGB888[] src, int srcW, int srcH, int dstW, int dstH)
    {
        var result = new AssetRGB888[dstW * dstH];
        // Fill with black (background already composited)
        for (int i = 0; i < result.Length; i++)
            result[i] = new AssetRGB888(0, 0, 0);

        float scale = Math.Min((float)dstW / srcW, (float)dstH / srcH);
        int scaledW = (int)(srcW * scale);
        int scaledH = (int)(srcH * scale);
        int offsetX = (dstW - scaledW) / 2;
        int offsetY = (dstH - scaledH) / 2;

        BilinearScale(src, srcW, srcH, result, dstW, dstH, scaledW, scaledH, offsetX, offsetY);
        return result;
    }

    private static AssetRGB888[] ScaleCover(AssetRGB888[] src, int srcW, int srcH, int dstW, int dstH)
    {
        var result = new AssetRGB888[dstW * dstH];
        float scale = Math.Max((float)dstW / srcW, (float)dstH / srcH);
        int scaledW = (int)(srcW * scale);
        int scaledH = (int)(srcH * scale);
        int offsetX = (dstW - scaledW) / 2;
        int offsetY = (dstH - scaledH) / 2;

        BilinearScale(src, srcW, srcH, result, dstW, dstH, scaledW, scaledH, offsetX, offsetY);
        return result;
    }

    private static AssetRGB888[] ScaleStretch(AssetRGB888[] src, int srcW, int srcH, int dstW, int dstH)
    {
        var result = new AssetRGB888[dstW * dstH];
        BilinearScale(src, srcW, srcH, result, dstW, dstH, dstW, dstH, 0, 0);
        return result;
    }

    private static AssetRGB888[] ScaleCenter(AssetRGB888[] src, int srcW, int srcH, int dstW, int dstH)
    {
        var result = new AssetRGB888[dstW * dstH];
        for (int i = 0; i < result.Length; i++)
            result[i] = new AssetRGB888(0, 0, 0);

        int offsetX = (dstW - srcW) / 2;
        int offsetY = (dstH - srcH) / 2;

        for (int y = 0; y < srcH; y++)
        {
            int dy = y + offsetY;
            if (dy < 0 || dy >= dstH) continue;
            for (int x = 0; x < srcW; x++)
            {
                int dx = x + offsetX;
                if (dx < 0 || dx >= dstW) continue;
                result[dy * dstW + dx] = src[y * srcW + x];
            }
        }
        return result;
    }

    private static void BilinearScale(AssetRGB888[] src, int srcW, int srcH,
        AssetRGB888[] dst, int dstW, int dstH,
        int scaledW, int scaledH, int offsetX, int offsetY)
    {
        if (scaledW <= 0 || scaledH <= 0) return;

        float xRatio = (float)srcW / scaledW;
        float yRatio = (float)srcH / scaledH;

        for (int y = 0; y < scaledH; y++)
        {
            int dy = y + offsetY;
            if (dy < 0 || dy >= dstH) continue;

            float sy = y * yRatio;
            int sy0 = (int)sy;
            int sy1 = Math.Min(sy0 + 1, srcH - 1);
            float fy = sy - sy0;

            for (int x = 0; x < scaledW; x++)
            {
                int dx = x + offsetX;
                if (dx < 0 || dx >= dstW) continue;

                float sx = x * xRatio;
                int sx0 = (int)sx;
                int sx1 = Math.Min(sx0 + 1, srcW - 1);
                float fx = sx - sx0;

                var p00 = src[sy0 * srcW + sx0];
                var p01 = src[sy0 * srcW + sx1];
                var p10 = src[sy1 * srcW + sx0];
                var p11 = src[sy1 * srcW + sx1];

                byte r = Blerp(p00.Red, p01.Red, p10.Red, p11.Red, fx, fy);
                byte g = Blerp(p00.Green, p01.Green, p10.Green, p11.Green, fx, fy);
                byte b = Blerp(p00.Blue, p01.Blue, p10.Blue, p11.Blue, fx, fy);

                dst[dy * dstW + dx] = new AssetRGB888(r, g, b);
            }
        }
    }

    private static byte Blerp(byte v00, byte v01, byte v10, byte v11, float fx, float fy)
    {
        float top = v00 * (1 - fx) + v01 * fx;
        float bottom = v10 * (1 - fx) + v11 * fx;
        return (byte)(top * (1 - fy) + bottom * fy);
    }
}
