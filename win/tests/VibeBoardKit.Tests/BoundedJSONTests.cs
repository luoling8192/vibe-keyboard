using Xunit;
using VibeBoardKit.Protocol;
using System.Text;

namespace VibeBoardKit.Tests;

public class BoundedJSONTests
{
    [Fact]
    public void ParsesSimpleObject()
    {
        var json = Encoding.UTF8.GetBytes("{\"event\":\"ping\",\"count\":42}");
        var obj = BoundedJSON.Object(json);

        Assert.Equal("ping", BoundedJSON.String(obj["event"]));
        Assert.Equal((ulong)42, BoundedJSON.UInt(obj["count"]));
    }

    [Fact]
    public void RejectsLeadingZerosInNumbers()
    {
        var json = Encoding.UTF8.GetBytes("{\"value\":042}");
        Assert.Throws<ProtocolException>(() => BoundedJSON.Object(json));
    }

    [Fact]
    public void AcceptsZero()
    {
        var json = Encoding.UTF8.GetBytes("{\"value\":0}");
        var obj = BoundedJSON.Object(json);
        Assert.Equal((ulong)0, BoundedJSON.UInt(obj["value"]));
    }

    [Fact]
    public void RejectsNegativeNumbers()
    {
        var json = Encoding.UTF8.GetBytes("{\"value\":-1}");
        var obj = BoundedJSON.Object(json);
        // Negative numbers are parsed but not canonical unsigned integers
        Assert.Null(BoundedJSON.UInt(obj["value"]));
    }

    [Fact]
    public void ParsesNestedObject()
    {
        var json = Encoding.UTF8.GetBytes("{\"outer\":{\"inner\":5}}");
        var obj = BoundedJSON.Object(json);

        var inner = BoundedJSON.Dictionary(obj["outer"]);
        Assert.NotNull(inner);
        Assert.Equal((ulong)5, BoundedJSON.UInt(inner!["inner"]));
    }

    [Fact]
    public void ParsesArray()
    {
        var json = Encoding.UTF8.GetBytes("{\"items\":[1,2,3]}");
        var obj = BoundedJSON.Object(json);

        var arr = BoundedJSON.Array(obj["items"]);
        Assert.NotNull(arr);
        Assert.Equal(3, arr!.Length);
    }

    [Fact]
    public void ParsesBoolean()
    {
        var json = Encoding.UTF8.GetBytes("{\"flag\":true}");
        var obj = BoundedJSON.Object(json);
        Assert.True(BoundedJSON.Bool(obj["flag"]));
    }

    [Fact]
    public void ValidatesSHA256()
    {
        Assert.True(BoundedJSON.IsValidSHA256("a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"));
        Assert.False(BoundedJSON.IsValidSHA256("short"));
        Assert.False(BoundedJSON.IsValidSHA256("A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2")); // uppercase rejected
    }

    [Fact]
    public void ValidatesIdentifier()
    {
        Assert.True(BoundedJSON.IsValidIdentifier("font-default"));
        Assert.True(BoundedJSON.IsValidIdentifier("test_123"));
        Assert.False(BoundedJSON.IsValidIdentifier(""));
        Assert.False(BoundedJSON.IsValidIdentifier(new string('a', 33)));
    }

    [Fact]
    public void ExactKeysValidation()
    {
        var json = Encoding.UTF8.GetBytes("{\"a\":1,\"b\":2}");
        var obj = BoundedJSON.Object(json);

        BoundedJSON.ExactKeys(obj, new HashSet<string> { "a", "b" }, "test");

        Assert.Throws<ProtocolException>(() =>
            BoundedJSON.ExactKeys(obj, new HashSet<string> { "a" }, "test"));
    }

    [Fact]
    public void RejectsOversizedInput()
    {
        var large = new string('x', BoundedJSON.MaximumBytes + 1);
        Assert.Throws<ProtocolException>(() =>
            BoundedJSON.Object(Encoding.UTF8.GetBytes($"{{\"data\":\"{large}\"}}")));
    }
}
