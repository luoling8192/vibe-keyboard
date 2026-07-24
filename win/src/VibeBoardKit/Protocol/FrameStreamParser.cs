using System;
using System.Collections.Generic;

namespace VibeBoardKit.Protocol;

/// <summary>
/// Incremental parser for the USB binary frame stream.
///
/// The device sends frames with a 4-byte envelope:
///   offset 0: protocol version (0x01)
///   offset 1: frame type
///   offset 2: body length (UInt16 LE)
///   offset 4: body
///
/// Audio frames use a 16-byte fixed header instead:
///   offset 0-3: version + type + header_length (always 0x0010)
///   offset 4-7: session ID (UInt32 LE)
///   offset 8-11: sequence (UInt32 LE)
///   offset 12: flags
///   offset 13: reserved
///   offset 14-15: payload length (UInt16 LE)
///   offset 16: payload
/// </summary>
public sealed class FrameStreamParser
{
    public const byte ProtocolVersion = 0x01;
    public const int MaximumFrameLength = 4096;

    private readonly List<byte> _buffer = new();
    public int ReceiveBufferLimit { get; }
    public int BufferedByteCount => _buffer.Count;

    public FrameStreamParser(int receiveBufferLimit = MaximumFrameLength)
    {
        if (receiveBufferLimit < 16)
            throw new ProtocolException($"Receive buffer limit must be >= 16, got {receiveBufferLimit}");
        ReceiveBufferLimit = receiveBufferLimit;
    }

    /// <summary>
    /// Append raw bytes and return any completed frames or discarded bytes.
    /// </summary>
    public List<FrameStreamEvent> Append(byte[] data)
    {
        if (data.Length > ReceiveBufferLimit)
            throw new ProtocolException(
                $"Receive buffer limit exceeded: limit={ReceiveBufferLimit}, attempted={data.Length}");

        var events = new List<FrameStreamEvent>(Math.Min(data.Length, ReceiveBufferLimit));
        int offset = 0;

        while (offset < data.Length)
        {
            if (_buffer.Count == ReceiveBufferLimit)
            {
                Drain(events);
                if (_buffer.Count >= ReceiveBufferLimit)
                    throw new ProtocolException(
                        $"Receive buffer limit exceeded after drain: limit={ReceiveBufferLimit}");
            }

            int writable = ReceiveBufferLimit - _buffer.Count;
            int count = Math.Min(writable, data.Length - offset);
            for (int i = 0; i < count; i++)
                _buffer.Add(data[offset + i]);
            offset += count;
            Drain(events);
        }

        return events;
    }

    private void Drain(List<FrameStreamEvent> events)
    {
        while (_buffer.Count >= 4)
        {
            if (_buffer[0] != ProtocolVersion)
            {
                byte b = _buffer[0];
                _buffer.RemoveAt(0);
                events.Add(FrameStreamEvent.Discard(b, FrameDiscardReason.InvalidVersion));
                continue;
            }

            byte typeByte = _buffer[1];
            if (!Enum.IsDefined(typeof(FrameType), typeByte))
            {
                byte version = _buffer[0];
                _buffer.RemoveAt(0);
                events.Add(FrameStreamEvent.Discard(version, FrameDiscardReason.UnknownType));
                continue;
            }

            var type = (FrameType)typeByte;
            int totalLength;

            if (type == FrameType.Audio)
            {
                if (_buffer.Count < 16) break;
                totalLength = 16 + ReadUInt16LE(_buffer, 14);
            }
            else
            {
                totalLength = 4 + ReadUInt16LE(_buffer, 2);
            }

            if (totalLength > MaximumFrameLength)
            {
                byte b = _buffer[0];
                _buffer.RemoveAt(0);
                events.Add(FrameStreamEvent.Discard(b, FrameDiscardReason.FrameTooLarge));
                continue;
            }

            if (totalLength > ReceiveBufferLimit)
                throw new ProtocolException(
                    $"Receive buffer limit exceeded: limit={ReceiveBufferLimit}, attempted={totalLength}");

            if (_buffer.Count < totalLength) break;

            var frameBytes = new byte[totalLength];
            _buffer.CopyTo(0, frameBytes, 0, totalLength);
            _buffer.RemoveRange(0, totalLength);
            events.Add(FrameStreamEvent.FrameEvent(new RawFrame(type, frameBytes)));
        }
    }

    public static ushort ReadUInt16LE(IList<byte> b, int offset) =>
        (ushort)(b[offset] | (b[offset + 1] << 8));

    public static uint ReadUInt32LE(IList<byte> b, int offset) =>
        (uint)(b[offset] | (b[offset + 1] << 8) | (b[offset + 2] << 16) | (b[offset + 3] << 24));

    public static ushort ReadUInt16LE(byte[] b, int offset) =>
        (ushort)(b[offset] | (b[offset + 1] << 8));

    public static uint ReadUInt32LE(byte[] b, int offset) =>
        (uint)(b[offset] | (b[offset + 1] << 8) | (b[offset + 2] << 16) | (b[offset + 3] << 24));
}
