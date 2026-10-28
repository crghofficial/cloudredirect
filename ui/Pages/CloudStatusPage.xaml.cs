using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CloudRedirect.Resources;
using CloudRedirect.Services;
using SymbolIcon = Wpf.Ui.Controls.SymbolIcon;
using SymbolRegular = Wpf.Ui.Controls.SymbolRegular;
using CardControl = Wpf.Ui.Controls.CardControl;

namespace CloudRedirect.Pages;

public partial class CloudStatusPage : Page
{
    private enum Tone { Ok, Warn, Bad, Unknown }

    public CloudStatusPage()
    {
        InitializeComponent();
        Loaded += async (_, _) => await RefreshAsync();
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await RefreshAsync();

    private async Task RefreshAsync()
    {
        try
        {
            var data = await Task.Run(() =>
            {
                var steamPath = SteamDetector.FindSteamPath();
                if (steamPath == null) return (null, new Dictionary<string, string>());
                var snapshot = CloudStatusService.Read(steamPath);
                var names = new Dictionary<string, string>();
                foreach (var game in SaveRulesService.ScanInstalledGames(steamPath))
                    names.TryAdd(game.AppId, game.Name);
                return (snapshot, names);
            });
            Render(data.Item1, data.Item2);
        }
        catch (Exception ex)
        {
            HealthHeadline.Text = ex.Message;
            SetTone(Tone.Bad, HealthIcon);
        }
    }

    private void Render(CloudStatusSnapshot? snapshot, Dictionary<string, string> names)
    {
        GamesList.Children.Clear();
        if (snapshot == null)
        {
            HealthHeadline.Text = S.Get("Status_NoSteam");
            HealthDetails.Text = "";
            SetTone(Tone.Unknown, HealthIcon);
            ShowEmpty(S.Get("Status_NoGames"));
            return;
        }

        var (tone, text) = Headline(snapshot.Health, snapshot.LogFound);
        HealthHeadline.Text = text;
        SetTone(tone, HealthIcon);
        HealthDetails.Text = Details(snapshot);

        if (snapshot.Games.Count == 0)
        {
            ShowEmpty(S.Get("Status_NoGames"));
            return;
        }
        GamesEmpty.Visibility = Visibility.Collapsed;
        foreach (var game in snapshot.Games)
            GamesList.Children.Add(BuildRow(game, names.GetValueOrDefault(game.AppId)));
    }

    private void ShowEmpty(string text)
    {
        GamesEmpty.Text = text;
        GamesEmpty.Visibility = Visibility.Visible;
    }

    private static (Tone, string) Headline(CloudHealth health, bool logFound)
    {
        if (!logFound)
            return (Tone.Unknown, S.Get("Status_NoLog"));
        if (health.Resolver == "failed")
            return (Tone.Bad, S.Get("Status_ResolverFailed"));
        if (health.HooksActive == false)
            return (Tone.Bad, S.Get("Status_HooksFailed"));
        if (health.Resolver == "unknown" || !health.InitComplete)
            return (Tone.Unknown, S.Get("Status_NoSession"));
        if (health.Resolver == "partial")
            return (Tone.Warn, S.Format("Status_ResolverPartial", health.ResolverFailed ?? 0));
        return (Tone.Ok, S.Get("Status_AllGood"));
    }

    private static string Details(CloudStatusSnapshot snapshot)
    {
        var h = snapshot.Health;
        var parts = new List<string>();
        if (h.DllVersion != null) parts.Add($"DLL {h.DllVersion}");
        if (h.SteamBuild != null) parts.Add($"Steam {h.SteamBuild}");
        if (h.SessionStartedAt != null) parts.Add(S.Format("Status_Session", h.SessionStartedAt.Value.ToString("g")));
        parts.Add(snapshot.SteamRunning ? S.Get("Status_SteamOpen") : S.Get("Status_SteamClosed"));
        return string.Join("  ·  ", parts);
    }

    private UIElement BuildRow(CloudGameStatus game, string? name)
    {
        var tone = game.LastSyncResult switch
        {
            "ok" => Tone.Ok,
            "slow" => Tone.Warn,
            "failed" => Tone.Bad,
            _ => Tone.Unknown,
        };

        var title = new TextBlock
        {
            Text = name != null ? $"{name}  ({game.AppId})" : game.AppId,
            FontWeight = FontWeights.SemiBold,
        };
        title.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");

        var lines = new StackPanel();
        lines.Children.Add(title);
        lines.Children.Add(Line(SyncLine(game)));
        lines.Children.Add(Line(CloudLine(game)));
        if (game.ExitWatchedNothing)
            lines.Children.Add(Line(S.Get("Status_RuleMissing"), Tone.Warn));
        if (game.LastError != null)
            lines.Children.Add(Line(game.LastError, Tone.Bad));

        var icon = new SymbolIcon { FontSize = 24 };
        SetTone(tone, icon);

        return new CardControl { Margin = new Thickness(0, 0, 0, 12), Header = lines, Content = icon };
    }

    private static TextBlock Line(string text, Tone? tone = null)
    {
        var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = 12 };
        if (tone is Tone.Warn) block.Foreground = Brushes.Orange;
        else if (tone is Tone.Bad) block.Foreground = Brushes.IndianRed;
        else block.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
        return block;
    }

    private static string SyncLine(CloudGameStatus game)
    {
        if (game.LastSyncAt == null)
            return S.Get("Status_NoSyncYet");
        var kind = game.LastSyncKind switch
        {
            "launch" => S.Get("Status_KindLaunch"),
            "exit" => S.Get("Status_KindExit"),
            _ => S.Get("Status_KindOther"),
        };
        var result = game.LastSyncResult switch
        {
            "ok" => "OK",
            "slow" => S.Get("Status_ResultSlow"),
            "failed" => S.Get("Status_ResultFailed"),
            _ => S.Get("Status_ResultNone"),
        };
        var secs = game.LastSyncSecs is { } s ? $" · {s} s" : "";
        return $"{kind} · {Relative(game.LastSyncAt.Value)}{secs} · {result}";
    }

    private static string CloudLine(CloudGameStatus game)
    {
        var watched = game.WatchedFiles is { } w ? S.Format("Status_Watching", w) : S.Get("Status_WatchingUnknown");
        var cloud = game.CloudCn is { } cn
            ? S.Format("Status_CloudState", cn, game.CloudFiles?.ToString() ?? "?")
            : S.Get("Status_CloudUnknown");
        return $"{watched} · {cloud}";
    }

    private static string Relative(DateTime then)
    {
        var minutes = (int)Math.Round((DateTime.Now - then).TotalMinutes);
        if (minutes < 1) return S.Get("Status_JustNow");
        if (minutes < 60) return S.Format("Status_MinutesAgo", minutes);
        var hours = (int)Math.Round(minutes / 60.0);
        if (hours < 24) return S.Format("Status_HoursAgo", hours);
        return S.Format("Status_DaysAgo", (int)Math.Round(hours / 24.0));
    }

    private static void SetTone(Tone tone, SymbolIcon icon)
    {
        (icon.Symbol, icon.Foreground) = tone switch
        {
            Tone.Ok => (SymbolRegular.CheckmarkCircle24, (Brush)Brushes.MediumSeaGreen),
            Tone.Warn => (SymbolRegular.Warning24, Brushes.Orange),
            Tone.Bad => (SymbolRegular.ErrorCircle24, Brushes.IndianRed),
            _ => (SymbolRegular.QuestionCircle24, Brushes.Gray),
        };
    }
}
