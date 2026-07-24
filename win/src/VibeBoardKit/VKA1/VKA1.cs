using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace VibeBoardKit.VKA1;

public class VKA1Exception : Exception
{
    public VKA1Exception(string message) : base(message) { }
}

public enum VKA1Kind : byte
{
    Image = 1,
    Animation = 2,
    GlyphBitmap = 3
}

public enum VKA1FrameEncoding : byte
{
    Raw = 0,
    RowRLE = 1
}

public sealed class VKA1Limits
{
    public ushort MaxFrames { get; }
    public ushort MinFrameDurationMS { get; }
    public ushort MaxFrameDurationMS { get; }
    public uint MaxContainerBytes { get; }
    public uint MaxDecodedBytes { get; }

    public VKA1Limits(ushort maxFrames, ushort minFrameDurationMS, ushort maxFrameDurationMS,
        uint maxContainerBytes, uint maxDecodedBytes)
    {
        MaxFrames = maxFrames;
        MinFrameDurationMS = minFrameDurationMS;
        MaxFrameDurationMS = maxFrameDurationMS;
        MaxContainerBytes = maxContainerBytes;
        MaxDecodedBytes = maxDecodedBytes;
    }
}

public sealed class VKA1SourceFrame
{
    public ushort[] Pixels { get; }
    public ushort DurationMS { get; }
    public VKA1SourceFrame(ushort[] pixels, ushort durationMS)
    { Pixels = pixels; DurationMS = durationMS; }
}

public sealed class VKA1Frame
{
    public VKA1FrameEncoding Encoding { get; }
    public ushort DurationMS { get; }
    public ushort[] Pixels { get; }
    public VKA1Frame(VKA1FrameEncoding encoding, ushort durationMS, ushort[] pixels)
    { Encoding = encoding; DurationMS = durationMS; Pixels = pixels; }
}

public sealed class VKA1Container
{
    public VKA1Kind Kind { get; }
    public ushort Width { get; }
    public ushort Height { get; }
    public List<VKA1Frame> Frames { get; }
    public string SHA256 { get; }

    public VKA1Container(VKA1Kind kind, ushort width, ushort height, List<VKA1Frame> frames, string sha256)
    { Kind = kind; Width = width; Height = height; Frames = frames; SHA256 = sha256; }
}

/// <summary>
/// Codec for the VKA1 binary asset container format.
/// This is the device-side asset format for images and animations.
/// </summary>
public static class VKA1Codec
{
    public static byte[] Encode(VKA1Kind kind, ushort width, ushort height,
        List<VKA1SourceFrame> frames, VKA1Limits limits)
    {
        int pixelCount = CheckedGeometry(width, height, limits);
        if (frames.Count == 0 || frames.Count > limits.MaxFrames || frames.Count > ushort.MaxValue)
            throw new VKA1Exception("Invalid frame count");
        ValidateKind(kind, frames, limits);

        var encoded = new List<(byte[] data, VKA1FrameEncoding encoding, ushort durationMS)>(frames.Count);
        byte encodingBits = 0;

        foreach (var frame in frames)
        {
            if (frame.Pixels.Length != pixelCount)
                throw new VKA1Exception("Invalid dimensions");
            byte[] raw = RawData(frame.Pixels);
            byte[] rle = RowRLE(frame.Pixels, width, height);
            var choice = rle.Length < raw.Length ? (rle, VKA1FrameEncoding.RowRLE) : (raw, VKA1FrameEncoding.Raw);
            encodingBits |= (byte)(choice.Item2 == VKA1FrameEncoding.Raw ? 1 : 2);
            encoded.Add((choice.Item1, choice.Item2, frame.DurationMS));
        }

        int headerBytes = 56 + frames.Count * 12;
        if (headerBytes > ushort.MaxValue)
            throw new VKA1Exception("Limit exceeded: header");

        long total = headerBytes;
        foreach (var frame in encoded)
        {
            total += frame.data.Length;
            if (total > limits.MaxContainerBytes || total > uint.MaxValue)
                throw new VKA1Exception("Limit exceeded: container");
        }

        var data = new List<byte>((int)total);
        // Magic
        data.AddRange(new byte[] { 0x56, 0x4b, 0x41, 0x31, (byte)kind, 1, encodingBits, 0 });
        AppendLE(data, width);
        AppendLE(data, height);
        AppendLE(data, (ushort)frames.Count);
        AppendLE(data, (ushort)headerBytes);
        AppendLE(data, (uint)(pixelCount * 2));
        AppendLE(data, (uint)total);
        // SHA256 placeholder (32 zeros)
        data.AddRange(new byte[32]);

        // Frame table
        int offset = headerBytes;
        foreach (var frame in encoded)
        {
            AppendLE(data, (uint)offset);
            AppendLE(data, (uint)frame.data.Length);
            AppendLE(data, frame.durationMS);
            data.Add((byte)frame.encoding);
            data.Add(0);
            offset += frame.data.Length;
        }

        // Frame data
        foreach (var frame in encoded)
            data.AddRange(frame.data);

        // Compute SHA256 with hash field zeroed
        var hashData = data.ToArray();
        var sha256 = SHA256.HashData(hashData);
        // Write hash into bytes 24..56
        Array.Copy(sha256, 0, hashData, 24, 32);

        // Self-verify
        Decode(hashData, limits);
        return hashData;
    }

    public static VKA1Container Decode(byte[] data, VKA1Limits limits)
    {
        if (data.Length < 68 || data.Length > limits.MaxContainerBytes)
            throw new VKA1Exception("Invalid header");
        if (data[0] != 0x56 || data[1] != 0x4b || data[2] != 0x41 || data[3] != 0x31)
            throw new VKA1Exception("Invalid header: magic");

        var kind = (VKA1Kind)data[4];
        if (!Enum.IsDefined(typeof(VKA1Kind), kind))
            throw new VKA1Exception("Invalid kind");
        if (data[5] != 1 || data[7] != 0 || (data[6] & ~3) != 0)
            throw new VKA1Exception("Invalid encoding");

        ushort width = Read16(data, 8);
        ushort height = Read16(data, 10);
        ushort frameCount = Read16(data, 12);
        ushort headerBytes = Read16(data, 14);
        uint decodedBytes = Read32(data, 16);
        uint totalBytes = Read32(data, 20);

        int pixelCount = CheckedGeometry(width, height, limits);
        if (frameCount == 0 || frameCount > limits.MaxFrames)
            throw new VKA1Exception("Invalid frame count");

        long aggregateDecoded = (long)decodedBytes * frameCount;
        if (aggregateDecoded > limits.MaxDecodedBytes)
            throw new VKA1Exception("Limit exceeded: decoded");

        int expectedHeader = 56 + frameCount * 12;
        if (headerBytes != expectedHeader || totalBytes != (uint)data.Length || decodedBytes != (uint)(pixelCount * 2))
            throw new VKA1Exception("Invalid header: size mismatch");

        // Verify SHA256
        byte[] expectedHash = new byte[32];
        Array.Copy(data, 24, expectedHash, 0, 32);
        var zeroed = (byte[])data.Clone();
        Array.Clear(zeroed, 24, 32);
        var actualHash = SHA256.HashData(zeroed);
        if (!actualHash.AsSpan().SequenceEqual(expectedHash))
            throw new VKA1Exception("Invalid hash");

        // Parse frame table
        var entries = new List<(int offset, int length, ushort duration, VKA1FrameEncoding encoding)>();
        int expectedOffset = expectedHeader;
        byte union = 0;

        for (int i = 0; i < frameCount; i++)
        {
            int base_ = 56 + i * 12;
            int offset = (int)Read32(data, base_);
            int length = (int)Read32(data, base_ + 4);
            ushort duration = Read16(data, base_ + 8);
            var encoding = (VKA1FrameEncoding)data[base_ + 10];
            if (!Enum.IsDefined(typeof(VKA1FrameEncoding), encoding))
                throw new VKA1Exception("Invalid encoding");
            if (data[base_ + 11] != 0)
                throw new VKA1Exception("Invalid encoding: reserved");

            if (offset != expectedOffset || length <= 0)
                throw new VKA1Exception("Invalid range");
            int end = offset + length;
            if (end > data.Length)
                throw new VKA1Exception("Invalid range: end");
            expectedOffset = end;
            union |= (byte)(encoding == VKA1FrameEncoding.Raw ? 1 : 2);
            entries.Add((offset, length, duration, encoding));
        }

        if (expectedOffset != data.Length || union != data[6])
            throw new VKA1Exception("Invalid range: union");

        var durations = new ushort[entries.Count];
        for (int i = 0; i < entries.Count; i++)
            durations[i] = entries[i].duration;
        ValidateDurations(kind, durations, limits);

        var frames = new List<VKA1Frame>(entries.Count);
        foreach (var entry in entries)
        {
            var payload = new byte[entry.length];
            Array.Copy(data, entry.offset, payload, 0, entry.length);
            ushort[] pixels;

            if (entry.encoding == VKA1FrameEncoding.Raw)
            {
                if (payload.Length != pixelCount * 2)
                    throw new VKA1Exception("Invalid range: raw size");
                pixels = new ushort[pixelCount];
                for (int i = 0; i < pixelCount; i++)
                    pixels[i] = (ushort)(payload[i * 2] | (payload[i * 2 + 1] << 8));

                // Canonical RLE must be >= raw (device chose raw, so RLE must not be smaller)
                int canonicalRLELength = RowRLE(pixels, width, height).Length;
                if (canonicalRLELength < payload.Length)
                    throw new VKA1Exception("Invalid encoding: canonical RLE smaller than raw");
            }
            else
            {
                pixels = DecodeRLE(payload, width, height);
                if (payload.Length >= pixelCount * 2)
                    throw new VKA1Exception("Invalid encoding: RLE not smaller");
            }

            frames.Add(new VKA1Frame(entry.encoding, entry.duration, pixels));
        }

        var hashStr = new StringBuilder(64);
        foreach (byte b in expectedHash)
            hashStr.Append(b.ToString("x2"));

        return new VKA1Container(kind, width, height, frames, hashStr.ToString());
    }

    private static int CheckedGeometry(ushort width, ushort height, VKA1Limits limits)
    {
        if (width < 1 || width > 428 || height < 1 || height > 142)
            throw new VKA1Exception("Invalid dimensions");
        int pixels = width * height;
        if (pixels > uint.MaxValue / 2 || (long)pixels * 2 > limits.MaxDecodedBytes)
            throw new VKA1Exception("Limit exceeded: decoded");
        return pixels;
    }

    private static void ValidateKind(VKA1Kind kind, List<VKA1SourceFrame> frames, VKA1Limits limits)
    {
        var durations = new ushort[frames.Count];
        for (int i = 0; i < frames.Count; i++)
            durations[i] = frames[i].DurationMS;
        ValidateDurations(kind, durations, limits);
    }

    private static void ValidateDurations(VKA1Kind kind, ushort[] durations, VKA1Limits limits)
    {
        switch (kind)
        {
            case VKA1Kind.Image:
            case VKA1Kind.GlyphBitmap:
                if (durations.Length != 1 || durations[0] != 0)
                    throw new VKA1Exception("Invalid duration");
                break;
            case VKA1Kind.Animation:
                foreach (var d in durations)
                {
                    if (d == 0 || d < limits.MinFrameDurationMS || d > limits.MaxFrameDurationMS)
                        throw new VKA1Exception("Invalid duration");
                }
                break;
        }
    }

    private static byte[] RawData(ushort[] pixels)
    {
        var data = new byte[pixels.Length * 2];
        for (int i = 0; i < pixels.Length; i++)
        {
            data[i * 2] = (byte)(pixels[i] & 0xFF);
            data[i * 2 + 1] = (byte)((pixels[i] >> 8) & 0xFF);
        }
        return data;
    }

    private static byte[] RowRLE(ushort[] pixels, ushort width, ushort height)
    {
        var data = new List<byte>();
        for (int row = 0; row < height; row++)
        {
            int column = 0;
            while (column < width)
            {
                ushort pixel = pixels[row * width + column];
                int end = column + 1;
                while (end < width && pixels[row * width + end] == pixel)
                    end++;
                int count = end - column;
                if (count > ushort.MaxValue)
                    throw new VKA1Exception("Limit exceeded: RLE count");
                AppendLE(data, (ushort)count);
                AppendLE(data, pixel);
                column = end;
            }
        }
        return data.ToArray();
    }

    private static ushort[] DecodeRLE(byte[] payload, ushort widthU, ushort heightU)
    {
        int width = widthU;
        int height = heightU;
        var pixels = new List<ushort>(width * height);
        int index = 0;

        for (int row = 0; row < height; row++)
        {
            int columns = 0;
            ushort? prior = null;
            while (columns < width)
            {
                if (index + 4 > payload.Length)
                    throw new VKA1Exception("Invalid RLE: truncated");
                int count = payload[index] | (payload[index + 1] << 8);
                ushort pixel = (ushort)(payload[index + 2] | (payload[index + 3] << 8));

                if (count <= 0 || count > width - columns)
                    throw new VKA1Exception("Invalid RLE: count");
                if (prior.HasValue && prior.Value == pixel)
                    throw new VKA1Exception("Invalid RLE: consecutive same pixel");

                for (int i = 0; i < count; i++)
                    pixels.Add(pixel);
                columns += count;
                prior = pixel;
                index += 4;
            }
        }

        if (index != payload.Length)
            throw new VKA1Exception("Invalid RLE: trailing data");

        return pixels.ToArray();
    }

    private static void AppendLE(List<byte> data, ushort value)
    {
        data.Add((byte)(value & 0xFF));
        data.Add((byte)((value >> 8) & 0xFF));
    }

    private static void AppendLE(List<byte> data, uint value)
    {
        data.Add((byte)(value & 0xFF));
        data.Add((byte)((value >> 8) & 0xFF));
        data.Add((byte)((value >> 16) & 0xFF));
        data.Add((byte)((value >> 24) & 0xFF));
    }

    private static ushort Read16(byte[] data, int offset)
    {
        return (ushort)(data[offset] | (data[offset + 1] << 8));
    }

    private static uint Read32(byte[] data, int offset)
    {
        return (uint)(data[offset] | (data[offset + 1] << 8) |
            (data[offset + 2] << 16) | (data[offset + 3] << 24));
    }
}
