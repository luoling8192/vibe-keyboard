using System;
using System.IO;

namespace VibeBoardKit.Audio;

/// <summary>
/// Receives encoded Ogg page bytes. A sink writes pages, then either
/// commits (atomically persisting the accumulated data) or cancels.
/// </summary>
public interface IOggPageSink
{
    /// <summary>Writes a single encoded Ogg page.</summary>
    void Write(byte[] data);

    /// <summary>Commits the accumulated pages (e.g. an atomic file replacement).</summary>
    void Commit();

    /// <summary>Abandons the accumulated pages and releases any backing resource.</summary>
    void Cancel();
}

/// <summary>
/// Error categories raised by <see cref="IOggPageSink"/> implementations,
/// mirroring the Swift <c>OggSinkError</c> cases.
/// </summary>
public enum OggSinkErrorKind
{
    CreateFailed,
    WriteFailed,
    SyncFailed,
    CloseFailed,
    RenameFailed,
    AlreadyClosed,
}

/// <summary>
/// Raised by Ogg page sinks. Carries the error category and, where relevant,
/// the operating-system error code and the affected path.
/// </summary>
public sealed class OggSinkException : Exception
{
    public OggSinkErrorKind Kind { get; }
    public int ErrorCode { get; }
    public string? Path { get; }

    public OggSinkException(OggSinkErrorKind kind, string message, int errorCode = 0, string? path = null, Exception? innerException = null)
        : base(message, innerException)
    {
        Kind = kind;
        ErrorCode = errorCode;
        Path = path;
    }
}

/// <summary>
/// In-memory <see cref="IOggPageSink"/> backed by a <see cref="MemoryStream"/>.
/// Used for tests and for capturing encoded pages without touching the disk.
/// </summary>
public sealed class DataOggPageSink : IOggPageSink
{
    private readonly MemoryStream _stream = new();
    private bool _committed;
    private bool _cancelled;

    /// <summary>The accumulated page bytes.</summary>
    public byte[] Data => _stream.ToArray();

    /// <summary>Whether <see cref="Commit"/> has been called.</summary>
    public bool IsCommitted => _committed;

    /// <summary>Whether <see cref="Cancel"/> has been called.</summary>
    public bool IsCancelled => _cancelled;

    public DataOggPageSink() { }

    public void Write(byte[] data)
    {
        if (_committed || _cancelled)
            throw new OggSinkException(OggSinkErrorKind.AlreadyClosed, "Ogg page sink is already closed.");
        _stream.Write(data, 0, data.Length);
    }

    public void Commit()
    {
        if (_committed || _cancelled)
            throw new OggSinkException(OggSinkErrorKind.AlreadyClosed, "Ogg page sink is already closed.");
        _committed = true;
    }

    public void Cancel()
    {
        _cancelled = true;
        _stream.SetLength(0);
    }
}

/// <summary>
/// <see cref="IOggPageSink"/> that writes to a private <c>.tmp</c> file and, on
/// commit, atomically replaces the destination via <see cref="File.Move"/>.
/// On any failure the temporary file is deleted and the destination is left intact.
/// </summary>
public sealed class AtomicFileOggPageSink : IOggPageSink
{
    /// <summary>The final destination path supplied to the constructor.</summary>
    public string DestinationPath { get; }

    /// <summary>The private temporary file used until <see cref="Commit"/>.</summary>
    public string TemporaryPath { get; }

    private FileStream? _stream;
    private bool _closed;

    /// <summary>Creates a sink writing atomically to <paramref name="destinationPath"/>.</summary>
    public AtomicFileOggPageSink(string destinationPath)
    {
        DestinationPath = destinationPath;

        string directory = Path.GetDirectoryName(destinationPath) ?? string.Empty;
        string name = "." + Path.GetFileName(destinationPath) + "." + Guid.NewGuid().ToString("N") + ".tmp";
        TemporaryPath = Path.Combine(directory, name);

        try
        {
            // CreateNew gives O_CREAT | O_EXCL semantics: fail if the temp file already exists.
            _stream = new FileStream(TemporaryPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        }
        catch (Exception ex)
        {
            TryRemove(TemporaryPath);
            throw new OggSinkException(OggSinkErrorKind.CreateFailed, $"Failed to create Ogg temporary file: {TemporaryPath}", ex.HResult, TemporaryPath, ex);
        }
    }

    /// <summary>Best-effort cleanup of an abandoned temporary file, mirroring Swift's <c>deinit</c>.</summary>
    ~AtomicFileOggPageSink()
    {
        if (!_closed)
            TryRemove(TemporaryPath);
    }

    public void Write(byte[] data)
    {
        if (_closed)
            throw new OggSinkException(OggSinkErrorKind.AlreadyClosed, "Ogg page sink is already closed.");
        try
        {
            _stream!.Write(data, 0, data.Length);
        }
        catch (Exception ex)
        {
            CloseAndRemove();
            throw new OggSinkException(OggSinkErrorKind.WriteFailed, "Failed to write Ogg page.", ex.HResult, TemporaryPath, ex);
        }
    }

    public void Commit()
    {
        if (_closed)
            throw new OggSinkException(OggSinkErrorKind.AlreadyClosed, "Ogg page sink is already closed.");

        // fsync equivalent: flush the .NET buffer and the OS file buffer.
        try
        {
            _stream!.Flush(flushToDisk: true);
        }
        catch (Exception ex)
        {
            CloseAndRemove();
            throw new OggSinkException(OggSinkErrorKind.SyncFailed, "Failed to flush Ogg temporary file.", ex.HResult, TemporaryPath, ex);
        }

        try
        {
            _stream!.Dispose();
        }
        catch (Exception ex)
        {
            _stream = null;
            _closed = true;
            TryRemove(TemporaryPath);
            throw new OggSinkException(OggSinkErrorKind.CloseFailed, "Failed to close Ogg temporary file.", ex.HResult, TemporaryPath, ex);
        }

        _stream = null;
        _closed = true;

        try
        {
            File.Move(TemporaryPath, DestinationPath, overwrite: true);
        }
        catch (Exception ex)
        {
            TryRemove(TemporaryPath);
            throw new OggSinkException(OggSinkErrorKind.RenameFailed, $"Failed to move Ogg temporary file to destination: {DestinationPath}", ex.HResult, DestinationPath, ex);
        }
    }

    public void Cancel()
    {
        if (_closed)
            return;
        CloseAndRemove();
    }

    private void CloseAndRemove()
    {
        if (_stream is not null)
        {
            try { _stream.Dispose(); }
            catch { /* best effort */ }
            _stream = null;
        }
        _closed = true;
        TryRemove(TemporaryPath);
    }

    private static void TryRemove(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch { /* best effort */ }
    }
}
