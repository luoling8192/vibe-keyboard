using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;

namespace VibeBoardKit.Protocol;

/// <summary>
/// A bounded JSON parser that enforces strict limits on depth, token count,
/// string length, and number formatting. This is a security-critical component
/// that must match the device's parser expectations exactly.
///
/// It only accepts canonical unsigned integers (no leading zeros, no +, no
/// decimal points, no exponents) as numbers. Strings must be valid UTF-8.
/// </summary>
public static class BoundedJSON
{
    public const int MaximumBytes = 4092;
    public const int MaximumDepth = 12;
    public const int MaximumTokens = 1024;
    public const int MaximumStringBytes = 512;

    /// <summary>
    /// Parse a JSON object from bytes, enforcing all bounds.
    /// Returns a dictionary with string keys and object values.
    /// </summary>
    public static Dictionary<string, object?> Object(byte[] data)
    {
        var parser = new BoundedJSONParser(data, MaximumDepth, MaximumTokens, MaximumStringBytes);
        var value = parser.Parse();
        if (value is not Dictionary<string, object?> dict)
            throw new ProtocolException("Invalid envelope: expected object");
        return dict;
    }

    public static void ValidateNegotiated(byte[] data, byte maxDepth, ushort maxTokens, ushort maxStringBytes)
    {
        var parser = new BoundedJSONParser(data, maxDepth, maxTokens, maxStringBytes);
        parser.Parse();
    }

    public static void ExactKeys(Dictionary<string, object?> obj, HashSet<string> keys, string context)
    {
        if (obj.Count != keys.Count) throw new ProtocolException($"Invalid keys: {context}");
        foreach (var key in obj.Keys)
        {
            if (!keys.Contains(key))
                throw new ProtocolException($"Invalid keys: {context}");
        }
    }

    public static Dictionary<string, object?>? Dictionary(object? value) =>
        value as Dictionary<string, object?>;

    public static object?[]? Array(object? value) =>
        value as object?[];

    public static string? String(object? value) =>
        value as string;

    public static string[]? StringArray(object? value)
    {
        if (value is not object?[] arr) return null;
        var result = new string[arr.Length];
        for (int i = 0; i < arr.Length; i++)
        {
            if (arr[i] is not string s) return null;
            result[i] = s;
        }
        return result;
    }

    public static bool? Bool(object? value)
    {
        // We store booleans as a special marker
        if (value is JsonBool b) return b.Value;
        return null;
    }

    public static ulong? UInt(object? value)
    {
        if (value is not JsonNumber n) return null;
        if (!n.IsCanonicalUnsignedInteger) return null;

        ulong result = 0;
        foreach (byte b in n.Lexeme)
        {
            byte digit = (byte)(b - 0x30);
            // Check for overflow
            if (result > (ulong.MaxValue - digit) / 10)
                return null;
            result = result * 10 + digit;
        }
        return result;
    }

    public static byte? Byte(object? value)
    {
        var v = UInt(value);
        if (v == null) return null;
        if (v > byte.MaxValue) return null;
        return (byte)v;
    }

    public static ushort? UInt16(object? value)
    {
        var v = UInt(value);
        if (v == null) return null;
        if (v > ushort.MaxValue) return null;
        return (ushort)v;
    }

    public static uint? UInt32(object? value)
    {
        var v = UInt(value);
        if (v == null) return null;
        if (v > uint.MaxValue) return null;
        return (uint)v;
    }

    public static ushort? PositiveUInt16(object? value)
    {
        var v = UInt16(value);
        if (v == null || v == 0) return null;
        return v;
    }

    public static uint? PositiveUInt32(object? value)
    {
        var v = UInt32(value);
        if (v == null || v == 0) return null;
        return v;
    }

    public static byte? PositiveByte(object? value)
    {
        var v = Byte(value);
        if (v == null || v == 0) return null;
        return v;
    }

    public static ushort? RangeUInt16(object? value, ushort min, ushort max)
    {
        var v = UInt16(value);
        if (v == null) return null;
        if (v < min || v > max) return null;
        return v;
    }

    public static byte? RangeByte(object? value, byte min, byte max)
    {
        var v = Byte(value);
        if (v == null) return null;
        if (v < min || v > max) return null;
        return v;
    }

    public static uint? RangeUInt32(object? value, uint min, uint max)
    {
        var v = UInt32(value);
        if (v == null) return null;
        if (v < min || v > max) return null;
        return v;
    }

    public static bool IsValidSHA(string value)
    {
        if (value.Length != 64) return false;
        foreach (char c in value)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    }

    public static bool IsValidIdentifier(string value)
    {
        if (value.Length < 1 || value.Length > 32) return false;
        foreach (byte b in Encoding.UTF8.GetBytes(value))
        {
            if (!((b >= 48 && b <= 57) || (b >= 65 && b <= 90) ||
                  (b >= 97 && b <= 122) || b == 45 || b == 95))
                return false;
        }
        return true;
    }
}

/// <summary>
/// Marker for JSON boolean values.
/// </summary>
public readonly struct JsonBool
{
    public bool Value { get; }
    public JsonBool(bool value) { Value = value; }
}

/// <summary>
/// A parsed JSON number, stored as a raw lexeme string.
/// </summary>
public sealed class JsonNumber
{
    public string Lexeme { get; }
    public JsonNumber(string lexeme) { Lexeme = lexeme; }

    public bool IsCanonicalUnsignedInteger
    {
        get
        {
            var bytes = Encoding.UTF8.GetBytes(Lexeme);
            if (bytes.Length == 0) return false;
            if (bytes.Length == 1 && bytes[0] == 0x30) return true; // "0"
            if (bytes[0] < 0x31 || bytes[0] > 0x39) return false; // no leading zero
            for (int i = 1; i < bytes.Length; i++)
            {
                if (bytes[i] < 0x30 || bytes[i] > 0x39) return false;
            }
            return true;
        }
    }
}

/// <summary>
/// Bounded JSON parser. Enforces strict limits and canonical number formatting.
/// </summary>
public ref struct BoundedJSONParser
{
    private readonly byte[] _bytes;
    private readonly int _depthLimit;
    private readonly int _tokenLimit;
    private readonly int _stringLimit;
    private int _index;
    private int _tokens;

    public BoundedJSONParser(byte[] data, int depthLimit, int tokenLimit, int stringLimit)
    {
        if (data.Length > BoundedJSON.MaximumBytes)
            throw new ProtocolException("Limit exceeded: bytes");
        _bytes = data;
        _depthLimit = depthLimit;
        _tokenLimit = tokenLimit;
        _stringLimit = stringLimit;
        _index = 0;
        _tokens = 0;
    }

    public object? Parse()
    {
        SkipWhitespace();
        var value = ParseValue(0);
        SkipWhitespace();
        if (_index != _bytes.Length)
            throw new ProtocolException("Invalid JSON: trailing content");
        return value;
    }

    private object? ParseValue(int depth)
    {
        if (depth > _depthLimit)
            throw new ProtocolException("Limit exceeded: depth");
        ConsumeToken();
        SkipWhitespace();
        if (_index >= _bytes.Length)
            throw new ProtocolException("Invalid JSON: unexpected end");

        byte b = _bytes[_index];
        if (b == 0x7b) return ParseObject(depth);
        if (b == 0x5b) return ParseArray(depth);
        if (b == 0x22)
        {
            string? s = ParseString();
            if (s == null) throw new ProtocolException("Invalid JSON: string");
            ValidateString(s);
            return s;
        }
        if (b == 0x74)
        {
            if (!Consume("true")) throw new ProtocolException("Invalid JSON: true");
            return new JsonBool(true);
        }
        if (b == 0x66)
        {
            if (!Consume("false")) throw new ProtocolException("Invalid JSON: false");
            return new JsonBool(false);
        }
        if (b == 0x6e)
        {
            if (!Consume("null")) throw new ProtocolException("Invalid JSON: null");
            return null;
        }

        var num = ParseNumber();
        if (num == null) throw new ProtocolException("Invalid JSON: number");
        return num;
    }

    private Dictionary<string, object?> ParseObject(int depth)
    {
        _index++; // skip {
        SkipWhitespace();
        var result = new Dictionary<string, object?>();
        if (ConsumeByte(0x7d)) return result; // empty object

        while (true)
        {
            SkipWhitespace();
            ConsumeToken(); // object key counts as a token
            string? key = ParseString();
            if (key == null) throw new ProtocolException("Invalid JSON: object key");
            ValidateString(key);
            if (result.ContainsKey(key)) throw new ProtocolException("Invalid JSON: duplicate key");
            SkipWhitespace();
            if (!ConsumeByte(0x3a)) throw new ProtocolException("Invalid JSON: expected colon");
            var value = ParseValue(CheckChildDepth(depth));
            result[key] = value;
            SkipWhitespace();
            if (ConsumeByte(0x7d)) return result;
            if (!ConsumeByte(0x2c)) throw new ProtocolException("Invalid JSON: expected comma or }");
        }
    }

    private object?[] ParseArray(int depth)
    {
        _index++; // skip [
        SkipWhitespace();
        if (ConsumeByte(0x5d)) return Array.Empty<object?>();

        var result = new List<object?>();
        while (true)
        {
            var value = ParseValue(CheckChildDepth(depth));
            result.Add(value);
            SkipWhitespace();
            if (ConsumeByte(0x5d)) return result.ToArray();
            if (!ConsumeByte(0x2c)) throw new ProtocolException("Invalid JSON: expected comma or ]");
        }
    }

    private int CheckChildDepth(int depth)
    {
        int child = depth + 1;
        if (child > _depthLimit)
            throw new ProtocolException("Limit exceeded: depth");
        return child;
    }

    private void ConsumeToken()
    {
        _tokens++;
        if (_tokens > _tokenLimit)
            throw new ProtocolException("Limit exceeded: tokens");
    }

    private void ValidateString(string value)
    {
        if (Encoding.UTF8.GetByteCount(value) > _stringLimit)
            throw new ProtocolException("Limit exceeded: string");
    }

    private string? ParseString()
    {
        int opening = _index;
        if (!ConsumeByte(0x22)) return null;
        bool escaped = false;

        while (_index < _bytes.Length)
        {
            byte b = _bytes[_index];
            if (!escaped && b == 0x22)
            {
                _index++;
                // Extract the raw quoted string including quotes
                int len = _index - opening;
                var raw = new byte[len];
                Array.Copy(_bytes, opening, raw, 0, len);
                // Use System.Text.Json to decode the string properly
                var jsonStr = Encoding.UTF8.GetString(raw);
                var reader = new Utf8JsonReader(Encoding.UTF8.GetBytes(jsonStr), isFinalBlock: true, state: default);
                reader.Read();
                if (reader.TokenType != JsonTokenType.String)
                    throw new ProtocolException("Invalid JSON: string decode failed");
                return reader.GetString();
            }
            if (!escaped && b == 0x5c)
            {
                escaped = true;
                _index++;
                continue;
            }
            escaped = false;
            _index++;
        }
        return null;
    }

    private JsonNumber? ParseNumber()
    {
        int start = _index;
        if (ConsumeByte(0x2d) && _index >= _bytes.Length) return null;

        if (ConsumeByte(0x30))
        {
            if (_index < _bytes.Length && _bytes[_index] >= 0x30 && _bytes[_index] <= 0x39)
                return null;
        }
        else
        {
            if (_index >= _bytes.Length || _bytes[_index] < 0x31 || _bytes[_index] > 0x39)
                return null;
            _index++;
            while (_index < _bytes.Length && _bytes[_index] >= 0x30 && _bytes[_index] <= 0x39)
                _index++;
        }

        if (ConsumeByte(0x2e))
        {
            int fractionStart = _index;
            while (_index < _bytes.Length && _bytes[_index] >= 0x30 && _bytes[_index] <= 0x39)
                _index++;
            if (_index <= fractionStart) return null;
        }

        if (_index < _bytes.Length && (_bytes[_index] == 0x65 || _bytes[_index] == 0x45))
        {
            _index++;
            if (_index < _bytes.Length && (_bytes[_index] == 0x2b || _bytes[_index] == 0x2d))
                _index++;
            int expStart = _index;
            while (_index < _bytes.Length && _bytes[_index] >= 0x30 && _bytes[_index] <= 0x39)
                _index++;
            if (_index <= expStart) return null;
        }

        if (_index <= start || !IsDelimiter(_index)) return null;
        var lexeme = Encoding.UTF8.GetString(_bytes, start, _index - start);
        return new JsonNumber(lexeme);
    }

    private bool IsDelimiter(int pos)
    {
        if (pos >= _bytes.Length) return true;
        byte b = _bytes[pos];
        return b == 0x20 || b == 0x09 || b == 0x0a || b == 0x0d ||
               b == 0x2c || b == 0x5d || b == 0x7d;
    }

    private bool Consume(string value)
    {
        var expected = Encoding.UTF8.GetBytes(value);
        if (_index + expected.Length > _bytes.Length) return false;
        for (int i = 0; i < expected.Length; i++)
        {
            if (_bytes[_index + i] != expected[i]) return false;
        }
        _index += expected.Length;
        return true;
    }

    private bool ConsumeByte(byte b)
    {
        if (_index >= _bytes.Length || _bytes[_index] != b) return false;
        _index++;
        return true;
    }

    private void SkipWhitespace()
    {
        while (_index < _bytes.Length)
        {
            byte b = _bytes[_index];
            if (b == 0x20 || b == 0x09 || b == 0x0a || b == 0x0d)
                _index++;
            else
                break;
        }
    }
}
