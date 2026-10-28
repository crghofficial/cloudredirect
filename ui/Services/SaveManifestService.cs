using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;

namespace CloudRedirect.Services;

/// <summary>One folder the page offers as the game's save location.</summary>
public sealed record SaveSuggestion(string Root, string Path, string Pattern, bool Recursive, string DisplayPath, bool FoundByName)
{
    public SaveRule ToRule() => new(Root, Path, Pattern, Recursive);
    public string Label =>
        $"{DisplayPath}  ·  {Pattern}{(Recursive ? " +" : "")}{(FoundByName ? "  ·  ~" : "")}";
}

public sealed record SaveDetection(List<SaveSuggestion> Suggestions, List<string> Skipped);

/// <summary>
/// Suggests save folders for a game. Locations come from the public Ludusavi manifest
/// (https://github.com/mtkennerly/ludusavi-manifest, MIT), downloaded on demand and cached as a
/// compact JSON in %LOCALAPPDATA%\CloudRedirect. Only folders that exist on this PC are offered,
/// and only folders under a root Steam Auto-Cloud supports.
/// </summary>
public static class SaveManifestService
{
    private const string ManifestUrl =
        "https://raw.githubusercontent.com/mtkennerly/ludusavi-manifest/master/data/manifest.yaml";
    private static readonly TimeSpan CacheMaxAge = TimeSpan.FromDays(14);
    private const int MaxSuggestions = 8;
    private const int MaxSearchKeys = 3;
    private const int SearchDepth = 4;
    private const int SearchVisitedLimit = 60_000;
    private static readonly TimeSpan SearchBudget = TimeSpan.FromSeconds(6);
    private const int ScanLimit = 5000;

    private static readonly string[] MetadataExtensions =
        { "log", "json", "txt", "ini", "cfg", "xml", "tmp", "lock", "dmp", "md" };

    private static readonly HashSet<string> GenericNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "save", "saves", "savegame", "savegames", "saved", "data", "config", "settings", "profile",
        "profiles", "userdata", "users", "user", "cache", "local", "game", "games", "documents",
        "appdata", "steam", "remote", "backup", "backups", "win64", "win32", "windows", "output", "files",
    };

    private static readonly HashSet<string> SkipDirs = new(StringComparer.OrdinalIgnoreCase)
    {
        "temp", "packages", "microsoft", "google", "mozilla", "nvidia", "d3dscache", "crashdumps",
        "windowsapps", "node_modules", "cache", "code cache",
    };

    private static Dictionary<uint, List<string>>? _templates;
    private static readonly SemaphoreSlim LoadLock = new(1, 1);

    // ── manifest loading ────────────────────────────────────────────────

    private static string? CacheFile()
    {
        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        return string.IsNullOrEmpty(local) ? null : Path.Combine(local, "CloudRedirect", "save-manifest.json");
    }

    private static Dictionary<uint, List<string>>? ReadCache(string? file)
    {
        try
        {
            if (file == null || !File.Exists(file)) return null;
            return JsonSerializer.Deserialize<Dictionary<uint, List<string>>>(File.ReadAllBytes(file));
        }
        catch { return null; }
    }

    private static bool IsFresh(string? file) =>
        file != null && File.Exists(file) && DateTime.UtcNow - File.GetLastWriteTimeUtc(file) < CacheMaxAge;

    private static async Task<Dictionary<uint, List<string>>> LoadAsync()
    {
        await LoadLock.WaitAsync();
        try
        {
            if (_templates != null) return _templates;
            var cache = CacheFile();

            if (IsFresh(cache) && ReadCache(cache) is { } fresh)
                return _templates = fresh;

            try
            {
                var parsed = await DownloadAndParseAsync();
                if (cache != null)
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(cache)!);
                    var temp = cache + ".tmp";
                    File.WriteAllBytes(temp, JsonSerializer.SerializeToUtf8Bytes(parsed));
                    File.Move(temp, cache, overwrite: true);
                }
                return _templates = parsed;
            }
            catch
            {
                // Offline or GitHub unavailable: an outdated cache is better than nothing.
                if (ReadCache(cache) is { } stale) return _templates = stale;
                throw;
            }
        }
        finally { LoadLock.Release(); }
    }

    private static async Task<Dictionary<uint, List<string>>> DownloadAndParseAsync()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(180) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("CloudRedirect");
        using var response = await http.GetAsync(ManifestUrl, HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync();
        using var reader = new StreamReader(stream);
        return await Task.Run(() => ParseManifest(reader));
    }

    /// <summary>
    /// Streams the manifest and keeps, per Steam AppID, the Windows paths tagged "save".
    /// Uses the YAML event parser so the whole document is never held in memory.
    /// </summary>
    internal static Dictionary<uint, List<string>> ParseManifest(TextReader reader)
    {
        var parser = new Parser(reader);
        var result = new Dictionary<uint, List<string>>();
        parser.Consume<StreamStart>();
        parser.Consume<DocumentStart>();
        parser.Consume<MappingStart>();
        while (!parser.TryConsume<MappingEnd>(out _))
        {
            parser.Consume<Scalar>(); // game name
            if (!parser.TryConsume<MappingStart>(out _))
            {
                parser.SkipThisAndNestedEvents();
                continue;
            }

            var paths = new List<string>();
            var ids = new List<uint>();
            while (!parser.TryConsume<MappingEnd>(out _))
            {
                var key = parser.Consume<Scalar>().Value;
                switch (key)
                {
                    case "files": ReadFiles(parser, paths); break;
                    case "steam": ReadSteam(parser, ids); break;
                    case "id": ReadExtraIds(parser, ids); break;
                    default: parser.SkipThisAndNestedEvents(); break;
                }
            }
            if (paths.Count == 0) continue;
            foreach (var id in ids)
            {
                if (!result.TryGetValue(id, out var list)) result[id] = list = new List<string>();
                list.AddRange(paths);
            }
        }
        return result;
    }

    private static void ReadFiles(Parser parser, List<string> paths)
    {
        if (!parser.TryConsume<MappingStart>(out _)) { parser.SkipThisAndNestedEvents(); return; }
        while (!parser.TryConsume<MappingEnd>(out _))
        {
            var path = parser.Consume<Scalar>().Value;
            if (!parser.TryConsume<MappingStart>(out _)) { parser.SkipThisAndNestedEvents(); continue; }

            bool save = false, conditionless = true, windowsSteam = false;
            while (!parser.TryConsume<MappingEnd>(out _))
            {
                var key = parser.Consume<Scalar>().Value;
                if (key == "tags" && parser.TryConsume<SequenceStart>(out _))
                {
                    while (!parser.TryConsume<SequenceEnd>(out _))
                    {
                        if (parser.TryConsume<Scalar>(out var tag)) { if (tag.Value == "save") save = true; }
                        else parser.SkipThisAndNestedEvents();
                    }
                }
                else if (key == "when" && parser.TryConsume<SequenceStart>(out _))
                {
                    conditionless = false;
                    while (!parser.TryConsume<SequenceEnd>(out _))
                    {
                        if (!parser.TryConsume<MappingStart>(out _)) { parser.SkipThisAndNestedEvents(); continue; }
                        string? os = null, store = null;
                        while (!parser.TryConsume<MappingEnd>(out _))
                        {
                            var ck = parser.Consume<Scalar>().Value;
                            if ((ck == "os" || ck == "store") && parser.TryConsume<Scalar>(out var value))
                            {
                                if (ck == "os") os = value.Value; else store = value.Value;
                            }
                            else parser.SkipThisAndNestedEvents();
                        }
                        if ((os == null || os == "windows") && (store == null || store == "steam"))
                            windowsSteam = true;
                    }
                }
                else parser.SkipThisAndNestedEvents();
            }
            if (save && (conditionless || windowsSteam)) paths.Add(path);
        }
    }

    private static void ReadSteam(Parser parser, List<uint> ids)
    {
        if (!parser.TryConsume<MappingStart>(out _)) { parser.SkipThisAndNestedEvents(); return; }
        while (!parser.TryConsume<MappingEnd>(out _))
        {
            var key = parser.Consume<Scalar>().Value;
            if (key == "id" && parser.TryConsume<Scalar>(out var value))
            {
                if (uint.TryParse(value.Value, out var id)) ids.Add(id);
            }
            else parser.SkipThisAndNestedEvents();
        }
    }

    private static void ReadExtraIds(Parser parser, List<uint> ids)
    {
        if (!parser.TryConsume<MappingStart>(out _)) { parser.SkipThisAndNestedEvents(); return; }
        while (!parser.TryConsume<MappingEnd>(out _))
        {
            var key = parser.Consume<Scalar>().Value;
            if (key == "steamExtra" && parser.TryConsume<SequenceStart>(out _))
            {
                while (!parser.TryConsume<SequenceEnd>(out _))
                {
                    if (parser.TryConsume<Scalar>(out var value)) { if (uint.TryParse(value.Value, out var id)) ids.Add(id); }
                    else parser.SkipThisAndNestedEvents();
                }
            }
            else parser.SkipThisAndNestedEvents();
        }
    }

    // ── path expansion ──────────────────────────────────────────────────

    private static string? Env(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        return string.IsNullOrEmpty(value) ? null : value;
    }

    /// <summary>Fills the manifest placeholders; null when one cannot be resolved on this PC.</summary>
    internal static string? Expand(string template, string appId, string installDir)
    {
        var output = new System.Text.StringBuilder();
        var rest = template;
        while (true)
        {
            var start = rest.IndexOf('<');
            if (start < 0) break;
            output.Append(rest, 0, start);
            var end = rest.IndexOf('>', start);
            if (end < 0) return null;
            string? value = rest[(start + 1)..end] switch
            {
                "home" => Env("USERPROFILE"),
                "winAppData" => Env("APPDATA"),
                "winLocalAppData" => Env("LOCALAPPDATA"),
                "winLocalAppDataLow" => Env("USERPROFILE") is { } h ? h + "\\AppData\\LocalLow" : null,
                "winDocuments" => Env("USERPROFILE") is { } d ? d + "\\Documents" : null,
                "winProgramData" => Env("PROGRAMDATA"),
                "winPublic" => Env("PUBLIC"),
                "osUserName" => Env("USERNAME"),
                "storeGameId" => appId,
                // Any Steam account; the rule ends up covering the parent folder.
                "storeUserId" => "*",
                "base" when installDir.Length > 0 => installDir,
                "game" when installDir.Length > 0 => Path.GetFileName(installDir.TrimEnd('\\', '/')),
                _ => null,
            };
            if (value == null) return null;
            output.Append(value);
            rest = rest[(end + 1)..];
        }
        output.Append(rest);
        return output.ToString();
    }

    /// <summary>Turns an expanded manifest path into (folder, file pattern, recursive).</summary>
    internal static (string Dir, string Pattern, bool Recursive)? Resolve(string expanded)
    {
        var segments = expanded.Replace('/', '\\').Split('\\', StringSplitOptions.RemoveEmptyEntries);
        var wild = Array.FindIndex(segments, s => s.IndexOfAny(new[] { '*', '?', '[' }) >= 0);
        if (wild < 0)
        {
            var path = string.Join('\\', segments);
            if (Directory.Exists(path)) return (path, "*", true);
            if (File.Exists(path)) return (Path.GetDirectoryName(path)!, Path.GetFileName(path), false);
            return null;
        }
        if (wild == 0) return null;
        var dir = string.Join('\\', segments[..wild]);
        if (!Directory.Exists(dir)) return null;
        var segment = segments[wild];
        return wild + 1 == segments.Length && !segment.Contains('[')
            ? (dir, segment, false)
            : (dir, "*", true);
    }

    // ── searching ───────────────────────────────────────────────────────

    /// <summary>The last literal folder name of a manifest path, when it is specific enough to search for.</summary>
    internal static string? SearchKey(string template)
    {
        var last = template.Split('/', '\\')[^1];
        if (last.Length == 0 || last.IndexOfAny(new[] { '<', '>', '*', '?', '[' }) >= 0
            || Path.HasExtension(last))
            return null;
        var numeric = last.All(char.IsAsciiDigit);
        var specific = numeric ? last.Length >= 4 : last.Length >= 6 && !GenericNames.Contains(last);
        return specific ? last : null;
    }

    private static List<string> SearchRoots()
    {
        var roots = new List<string>();
        foreach (var name in new[] { "APPDATA", "LOCALAPPDATA", "PROGRAMDATA" })
            if (Env(name) is { } p) roots.Add(p);
        if (Env("USERPROFILE") is { } home)
        {
            roots.Add(Path.Combine(home, "Documents"));
            roots.Add(Path.Combine(home, "Saved Games"));
            roots.Add(Path.Combine(home, "AppData", "LocalLow"));
        }
        return roots;
    }

    /// <summary>Folders literally named <paramref name="key"/> below the roots (bounded depth and time; links not followed).</summary>
    private static List<string> FindNamedDirs(List<string> roots, string key)
    {
        var clock = Stopwatch.StartNew();
        var visited = 0;
        var found = new List<string>();
        foreach (var root in roots)
        {
            var level = new List<string> { root };
            for (var depth = 0; depth < SearchDepth && level.Count > 0; depth++)
            {
                var next = new List<string>();
                foreach (var dir in level)
                {
                    IEnumerable<DirectoryInfo> children;
                    try { children = new DirectoryInfo(dir).EnumerateDirectories(); } catch { continue; }
                    foreach (var child in children)
                    {
                        if (++visited > SearchVisitedLimit || clock.Elapsed > SearchBudget) return found;
                        if (child.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue;
                        if (child.Name.Equals(key, StringComparison.OrdinalIgnoreCase))
                        {
                            if (!found.Contains(child.FullName, StringComparer.OrdinalIgnoreCase)) found.Add(child.FullName);
                        }
                        else if (!SkipDirs.Contains(child.Name))
                            next.Add(child.FullName);
                    }
                }
                level = next;
            }
        }
        return found;
    }

    /// <summary>True when nothing matches or every match looks like metadata (.log, .json, ...).</summary>
    private static bool LowConfidence(string dir, string pattern, bool recursive)
    {
        var count = 0;
        var pending = new Stack<string>();
        pending.Push(dir);
        while (pending.Count > 0)
        {
            IEnumerable<FileSystemInfo> entries;
            try { entries = new DirectoryInfo(pending.Pop()).EnumerateFileSystemInfos(); } catch { continue; }
            foreach (var entry in entries)
            {
                if (entry.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue;
                if (entry is DirectoryInfo)
                {
                    if (recursive) pending.Push(entry.FullName);
                    continue;
                }
                if (!SaveRulesService.Matches(pattern, entry.Name)) continue;
                if (++count > ScanLimit) return false;
                var extension = entry.Extension.TrimStart('.').ToLowerInvariant();
                if (!MetadataExtensions.Contains(extension)) return false;
            }
        }
        return true;
    }

    // ── suggestions ─────────────────────────────────────────────────────

    private static bool AddCandidate(List<(SaveSuggestion Item, string Dir)> found, string installDir, string dir,
        string pattern, bool recursive, bool byName)
    {
        if (SaveRulesService.Classify(dir, installDir) is not { } hit) return false;
        // Never sync an entire root (install folder, AppData, Documents...) by accident.
        if ((hit.Path.Length == 0 && pattern == "*") || hit.Path.Length > 240 || pattern.Length > 128
            || pattern.IndexOfAny(new[] { ':', '\\', '/' }) >= 0)
            return false;
        var item = new SaveSuggestion(hit.Root, hit.Path, pattern, recursive, dir, byName);
        if (found.Any(f => f.Item.Root == item.Root
                           && f.Item.Path.Equals(item.Path, StringComparison.OrdinalIgnoreCase)
                           && f.Item.Pattern == item.Pattern))
            return false;
        found.Add((item, dir));
        return true;
    }

    public static async Task<SaveDetection> SuggestAsync(string appId, string installDir)
    {
        if (!uint.TryParse(appId, out var id)) throw new ArgumentException("AppID inválido.");
        var templates = await LoadAsync();
        return await Task.Run(() => Suggest(id, appId, installDir,
            templates.TryGetValue(id, out var list) ? list : new List<string>()));
    }

    private static SaveDetection Suggest(uint id, string appId, string installDir, List<string> templates)
    {
        var found = new List<(SaveSuggestion Item, string Dir)>();
        var skipped = new List<string>();
        var keys = new List<(string Key, string Template)>();

        // Ordinal order, so the same game always yields the same suggestions whichever duplicate comes first.
        foreach (var template in templates.OrderBy(t => t, StringComparer.Ordinal))
        {
            var expanded = Expand(template, appId, installDir);
            if (expanded != null && Resolve(expanded) is { } hit)
            {
                AddCandidate(found, installDir, hit.Dir, hit.Pattern, hit.Recursive, false);
                continue;
            }
            if (expanded == null && !skipped.Contains(template)) skipped.Add(template);
            if (SearchKey(template) is { } key
                && !keys.Any(k => k.Key.Equals(key, StringComparison.OrdinalIgnoreCase)))
                keys.Add((key, template));
        }

        // Manifest paths that point at a store launcher or a missing folder: look for the same
        // folder name where saves usually end up (also covers relocated Documents/AppData).
        var roots = SearchRoots();
        foreach (var (key, template) in keys.Take(MaxSearchKeys))
        {
            var any = false;
            foreach (var dir in FindNamedDirs(roots, key))
                any |= AddCandidate(found, installDir, dir, "*", true, true);
            if (any) skipped.RemoveAll(s => s == template);
        }

        // Folders with real save files first; metadata-only or empty ones last.
        var ranked = found
            .Select(f => (Low: LowConfidence(f.Dir, f.Item.Pattern, f.Item.Recursive), f.Item))
            .OrderBy(r => r.Low)
            .Select(r => r.Item)
            .Take(MaxSuggestions)
            .ToList();
        return new SaveDetection(ranked, skipped);
    }
}
