using System.IO;
using System.Text.Json;

namespace CloudRedirect.Services;

public sealed record InstalledGame(string AppId, string Name, string InstallDir);

public sealed record SaveRule(string Root, string Path, string Pattern, bool Recursive)
{
    public bool SameAs(SaveRule other) =>
        Root.Equals(other.Root, StringComparison.OrdinalIgnoreCase)
        && Path.Replace('/', '\\').Equals(other.Path.Replace('/', '\\'), StringComparison.OrdinalIgnoreCase)
        && Pattern.Equals(other.Pattern, StringComparison.OrdinalIgnoreCase)
        && Recursive == other.Recursive;
}

public sealed record FolderStats(int FileCount, long TotalBytes, bool Truncated, List<string> Sample);
public sealed record SaveBackupResult(string Dir, int FileCount, long TotalBytes);
public sealed record RuleBackup(string Name, string Label, long Size);

/// <summary>
/// Manages game-specific Auto-Cloud save rules for CloudRedirect. Rules are stored in
/// %LOCALAPPDATA%\CloudRedirectdded-rules.json, which the DLL reads; nothing is written to
/// Steam's appinfo.vdf. appinfo.vdf is only read, to tell when the developer already published a
/// rule (those win over ours). This class also previews the folder and copies the saves to a backup.
/// </summary>
public static class SaveRulesService
{
    public static readonly string[] Roots =
    {
        "GameInstall", "WinMyDocuments", "WinAppDataLocal", "WinAppDataRoaming",
        "WinSavedGames", "WinProgramData", "WinAppDataLocalLow", "WindowsHome",
    };

    private const int BackupMaxFiles = 5000;
    private const long BackupMaxBytes = 2L * 1024 * 1024 * 1024;
    private const int StatsMaxFiles = 20000;

    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
    };

    // ── installed games ─────────────────────────────────────────────────

    public static List<InstalledGame> ScanInstalledGames(string steamPath)
    {
        var games = new List<InstalledGame>();
        foreach (var library in LibraryFolders(steamPath))
        {
            var apps = Path.Combine(library, "steamapps");
            if (!Directory.Exists(apps)) continue;
            foreach (var manifest in Directory.EnumerateFiles(apps, "appmanifest_*.acf"))
            {
                var id = Path.GetFileNameWithoutExtension(manifest)["appmanifest_".Length..];
                if (id.Length == 0 || !id.All(char.IsAsciiDigit)) continue;
                var (name, installDir) = ReadManifest(manifest);
                if (installDir == null) continue;
                games.Add(new InstalledGame(id, name ?? id, Path.Combine(apps, "common", installDir)));
            }
        }
        return games.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    private static List<string> LibraryFolders(string steamPath)
    {
        var paths = new List<string> { steamPath };
        var vdf = Path.Combine(steamPath, "config", "libraryfolders.vdf");
        if (!File.Exists(vdf)) return paths;
        try
        {
            foreach (var line in File.ReadLines(vdf))
            {
                var trimmed = line.Trim();
                if (!trimmed.StartsWith("\"path\"", StringComparison.OrdinalIgnoreCase)) continue;
                var parts = trimmed.Split('"');
                if (parts.Length < 4) continue;
                var p = parts[3].Replace("\\\\", "\\");
                if (Directory.Exists(p) && !paths.Contains(p, StringComparer.OrdinalIgnoreCase))
                    paths.Add(p);
            }
        }
        catch { }
        return paths;
    }

    private static (string? Name, string? InstallDir) ReadManifest(string path)
    {
        string? name = null, dir = null;
        try
        {
            foreach (var line in File.ReadLines(path))
            {
                var trimmed = line.Trim();
                var parts = trimmed.Split('"');
                if (parts.Length < 4) continue;
                if (trimmed.StartsWith("\"name\"", StringComparison.OrdinalIgnoreCase)) name ??= parts[3];
                else if (trimmed.StartsWith("\"installdir\"", StringComparison.OrdinalIgnoreCase)) dir ??= parts[3];
            }
        }
        catch { }
        return (name, dir);
    }

    // ── rules published in appinfo.vdf (read only) ──

    /// <summary>
    /// Rules the developer published in appinfo.vdf. Those always win over ours, so the page
    /// uses them to warn. If the file cannot be read the list is empty.
    /// </summary>
    public static List<SaveRule> InspectRules(string steamPath, string appId)
    {
        RequireAppId(appId);
        try
        {
            var appInfo = Path.Combine(steamPath, "appcache", "appinfo.vdf");
            var config = File.Exists(appInfo) ? AppInfoParser.ParseSingle(appInfo, uint.Parse(appId)) : null;
            return config?.SaveFiles
                .Select(r => new SaveRule(r.Root, r.Path, string.IsNullOrEmpty(r.Pattern) ? "*" : r.Pattern, r.Recursive))
                .ToList() ?? new List<SaveRule>();
        }
        catch
        {
            // Only a courtesy check; the page works without it.
            return new List<SaveRule>();
        }
    }

    // ── rules added by the user (%LOCALAPPDATA%\CloudRedirect\added-rules.json) ──
    // The DLL reads this file when appinfo.vdf has no savefiles rule for the app and
    // injects the rule into Steam's view, so no change to appinfo.vdf is needed.

    public static void ApplyRule(string appId, SaveRule rule, SaveRule? replacing)
    {
        RequireAppId(appId);
        ValidateRule(rule);
        var file = RulesFile() ?? throw new InvalidOperationException("LOCALAPPDATA indisponível.");
        BackupRulesFile(file);

        var all = ReadFile(file);
        all[appId] = rule with { Path = rule.Path.Replace('/', '\\') };
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var temp = file + ".tmp";
        File.WriteAllBytes(temp, JsonSerializer.SerializeToUtf8Bytes(all, Json));
        File.Move(temp, file, overwrite: true);
    }

    private static string? RuleBackupsDir()
    {
        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        return string.IsNullOrEmpty(local) ? null : Path.Combine(local, "CloudRedirect", "rule-backups");
    }

    /// <summary>Keeps a copy of the rules file before each change so it can be restored.</summary>
    private static void BackupRulesFile(string file)
    {
        var dir = RuleBackupsDir();
        if (dir == null || !File.Exists(file)) return;
        Directory.CreateDirectory(dir);
        var name = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + ".json";
        File.Copy(file, Path.Combine(dir, name), overwrite: false);
        foreach (var old in new DirectoryInfo(dir).GetFiles("*.json").OrderByDescending(f => f.Name).Skip(30))
        {
            try { old.Delete(); } catch { }
        }
    }

    public static List<RuleBackup> ListRuleBackups()
    {
        var dir = RuleBackupsDir();
        if (dir == null || !Directory.Exists(dir)) return new List<RuleBackup>();
        return new DirectoryInfo(dir).GetFiles("*.json")
            .OrderByDescending(f => f.Name)
            .Select(f => new RuleBackup(f.Name, f.LastWriteTime.ToString("g"), f.Length))
            .ToList();
    }

    public static void RestoreRuleBackup(string name)
    {
        if (name.Length == 0 || name.Length > 64
            || !name.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-'))
            throw new ArgumentException("Backup inválido.");
        var dir = RuleBackupsDir() ?? throw new InvalidOperationException("LOCALAPPDATA indisponível.");
        var source = Path.Combine(dir, name);
        if (!File.Exists(source)) throw new FileNotFoundException("Backup não encontrado.");
        var file = RulesFile()!;
        BackupRulesFile(file);
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.Copy(source, file, overwrite: true);
    }

    private static void RequireAppId(string appId)
    {
        if (appId.Length == 0 || appId.Length > 10 || !appId.All(char.IsAsciiDigit))
            throw new ArgumentException("AppID inválido.");
    }

    private static void ValidateRule(SaveRule rule)
    {
        if (!Roots.Contains(rule.Root)) throw new ArgumentException("Raiz Steam inválida.");
        if (rule.Path.Length > 240 || rule.Pattern.Length is 0 or > 128)
            throw new ArgumentException("Caminho ou padrão inválido.");
    }

    private static string? RulesFile()
    {
        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        return string.IsNullOrEmpty(local) ? null : Path.Combine(local, "CloudRedirect", "added-rules.json");
    }

    private static Dictionary<string, SaveRule> ReadFile(string? file)
    {
        try
        {
            if (file != null && File.Exists(file))
                return JsonSerializer.Deserialize<Dictionary<string, SaveRule>>(File.ReadAllBytes(file), Json) ?? new();
        }
        catch { }
        return new();
    }

    /// <summary>The rules this app has saved, by app id.</summary>
    public static Dictionary<string, SaveRule> ReadRemembered() => ReadFile(RulesFile());

    // ── folder <-> Steam root ───────────────────────────────────────────

    private static List<(string Root, string Base)> RootBases(string installDir)
    {
        var bases = new List<(string, string)>();
        if (!string.IsNullOrEmpty(installDir)) bases.Add(("GameInstall", installDir));
        void Add(string root, string? path) { if (!string.IsNullOrEmpty(path)) bases.Add((root, path)); }
        Add("WinAppDataRoaming", Environment.GetEnvironmentVariable("APPDATA"));
        Add("WinAppDataLocal", Environment.GetEnvironmentVariable("LOCALAPPDATA"));
        Add("WinProgramData", Environment.GetEnvironmentVariable("PROGRAMDATA"));
        var home = Environment.GetEnvironmentVariable("USERPROFILE");
        if (!string.IsNullOrEmpty(home))
        {
            Add("WindowsHome", home);
            Add("WinMyDocuments", Path.Combine(home, "Documents"));
            Add("WinSavedGames", Path.Combine(home, "Saved Games"));
            Add("WinAppDataLocalLow", Path.Combine(home, "AppData", "LocalLow"));
        }
        return bases;
    }

    private static string Normalize(string path) => Path.GetFullPath(path).TrimEnd('\\', '/');

    /// <summary>Maps a folder to the most specific Steam root that contains it, plus the relative path.</summary>
    public static (string Root, string Path)? Classify(string selected, string installDir)
    {
        var target = Normalize(selected);
        (int Depth, string Root, string Relative)? best = null;
        foreach (var (root, baseDir) in RootBases(installDir))
        {
            string b;
            try { b = Normalize(baseDir); } catch { continue; }
            bool inside = target.Equals(b, StringComparison.OrdinalIgnoreCase)
                || target.StartsWith(b + "\\", StringComparison.OrdinalIgnoreCase);
            if (!inside) continue;
            var depth = b.Count(c => c == '\\');
            if (best == null || depth > best.Value.Depth)
                best = (depth, root, target.Length > b.Length ? target[(b.Length + 1)..] : "");
        }
        return best is { } hit ? (hit.Root, hit.Relative) : null;
    }

    public static string? ResolveFolder(SaveRule rule, string installDir)
    {
        var baseDir = RootBases(installDir).FirstOrDefault(b => b.Root == rule.Root).Base;
        if (baseDir == null) return null;
        var relative = rule.Path.Replace('/', '\\');
        if (relative.Contains(':') || relative.StartsWith('\\') || relative.Split('\\').Any(p => p == ".."))
            return null;
        return relative.Length == 0 ? baseDir : Path.Combine(baseDir, relative);
    }

    // ── preview and backup of the save folder ───────────────────────────

    /// <summary>Same wildcard rules Steam uses for the pattern: only * and ? are special.</summary>
    internal static bool Matches(string pattern, string name)
    {
        bool Walk(int p, int n)
        {
            while (p < pattern.Length)
            {
                if (pattern[p] == '*')
                {
                    for (int skip = n; skip <= name.Length; skip++)
                        if (Walk(p + 1, skip)) return true;
                    return false;
                }
                if (n >= name.Length) return false;
                if (pattern[p] != '?' && char.ToLowerInvariant(pattern[p]) != char.ToLowerInvariant(name[n])) return false;
                p++; n++;
            }
            return n == name.Length;
        }
        return Walk(0, 0);
    }

    public static FolderStats Stats(SaveRule rule, string installDir)
    {
        var folder = ResolveFolder(rule, installDir);
        if (folder == null || !Directory.Exists(folder))
            return new FolderStats(0, 0, false, new List<string>());

        int count = 0;
        long bytes = 0;
        bool truncated = false;
        var sample = new List<string>();
        var pending = new Stack<string>();
        pending.Push(folder);
        while (pending.Count > 0 && !truncated)
        {
            var dir = pending.Pop();
            IEnumerable<FileSystemInfo> entries;
            try { entries = new DirectoryInfo(dir).EnumerateFileSystemInfos(); } catch { continue; }
            foreach (var entry in entries)
            {
                if (entry.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue;
                if (entry is DirectoryInfo)
                {
                    if (rule.Recursive) pending.Push(entry.FullName);
                    continue;
                }
                if (!Matches(rule.Pattern, entry.Name)) continue;
                // Steam writes this bookkeeping file into the folder and never syncs it.
                if (entry.Name.Equals("steam_autocloud.vdf", StringComparison.OrdinalIgnoreCase)) continue;
                if (count >= StatsMaxFiles) { truncated = true; break; }
                count++;
                bytes += ((FileInfo)entry).Length;
                if (sample.Count < 8)
                    sample.Add(Path.GetRelativePath(folder, entry.FullName));
            }
        }
        return new FolderStats(count, bytes, truncated, sample);
    }

    /// <summary>
    /// Copies the save folder to %LOCALAPPDATA%\CloudRedirect\save-backups\appid\timestamp.
    /// The source is only read, and symlinks are skipped.
    /// </summary>
    public static SaveBackupResult BackupSaves(string appId, SaveRule rule, string installDir)
    {
        RequireAppId(appId);
        var source = ResolveFolder(rule, installDir) ?? throw new InvalidOperationException("Caminho inválido.");
        if (!Directory.Exists(source))
            throw new InvalidOperationException("A pasta de saves não existe neste computador.");

        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA") ?? throw new InvalidOperationException("LOCALAPPDATA indisponível.");
        var backupsRoot = Path.Combine(local, "CloudRedirect", "save-backups");
        var dest = Path.Combine(backupsRoot, appId, DateTime.Now.ToString("yyyyMMdd-HHmmss"));

        var files = new List<(string Path, long Size)>();
        long total = 0;
        var pending = new Stack<string>();
        pending.Push(source);
        while (pending.Count > 0)
        {
            var dir = pending.Pop();
            IEnumerable<FileSystemInfo> entries;
            try { entries = new DirectoryInfo(dir).EnumerateFileSystemInfos(); } catch { continue; }
            foreach (var entry in entries)
            {
                if (entry.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue;
                if (entry.FullName.StartsWith(backupsRoot, StringComparison.OrdinalIgnoreCase)) continue;
                if (entry is DirectoryInfo) { pending.Push(entry.FullName); continue; }
                var size = ((FileInfo)entry).Length;
                total += size;
                if (files.Count >= BackupMaxFiles || total > BackupMaxBytes)
                    throw new InvalidOperationException("A pasta é grande demais para o backup automático (limite de 5000 arquivos ou 2 GB).");
                files.Add((entry.FullName, size));
            }
        }
        if (files.Count == 0)
            throw new InvalidOperationException("Não há arquivos nessa pasta para copiar.");

        foreach (var (file, _) in files)
        {
            var relative = Path.GetRelativePath(source, file);
            var target = Path.Combine(dest, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target, overwrite: false);
        }
        return new SaveBackupResult(dest, files.Count, total);
    }
}
