using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Collections.Generic;
using VibeBoardKit.VKA1;

namespace VibeBoardKit.Assets;

/// <summary>
/// Decodes source images (GIF, PNG, JPEG, BMP) into rasters using System.Drawing.
/// </summary>
public static class AssetSourceDecoder
{
    /// <summary>
    /// Decode an image file into a list of frames with metadata.
    /// </summary>
    public static DecodedAsset Decode(byte[] data, ushort minimumFrameMS = 1, ushort maximumFrameMS = 2000)
    {
        using var ms = new MemoryStream(data);
        using var img = Image.FromStream(ms, useEmbeddedColorManagement: false, validateImageData: true);

        var frameDimensions = new System.Drawing.Imaging.FrameDimension(img.FrameDimensionsList[0]);
        int frameCount = img.GetFrameCount(frameDimensions);

        if (frameCount == 0)
            frameCount = 1;

        var frames = new List<DecodedAnimationFrame>(frameCount);
        var limits = new AssetConversionLimits();

        for (int i = 0; i < frameCount; i++)
        {
            img.SelectActiveFrame(frameDimensions, i);
            using var bmp = new Bitmap(img.Width, img.Height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.DrawImage(img, 0, 0, img.Width, img.Height);
            }

            var raster = BitmapToRaster(bmp);

            // Get frame duration for GIF
            ushort durationMS = 0;
            if (frameCount > 1)
            {
                var item = img.GetPropertyItem(0x5100); // PropertyTagFrameDelay
                if (item != null && i * 4 < item.Value.Length)
                {
                    int delay = item.Value[i * 4] | (item.Value[i * 4 + 1] << 8);
                    // GIF delays are in 1/100 seconds
                    durationMS = (ushort)(delay * 10);
                    if (durationMS < minimumFrameMS) durationMS = minimumFrameMS;
                    if (durationMS > maximumFrameMS) durationMS = maximumFrameMS;
                }
                else
                {
                    durationMS = Math.Max(minimumFrameMS, (ushort)100);
                }
            }

            frames.Add(new DecodedAnimationFrame(raster, durationMS));
        }

        return new DecodedAsset(frames, img.Width, img.Height);
    }

    private static AssetRaster BitmapToRaster(Bitmap bmp)
    {
        var pixels = new AssetRGBA8[bmp.Width * bmp.Height];
        var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
        var bmpData = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);

        try
        {
            int stride = bmpData.Stride;
            var row = new byte[stride];
            for (int y = 0; y < bmp.Height; y++)
            {
                System.Runtime.InteropServices.Marshal.Copy(
                    bmpData.Scan0 + y * stride, row, 0, stride);
                for (int x = 0; x < bmp.Width; x++)
                {
                    int srcIdx = x * 4;
                    // Format32bppArgb is BGRA in memory on little-endian
                    pixels[y * bmp.Width + x] = new AssetRGBA8(
                        row[srcIdx + 2], row[srcIdx + 1], row[srcIdx], row[srcIdx + 3]);
                }
            }
        }
        finally
        {
            bmp.UnlockBits(bmpData);
        }

        return new AssetRaster(bmp.Width, bmp.Height, pixels);
    }
}

/// <summary>
/// A decoded source asset with frames.
/// </summary>
public sealed class DecodedAsset
{
    public List<DecodedAnimationFrame> Frames { get; }
    public int Width { get; }
    public int Height { get; }

    public DecodedAsset(List<DecodedAnimationFrame> frames, int width, int height)
    {
        Frames = frames;
        Width = width;
        Height = height;
    }
}

public sealed class DecodedAnimationFrame
{
    public AssetRaster Raster { get; }
    public ushort DurationMS { get; }

    public DecodedAnimationFrame(AssetRaster raster, ushort durationMS)
    {
        Raster = raster;
        DurationMS = durationMS;
    }
}

/// <summary>
/// Factory for creating VKA1 containers from decoded source assets.
/// </summary>
public static class ConvertedAssetFactory
{
    public static byte[] MakeVKA1(DecodedAsset source, string fit, AssetRGB888 background,
        int targetWidth, int targetHeight, VKA1Limits limits)
    {
        var frames = new List<VKA1SourceFrame>(source.Frames.Count);

        foreach (var frame in source.Frames)
        {
            var pixels = AssetPixelConverter.Convert(
                frame.Raster, targetWidth, targetHeight, fit, background);

            frames.Add(new VKA1SourceFrame(pixels, frame.DurationMS));
        }

        var kind = source.Frames.Count > 1 ? VKA1Kind.Animation : VKA1Kind.Image;
        return VKA1Codec.Encode(kind, (ushort)targetWidth, (ushort)targetHeight, frames, limits);
    }
}
