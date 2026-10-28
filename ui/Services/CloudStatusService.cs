using System.Globalization;
using System.IO;

namespace CloudRedirect.Services;

/// <summary>Per-game sync status derived from Steam's cloud_log.txt and cloud_redirect.log.</summary>
public sealed class CloudGameStatus
{
    public string AppId { get; set; } = "";
    public DateTime? LastSyncAt { get; set; }
    /// <summary>"launch", "exit" or "other".</summary>
    public string? LastSyncKind { get; set; }
    /// <summary>"ok", "slow" or "failed"; null while the sync has no recognisable outcome.</summary>
    public string? LastSyncResult { get; set; }
    public int? LastSyncSecs { get; set; }
    public string? LastError { get; set; }
    public int? WatchedFiles { get; set; }
    public long? CloudCn { get; set; }
    public int? CloudFiles { get; set; }

    /// <summary>True when the last exit sync matched no files although the cloud holds some: the save rule is missing.</summary>
    public bool ExitWatchedNothing =>
        LastSyncKind == "exit" && WatchedFiles == 0 && (CloudFiles ?? 0) > 0;
}

public sealed class CloudHealth
{
    public DateTime? SessionStartedAt { get; set; }
    public string? DllVersion { get; set; }
    public string? SteamBuild { get; set; }
    public DateTime? SteamclientModified { get; set; }
    /// <summary>"ok", "partial", "failed" or "unknown".</summary>
    public string Resolver { get; set; } = "unknown";
    public string? ResolverDetail { get; set; }
    public int? ResolverFailed { get; set; }
    public bool? InjectorReady { get; set; }
    public bool? HooksActive { get; set; }
    public bool InitComplete { get; set; }
}

public sealed class CloudStatusSnapshot
{
    public bool LogFound { get; set; }
    public bool SteamRunning { get; set; }
    public CloudHealth Health { get; set; } = new();
    public List<CloudGameStatus> Games { get; set; } = new();
}

/// <summary>
/// Read-only: nothing here writes to either log. A sync that spends at least
/// <see cref="SlowSyncSecs"/> between its first and last line is reported as slow.
/// </summary>
public static class CloudStatusService
{
    public const int SlowSyncSecs = 10;
    private const int ErrorTextMax = 160;

    public static CloudStatusSnapshot Read(string steamPath)
    {
        var snapshot = new CloudStatusSnapshot { SteamRunning = SteamDetector.IsSteamRunning() };
        var redirectLog = Path.Combine(steamPath, "cloud_redirect.log");
        if (!File.Exists(redirectLog))
            return snapshot;

        snapshot.LogFound = true;
        var redirect = ParseRedirectLog(ReadLines(redirectLog));
        snapshot.Health = redirect.Health;
        try
        {
            var client = Path.Combine(steamPath, "steamclient64.dll");
            if (File.Exists(client))
                snapshot.Health.SteamclientModified = File.GetLastWriteTime(client);
        }
        catch { }

        var statuses = ParseSteamCloudLog(ReadLines(Path.Combine(steamPath, "logs", "cloud_log.txt")));
        foreach (var appId in redirect.Managed)
        {
            statuses.TryGetValue(appId, out var status);
            status ??= new CloudGameStatus();
            status.AppId = appId;
            if (redirect.Cloud.TryGetValue(appId, out var cloud))
            {
                status.CloudCn = cloud.Cn;
                status.CloudFiles = cloud.Files;
            }
            snapshot.Games.Add(status);
        }
        snapshot.Games = snapshot.Games
            .OrderByDescending(g => g.LastSyncAt ?? DateTime.MinValue)
            .ThenBy(g => g.AppId, StringComparer.Ordinal)
            .ToList();
        return snapshot;
    }

    /// <summary>Opens the log with sharing so Steam can keep appending while we read it.</summary>
    private static List<string> ReadLines(string path)
    {
        var lines = new List<string>();
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            string? line;
            while ((line = reader.ReadLine()) != null)
                lines.Add(line);
        }
        catch { }
        return lines;
    }

    private sealed class OpenSync
    {
        public DateTime Start;
        public DateTime End;
        public string Kind = "other";
        public string? Failed;
        public bool Completed;
        public int? Matched;

        public void Finish(CloudGameStatus status)
        {
            // Background init/eval passes run after every launch; they must not hide the
            // launch or exit sync the user cares about.
            if (Kind == "other" && status.LastSyncKind is "launch" or "exit")
                return;
            if (Matched.HasValue)
                status.WatchedFiles = Matched;
            var secs = Math.Max(0, (int)(End - Start).TotalSeconds);
            status.LastSyncAt = Start;
            status.LastSyncKind = Kind;
            status.LastSyncSecs = secs;
            status.LastError = Failed;
            status.LastSyncResult = Failed != null ? "failed"
                : secs >= SlowSyncSecs ? "slow"
                : Completed ? "ok"
                : null;
        }
    }

    /// <summary>Parses the leading "[YYYY-MM-DD HH:MM:SS] " of a Steam log line.</summary>
    private static bool TryStamp(string line, out DateTime when, out string rest)
    {
        when = default;
        rest = "";
        if (line.Length < 23 || line[0] != '[' || line[20] != ']' || line[21] != ' ')
            return false;
        if (!DateTime.TryParseExact(line.AsSpan(1, 19), "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out when))
            return false;
        rest = line[22..];
        return true;
    }

    private static string SyncKind(string started) =>
        started.Contains("AC Launch", StringComparison.Ordinal) ? "launch"
        : started.Contains("AC Exit", StringComparison.Ordinal) ? "exit"
        : "other";

    private static string Clip(string text)
    {
        text = text.Trim();
        return text.Length <= ErrorTextMax ? text : text[..ErrorTextMax] + "…";
    }

    private static bool AllDigits(string value) => value.Length > 0 && value.All(char.IsAsciiDigit);

    private static string? DigitsPrefix(string text)
    {
        int end = 0;
        while (end < text.Length && char.IsAsciiDigit(text[end])) end++;
        return end > 0 ? text[..end] : null;
    }

    private static string? FieldAfter(string line, string marker)
    {
        var at = line.IndexOf(marker, StringComparison.Ordinal);
        return at < 0 ? null : line[(at + marker.Length)..];
    }

    private static int? LeadingInt(string text)
    {
        var first = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
        return first != null && int.TryParse(first, NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : null;
    }

    internal static Dictionary<string, CloudGameStatus> ParseSteamCloudLog(IEnumerable<string> lines)
    {
        var statuses = new Dictionary<string, CloudGameStatus>();
        var open = new Dictionary<string, OpenSync>();

        CloudGameStatus StatusFor(string appId)
        {
            if (!statuses.TryGetValue(appId, out var s))
                statuses[appId] = s = new CloudGameStatus();
            return s;
        }

        foreach (var line in lines)
        {
            if (!TryStamp(line, out var when, out var rest)) continue;
            if (!rest.StartsWith("[AppID ", StringComparison.Ordinal)) continue;
            var body = rest["[AppID ".Length..];
            var split = body.IndexOf("] ", StringComparison.Ordinal);
            if (split < 0) continue;
            var appId = body[..split];
            var message = body[(split + 2)..];
            if (!AllDigits(appId)) continue;

            if (message.StartsWith("Starting sync (", StringComparison.Ordinal))
            {
                if (open.Remove(appId, out var previous))
                    previous.Finish(StatusFor(appId));
                open[appId] = new OpenSync
                {
                    Start = when,
                    End = when,
                    Kind = SyncKind(message["Starting sync (".Length..]),
                };
                continue;
            }

            const string watching = "AutoCloud done. Watching ";
            if (message.StartsWith(watching, StringComparison.Ordinal) && LeadingInt(message[watching.Length..]) is { } watched)
                StatusFor(appId).WatchedFiles = watched;

            if (!open.TryGetValue(appId, out var sync)) continue;
            sync.End = when;

            var trimmed = message.TrimStart();
            if (trimmed.StartsWith("Found ", StringComparison.Ordinal)
                && trimmed.Contains(" files that match", StringComparison.Ordinal)
                && LeadingInt(trimmed["Found ".Length..]) is { } found)
                sync.Matched = (sync.Matched ?? 0) + found;

            if (message.Contains("BYielding", StringComparison.Ordinal) && message.Contains("Failed", StringComparison.Ordinal))
                sync.Failed ??= Clip(message);
            else if (message.StartsWith("Upload complete", StringComparison.Ordinal)
                  || message.StartsWith("Download complete", StringComparison.Ordinal)
                  || message.StartsWith("Successfully synced", StringComparison.Ordinal))
                sync.Completed = true;
        }

        foreach (var (appId, sync) in open)
            sync.Finish(StatusFor(appId));
        return statuses;
    }

    internal sealed class RedirectLog
    {
        public SortedSet<string> Managed { get; } = new(StringComparer.Ordinal);
        public Dictionary<string, (long Cn, int? Files)> Cloud { get; } = new();
        public CloudHealth Health { get; set; } = new();
    }

    internal static RedirectLog ParseRedirectLog(IEnumerable<string> lines)
    {
        var log = new RedirectLog();
        const string loaded = "=== CloudRedirect loaded at ";
        foreach (var line in lines)
        {
            if (line.StartsWith(loaded, StringComparison.Ordinal))
            {
                // A new session: health describes only the latest one.
                var value = line[loaded.Length..];
                if (value.EndsWith(" ===", StringComparison.Ordinal)) value = value[..^4];
                string stamp = value;
                string? build = null;
                var b = value.IndexOf(" [BUILD:", StringComparison.Ordinal);
                if (b >= 0)
                {
                    stamp = value[..b];
                    build = value[(b + " [BUILD:".Length)..].TrimEnd(']');
                }
                DateTime? started = DateTime.TryParseExact(stamp, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var t) ? t : null;
                log.Health = new CloudHealth { SessionStartedAt = started, DllVersion = build };
                continue;
            }

            foreach (var marker in new[] { "[NS-CL] GetAppFileChangelist app=", "[NS] SignalAppLaunchIntent app=", "[NS] BeginBatch app=" })
            {
                var app = FieldAfter(line, marker) is { } tail ? DigitsPrefix(tail) : null;
                if (app != null) log.Managed.Add(app);
            }

            foreach (var marker in new[] { "FetchCloudState app ", "PublishCloudState app " })
            {
                if (FieldAfter(line, marker) is not { } tail) continue;
                var sep = tail.IndexOf(": ", StringComparison.Ordinal);
                if (sep < 0) continue;
                var app = tail[..sep];
                var rest = tail[(sep + 2)..];
                if (FieldAfter(rest, "CN=") is not { } cnTail || DigitsPrefix(cnTail) is not { } cnText
                    || !long.TryParse(cnText, NumberStyles.None, CultureInfo.InvariantCulture, out var cn))
                    continue;
                int? files = null;
                var comma = rest.IndexOf(", ", StringComparison.Ordinal);
                if (comma >= 0 && DigitsPrefix(rest[(comma + 2)..]) is { } filesText
                    && int.TryParse(filesText, NumberStyles.None, CultureInfo.InvariantCulture, out var f))
                    files = f;
                if (AllDigits(app))
                    log.Cloud[app] = (cn, files);
            }

            var health = log.Health;
            if (FieldAfter(line, "] Steam version: ") is { } build2)
                health.SteamBuild = build2.Trim();
            if (FieldAfter(line, "[Resolver] CAppInfoCache offset ") is { } detail)
                health.ResolverDetail = Clip(detail);
            if (FieldAfter(line, "[Resolver] Result: ") is { } result)
            {
                // "0/36 matched, 3 failed"
                foreach (var part in result.Split(", "))
                {
                    if (part.EndsWith(" failed", StringComparison.Ordinal)
                        && int.TryParse(part[..^" failed".Length].Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var failed))
                        health.ResolverFailed = failed;
                }
            }
            if (line.Contains("[KvInjector] Init: using resolver addresses", StringComparison.Ordinal))
                health.InjectorReady = true;
            else if (line.Contains("[KvInjector] Init:", StringComparison.Ordinal)
                  && (line.Contains("incomplete", StringComparison.Ordinal) || line.Contains("aborting", StringComparison.Ordinal)))
                health.InjectorReady = false;
            if (line.Contains("[VtHook] All hooks ACTIVE", StringComparison.Ordinal))
                health.HooksActive = true;
            else if (line.Contains("[VtHook] WARNING: Some hooks failed", StringComparison.Ordinal))
                health.HooksActive = false;
            if (line.Contains("CR_InitCloudSave complete", StringComparison.Ordinal))
                health.InitComplete = true;
        }

        log.Health.Resolver = (log.Health.InjectorReady, log.Health.ResolverFailed) switch
        {
            (true, > 0) => "partial",
            (true, _) => "ok",
            (false, _) => "failed",
            _ => "unknown",
        };
        return log;
    }
}
