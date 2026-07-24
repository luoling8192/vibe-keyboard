using System;
using System.Collections.Generic;
using System.Text;

namespace VibeBoardKit.Audio;

/// <summary>
/// Ogg page header type flags (analogous to Swift's <c>OggPageHeaderType</c> OptionSet).
/// </summary>
[Flags]
public enum OggPageHeaderType : byte
{
    None = 0,
    Continuation = 0x01,
    BeginningOfStream = 0x02,
    EndOfStream = 0x04,
}

/// <summary>
/// Error categories raised by <see cref="OggOpusMuxer"/>/<see cref="OggPageEncoder"/>,
/// mirroring the Swift <c>OggOpusError</c> cases.
/// </summary>
public enum OggOpusErrorKind
{
    EmptyPacket,
    PacketTooLarge,
    AlreadyFinished,
    SequenceOverflow,
    GranuleOverflow,
    OutputFailure,
}

/// <summary>Raised by the Ogg/Opus muxer and page encoder.</summary>
public sealed class OggOpusException : Exception
{
    public OggOpusErrorKind Kind { get; }
    public int PacketSize { get; }
    public string? OutputDetail { get; }

    public OggOpusException(OggOpusErrorKind kind, string message, int packetSize = 0, string? outputDetail = null)
        : base(message)
    {
        Kind = kind;
        PacketSize = packetSize;
        OutputDetail = outputDetail;
    }
}

/// <summary>
/// Encodes Opus packets into a single Ogg bitstream. Ports the Swift
/// <c>OggOpusMuxer</c>: a fixed stream serial, a granule increment of 2880 per
/// packet, idempotent header emission, and an explicit finish/commit/cancel cycle.
/// </summary>
public sealed class OggOpusMuxer
{
    /// <summary>The Ogg bitstream serial number for VibeBoard streams.</summary>
    public const uint StreamSerial = 0x5653544b;

    /// <summary>Granule position advance per Opus packet (120ms at 24kHz).</summary>
    public const ulong GranuleIncrement = 2880;

    private readonly IOggPageSink _sink;
    private uint _pageSequence;
    private ulong _granulePosition;
    private bool _wroteHeaders;
    private bool _finished;

    public OggOpusMuxer(IOggPageSink sink)
    {
        _sink = sink ?? throw new ArgumentNullException(nameof(sink));
    }

    /// <summary>
    /// Appends an Opus packet as its own page, advancing the granule position by
    /// <see cref="GranuleIncrement"/>. When <paramref name="isLast"/> is set the
    /// page carries the end-of-stream flag and finalises the muxer.
    /// </summary>
    public void Append(byte[] opusPacket, bool isLast = false)
    {
        if (_finished)
            throw new OggOpusException(OggOpusErrorKind.AlreadyFinished, "Muxer is already finished.");
        if (opusPacket.Length == 0)
            throw new OggOpusException(OggOpusErrorKind.EmptyPacket, "Cannot append an empty Opus packet.");

        WriteHeadersIfNeeded();

        ulong nextGranule;
        try
        {
            nextGranule = checked(_granulePosition + GranuleIncrement);
        }
        catch (OverflowException)
        {
            throw new OggOpusException(OggOpusErrorKind.GranuleOverflow, "Granule position overflow.");
        }
        _granulePosition = nextGranule;

        var headerType = isLast ? OggPageHeaderType.EndOfStream : OggPageHeaderType.None;
        WritePage(opusPacket, _granulePosition, headerType);
        if (isLast)
            _finished = true;
    }

    /// <summary>Writes the headers (if needed) and an empty end-of-stream page.</summary>
    public void Finish()
    {
        if (_finished)
            throw new OggOpusException(OggOpusErrorKind.AlreadyFinished, "Muxer is already finished.");

        WriteHeadersIfNeeded();
        WritePage(Array.Empty<byte>(), _granulePosition, OggPageHeaderType.EndOfStream);
        _finished = true;
    }

    /// <summary>Finishes (if needed) then commits the underlying sink.</summary>
    public void Commit()
    {
        if (!_finished)
            Finish();

        try
        {
            _sink.Commit();
        }
        catch (Exception ex)
        {
            throw new OggOpusException(OggOpusErrorKind.OutputFailure, "Failed to commit Ogg stream.", outputDetail: ex.ToString());
        }
    }

    /// <summary>Cancels the underlying sink and marks the muxer finished.</summary>
    public void Cancel()
    {
        _sink.Cancel();
        _finished = true;
    }

    private void WriteHeadersIfNeeded()
    {
        if (_wroteHeaders)
            return;

        WritePage(OpusHead(), 0, OggPageHeaderType.BeginningOfStream);
        WritePage(OpusTags(), 0, OggPageHeaderType.None);
        _wroteHeaders = true;
    }

    private void WritePage(byte[] packet, ulong granule, OggPageHeaderType headerType)
    {
        byte[] page = OggPageEncoder.Encode(packet, granule, StreamSerial, _pageSequence, headerType);

        try
        {
            _sink.Write(page);
        }
        catch (Exception ex)
        {
            throw new OggOpusException(OggOpusErrorKind.OutputFailure, "Failed to write Ogg page.", outputDetail: ex.ToString());
        }

        uint nextSequence;
        try
        {
            nextSequence = checked(_pageSequence + 1);
        }
        catch (OverflowException)
        {
            throw new OggOpusException(OggOpusErrorKind.SequenceOverflow, "Page sequence overflow.");
        }
        _pageSequence = nextSequence;
    }

    // "OpusHead" + [1, 1] + preskip(312 LE16) + originalInputRate(16000 LE32) + gain(0 LE16) + channelMapping(0)
    private static byte[] OpusHead()
    {
        var data = new List<byte>();
        OggPageEncoder.AppendAscii(data, "OpusHead");
        data.Add(1); // version
        data.Add(1); // channel count
        OggPageEncoder.AppendLittleEndian(data, (ushort)312);  // preskip
        OggPageEncoder.AppendLittleEndian(data, (uint)16000);  // original input sample rate
        OggPageEncoder.AppendLittleEndian(data, (ushort)0);    // output gain
        data.Add(0); // channel mapping family
        return data.ToArray();
    }

    // "OpusTags" + vendorLen(LE32) + "VibeBoard" + commentCount(0 LE32)
    private static byte[] OpusTags()
    {
        byte[] vendor = Encoding.UTF8.GetBytes("VibeBoard");
        var data = new List<byte>();
        OggPageEncoder.AppendAscii(data, "OpusTags");
        OggPageEncoder.AppendLittleEndian(data, (uint)vendor.Length);
        data.AddRange(vendor);
        OggPageEncoder.AppendLittleEndian(data, (uint)0); // comment count
        return data.ToArray();
    }
}

/// <summary>
/// Encodes a single Ogg page: capture pattern, header, segment lacing table, body
/// and a CRC32 checksum (polynomial 0x04c11db7, MSB-first). Ports Swift's
/// <c>OggPageEncoder</c>.
/// </summary>
public static class OggPageEncoder
{
    /// <summary>
    /// A complete packet whose length is divisible by 255 needs a zero lacing
    /// terminator; this is the largest packet that fits in a single 255-segment page.
    /// </summary>
    public const int MaximumCompletePacketBytes = (254 * 255) + 254;

    public static byte[] Encode(byte[] packet, ulong granulePosition, uint streamSerial, uint pageSequence, OggPageHeaderType headerType)
    {
        if (packet.Length > MaximumCompletePacketBytes)
        {
            throw new OggOpusException(
                OggOpusErrorKind.PacketTooLarge,
                $"Opus packet too large: actual={packet.Length}, maximum={MaximumCompletePacketBytes}.",
                packetSize: packet.Length);
        }

        byte[] lacing = MakeLacing(packet.Length);
        int pageSize = 27 + lacing.Length + packet.Length;
        var page = new byte[pageSize];

        // Capture pattern "OggS".
        page[0] = 0x4f;
        page[1] = 0x67;
        page[2] = 0x67;
        page[3] = 0x53;
        // Stream structure version.
        page[4] = 0;
        // Header type flag.
        page[5] = (byte)headerType;
        // Granule position (LE64).
        WriteLE64(page, 6, granulePosition);
        // Bitstream serial number (LE32).
        WriteLE32(page, 14, streamSerial);
        // Page sequence number (LE32).
        WriteLE32(page, 18, pageSequence);
        // CRC checksum placeholder (LE32, zero while computing).
        WriteLE32(page, 22, 0);
        // Number of page segments.
        page[26] = (byte)lacing.Length;
        // Segment table.
        Array.Copy(lacing, 0, page, 27, lacing.Length);
        // Page body.
        Array.Copy(packet, 0, page, 27 + lacing.Length, packet.Length);

        uint checksum = Crc32(page);
        WriteLE32(page, 22, checksum);
        return page;
    }

    private static byte[] MakeLacing(int packetByteCount)
    {
        int remaining = packetByteCount;
        var values = new List<byte>();
        while (remaining >= 255)
        {
            values.Add(255);
            remaining -= 255;
        }
        values.Add((byte)remaining);
        return values.ToArray();
    }

    // Ogg CRC32: polynomial 0x04c11db7, MSB-first, initial value 0.
    private static uint Crc32(byte[] data)
    {
        uint crc = 0;
        for (int i = 0; i < data.Length; i++)
        {
            crc ^= (uint)data[i] << 24;
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc & 0x80000000) != 0
                    ? (crc << 1) ^ 0x04c11db7u
                    : crc << 1;
            }
        }
        return crc;
    }

    private static void WriteLE32(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)value;
        buffer[offset + 1] = (byte)(value >> 8);
        buffer[offset + 2] = (byte)(value >> 16);
        buffer[offset + 3] = (byte)(value >> 24);
    }

    private static void WriteLE64(byte[] buffer, int offset, ulong value)
    {
        for (int k = 0; k < 8; k++)
            buffer[offset + k] = (byte)(value >> (k * 8));
    }

    // Little-endian appenders shared with the muxer for header packet construction.
    internal static void AppendLittleEndian(List<byte> buffer, ushort value)
    {
        buffer.Add((byte)value);
        buffer.Add((byte)(value >> 8));
    }

    internal static void AppendLittleEndian(List<byte> buffer, uint value)
    {
        buffer.Add((byte)value);
        buffer.Add((byte)(value >> 8));
        buffer.Add((byte)(value >> 16));
        buffer.Add((byte)(value >> 24));
    }

    internal static void AppendAscii(List<byte> buffer, string value)
    {
        buffer.AddRange(Encoding.UTF8.GetBytes(value));
    }
}
