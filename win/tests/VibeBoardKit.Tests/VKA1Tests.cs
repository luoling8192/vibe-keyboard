using Xunit;
using VibeBoardKit.VKA1;

namespace VibeBoardKit.Tests;

public class VKA1Tests
{
    private static VKA1Limits DefaultLimits() => new(
        maxFrames: 256,
        minFrameDurationMS: 1,
        maxFrameDurationMS: 2000,
        maxContainerBytes: 256 * 1024,
        maxDecodedBytes: 128 * 1024);

    [Fact]
    public void EncodesAndDecodesImage()
    {
        ushort width = 2;
        ushort height = 2;
        var pixels = new ushort[] { 0xF800, 0x07E0, 0x001F, 0x0000 }; // red, green, blue, black
        var frame = new VKA1SourceFrame(pixels, 0);
        var limits = DefaultLimits();

        var encoded = VKA1Codec.Encode(VKA1Kind.Image, width, height,
            new List<VKA1SourceFrame> { frame }, limits);

        Assert.True(encoded.Length > 0);

        var container = VKA1Codec.Decode(encoded, limits);
        Assert.Equal(VKA1Kind.Image, container.Kind);
        Assert.Equal(width, container.Width);
        Assert.Equal(height, container.Height);
        Assert.Single(container.Frames);
        Assert.Equal(pixels, container.Frames[0].Pixels);
        Assert.Equal(64, container.SHA256.Length);
    }

    [Fact]
    public void EncodesAndDecodesAnimation()
    {
        ushort width = 4;
        ushort height = 1;
        var frame1 = new VKA1SourceFrame(new ushort[] { 0xF800, 0x07E0, 0x001F, 0x0000 }, 100);
        var frame2 = new VKA1SourceFrame(new ushort[] { 0x0000, 0xF800, 0x07E0, 0x001F }, 100);
        var limits = DefaultLimits();

        var encoded = VKA1Codec.Encode(VKA1Kind.Animation, width, height,
            new List<VKA1SourceFrame> { frame1, frame2 }, limits);

        var container = VKA1Codec.Decode(encoded, limits);
        Assert.Equal(VKA1Kind.Animation, container.Kind);
        Assert.Equal(2, container.Frames.Count);
        Assert.Equal(100, container.Frames[0].DurationMS);
        Assert.Equal(100, container.Frames[1].DurationMS);
    }

    [Fact]
    public void RejectsInvalidMagic()
    {
        var data = new byte[68];
        data[0] = 0x00; // wrong magic
        Assert.Throws<VKA1Exception>(() => VKA1Codec.Decode(data, DefaultLimits()));
    }

    [Fact]
    public void RejectsInvalidDimensions()
    {
        Assert.Throws<VKA1Exception>(() =>
            VKA1Codec.Encode(VKA1Kind.Image, 500, 1, // width > 428
                new List<VKA1SourceFrame> { new(new ushort[] { 0 }, 0) },
                DefaultLimits()));
    }

    [Fact]
    public void RejectsImageWithMultipleFrames()
    {
        Assert.Throws<VKA1Exception>(() =>
            VKA1Codec.Encode(VKA1Kind.Image, 1, 1,
                new List<VKA1SourceFrame>
                {
                    new(new ushort[] { 0 }, 0),
                    new(new ushort[] { 0 }, 0)
                },
                DefaultLimits()));
    }

    [Fact]
    public void RejectsImageWithNonZeroDuration()
    {
        Assert.Throws<VKA1Exception>(() =>
            VKA1Codec.Encode(VKA1Kind.Image, 1, 1,
                new List<VKA1SourceFrame> { new(new ushort[] { 0 }, 100) },
                DefaultLimits()));
    }

    [Fact]
    public void RejectsAnimationWithZeroDuration()
    {
        Assert.Throws<VKA1Exception>(() =>
            VKA1Codec.Encode(VKA1Kind.Animation, 1, 1,
                new List<VKA1SourceFrame> { new(new ushort[] { 0 }, 0) },
                DefaultLimits()));
    }
}
