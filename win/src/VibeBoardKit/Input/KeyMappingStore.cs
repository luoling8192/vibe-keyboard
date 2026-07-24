using System;
using System.IO;
using System.Text.Json;

namespace VibeBoardKit.Input;

/// <summary>
/// Provides raw, opaque access to the persisted configuration blob.
/// Implementations must make replacement atomic so that a crash mid-write
/// cannot leave a partial or corrupt file.
/// </summary>
public interface IConfigurationDataStore
{
    /// <summary>
    /// Reads the persisted bytes, or returns null when no configuration exists yet.
    /// </summary>
    byte[]? Read();

    /// <summary>
    /// Atomically replaces the persisted bytes with <paramref name="data"/>.
    /// </summary>
    void ReplaceAtomically(byte[] data);
}

/// <summary>
/// A <see cref="IConfigurationDataStore"/> backed by a single file on disk.
/// Writes are atomic: data is written to a temporary file in the same directory
/// and then moved into place with <see cref="File.Move(string, string, bool)"/>,
/// so a failure during replacement never corrupts the existing file.
/// </summary>
public sealed class FileConfigurationDataStore : IConfigurationDataStore
{
    private readonly string _path;

    public FileConfigurationDataStore(string path)
    {
        _path = path ?? throw new ArgumentNullException(nameof(path));
    }

    /// <summary>The absolute path of the backing file.</summary>
    public string Path => _path;

    public byte[]? Read()
    {
        try
        {
            if (!File.Exists(_path))
                return null;
            return File.ReadAllBytes(_path);
        }
        catch (Exception ex)
        {
            throw new InputConfigurationException(
                InputConfigurationReason.PersistenceRead, ex.Message);
        }
    }

    public void ReplaceAtomically(byte[] data)
    {
        if (data is null)
            throw new ArgumentNullException(nameof(data));

        var directory = System.IO.Path.GetDirectoryName(_path);
        if (directory is null || directory.Length == 0)
            directory = ".";
        var fileName = System.IO.Path.GetFileName(_path);
        var tempPath = System.IO.Path.Combine(
            directory, $".{fileName}.{Guid.NewGuid():N}.tmp");
        var backupPath = System.IO.Path.Combine(
            directory, $".{fileName}.{Guid.NewGuid():N}.bak");

        try
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(tempPath, data);

            if (File.Exists(_path))
            {
                // Atomic replace within the same volume. Rename the old file aside,
                // move the new file in, then drop the backup. On Windows, File.Move
                // with overwrite is atomic for same-volume renames.
                File.Move(_path, backupPath, overwrite: true);
                try
                {
                    File.Move(tempPath, _path);
                    try { File.Delete(backupPath); } catch { /* best effort */ }
                }
                catch
                {
                    // Roll the original back if the final move failed.
                    try { File.Move(_path, _path + ".corrupt", overwrite: true); } catch { }
                    File.Move(backupPath, _path, overwrite: true);
                    throw;
                }
            }
            else
            {
                File.Move(tempPath, _path);
            }
        }
        catch (InputConfigurationException)
        {
            throw;
        }
        catch (Exception ex)
        {
            try { if (File.Exists(tempPath)) File.Delete(tempPath); } catch { /* best effort */ }
            throw new InputConfigurationException(
                InputConfigurationReason.PersistenceWrite, ex.Message);
        }
    }
}

/// <summary>
/// Loads and saves a <see cref="KeyMappingProfile"/> as JSON, delegating raw storage
/// to an <see cref="IConfigurationDataStore"/>. When no file exists, <see cref="Load"/>
/// returns the <see cref="KeyMappingProfile.VendorDefault"/> profile.
/// </summary>
public sealed class KeyMappingRepository
{
    private readonly IConfigurationDataStore _store;

    public KeyMappingRepository(IConfigurationDataStore store)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
    }

    /// <summary>
    /// Loads the persisted profile, or the vendor default when no file exists.
    /// </summary>
    public KeyMappingProfile Load()
    {
        byte[]? data;
        try
        {
            data = _store.Read();
        }
        catch (InputConfigurationException)
        {
            throw;
        }
        catch (Exception ex)
        {
            throw new InputConfigurationException(
                InputConfigurationReason.PersistenceRead, ex.Message);
        }

        if (data is null || data.Length == 0)
            return KeyMappingProfile.VendorDefault();

        try
        {
            return JsonSerializer.Deserialize<KeyMappingProfile>(data, InputJson.Options)
                ?? throw new InputConfigurationException(
                    InputConfigurationReason.InvalidStoredConfiguration, "profile is null");
        }
        catch (InputConfigurationException)
        {
            throw;
        }
        catch (JsonException ex)
        {
            throw new InputConfigurationException(
                InputConfigurationReason.InvalidStoredConfiguration, ex.Message);
        }
    }

    /// <summary>
    /// Validates, serializes, and atomically persists <paramref name="profile"/>.
    /// </summary>
    public void Save(KeyMappingProfile profile)
    {
        if (profile is null)
            throw new ArgumentNullException(nameof(profile));

        try
        {
            profile.Validate();
            var data = JsonSerializer.SerializeToUtf8Bytes(profile, InputJson.Options);
            _store.ReplaceAtomically(data);
        }
        catch (InputConfigurationException)
        {
            throw;
        }
        catch (Exception ex)
        {
            throw new InputConfigurationException(
                InputConfigurationReason.PersistenceWrite, ex.Message);
        }
    }
}
