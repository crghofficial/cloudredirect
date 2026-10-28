using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using CloudRedirect.Resources;
using CloudRedirect.Services;
using SymbolRegular = Wpf.Ui.Controls.SymbolRegular;

namespace CloudRedirect.Pages;

public partial class SaveRulesPage : Page
{
    private readonly DispatcherTimer _steamTimer = new() { Interval = TimeSpan.FromSeconds(3) };
    private string? _steamPath;
    private InstalledGame? _game;
    private string? _root;
    private string _path = "";
    private List<SaveRule> _existing = new();
    private SaveRule? _own;
    private bool _foreignRules;
    private bool _steamRunning;
    private bool _busy;
    private int _previewVersion;
    private int _suggestVersion;

    public SaveRulesPage()
    {
        InitializeComponent();
        _steamTimer.Tick += (_, _) => UpdateSteamState();
        Loaded += async (_, _) => await LoadAsync();
        Unloaded += (_, _) => _steamTimer.Stop();
    }

    private async Task LoadAsync()
    {
        _steamPath = SteamDetector.FindSteamPath();
        UpdateSteamState();
        _steamTimer.Start();

        if (_steamPath == null)
        {
            Show(S.Get("Status_NoSteam"), error: true);
            UpdateButtons();
            return;
        }

        var steam = _steamPath;
        GameBox.ItemsSource = await Task.Run(() => SaveRulesService.ScanInstalledGames(steam));
        await ReloadBackupsAsync();
        UpdateButtons();
    }

    private void UpdateSteamState()
    {
        _steamRunning = SteamDetector.IsSteamRunning();
        SteamState.Text = S.Get(_steamRunning ? "Rules_SteamOpen" : "Rules_SteamClosed");
        SteamStateIcon.Symbol = _steamRunning ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.Info24;
        SteamStateIcon.Foreground = _steamRunning ? Brushes.MediumSeaGreen : Brushes.Gray;
        UpdateButtons();
    }

    private SaveRule? CurrentRule()
    {
        if (_root == null) return null;
        var pattern = PatternText();
        return pattern.Length == 0 ? null : new SaveRule(_root, _path, pattern, RecursiveBox.IsChecked == true);
    }

    private string PatternText() =>
        ((PatternBox.SelectedItem as ComboBoxItem)?.Content as string ?? PatternBox.Text ?? "").Trim();

    private void UpdateButtons()
    {
        var ready = !_busy && _game != null && CurrentRule() != null;
        BackupButton.IsEnabled = ready;
        SaveButton.IsEnabled = ready && !_foreignRules;
        RestoreButton.IsEnabled = !_busy && BackupBox.SelectedItem != null;
    }

    private void Show(string text, bool error = false)
    {
        Message.Text = text;
        if (error) Message.Foreground = Brushes.IndianRed;
        else Message.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
    }

    private async void GameBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        _game = GameBox.SelectedItem as InstalledGame;
        _root = null;
        _path = "";
        _existing = new();
        _own = null;
        _foreignRules = false;
        FolderText.Text = S.Get("Rules_NoFolder");
        GameInfo.Text = "";
        SuggestBox.ItemsSource = null;
        SuggestInfo.Text = "";
        _suggestVersion++;
        Show("");
        RefreshPreview();
        if (_game == null) { UpdateButtons(); return; }

        var id = _game.AppId;
        try
        {
            _busy = true;
            UpdateButtons();
            var (rules, remembered) = await Task.Run(() => (SaveRulesService.InspectRules(_steamPath!, id), SaveRulesService.ReadRemembered()));
            if (_game?.AppId != id) return;
            _existing = rules;
            if (remembered.TryGetValue(id, out var own))
                _own = own;
            // Rules published by the developer win over ours, unless that is the rule we wrote.
            _foreignRules = rules.Count > 0 && !(_own != null && rules.Any(r => r.SameAs(_own)));
            GameInfo.Text = _foreignRules
                ? S.Format("Rules_ForeignRules", rules.Count)
                : _own != null
                    ? S.Format("Rules_OwnRule", $"{_own.Root} › {(_own.Path.Length == 0 ? "(raiz)" : _own.Path)} › {_own.Pattern}")
                    : S.Get("Rules_NoRules");

            // Editing an existing rule starts from where it points today.
            if (_own != null)
            {
                _root = _own.Root;
                _path = _own.Path;
                PatternBox.Text = _own.Pattern;
                RecursiveBox.IsChecked = _own.Recursive;
                FolderText.Text = FolderDisplay();
            }
        }
        catch (Exception ex)
        {
            Show(ex.Message, error: true);
        }
        finally
        {
            _busy = false;
        }
        RefreshPreview();
        UpdateButtons();
        _ = LoadSuggestionsAsync(_game);
    }

    private async Task LoadSuggestionsAsync(InstalledGame? game)
    {
        if (game == null) return;
        var version = ++_suggestVersion;
        SuggestInfo.Text = S.Get("Rules_SuggestSearching");
        try
        {
            var detection = await SaveManifestService.SuggestAsync(game.AppId, game.InstallDir);
            if (version != _suggestVersion) return;
            SuggestBox.ItemsSource = detection.Suggestions;
            if (detection.Suggestions.Count == 0)
            {
                SuggestInfo.Text = S.Get("Rules_SuggestNone");
                return;
            }
            SuggestInfo.Text = S.Format("Rules_SuggestFound", detection.Suggestions.Count);
            // Do not override a rule already saved for this game, or a folder picked meanwhile.
            if (_root == null) SuggestBox.SelectedIndex = 0;
        }
        catch (Exception ex)
        {
            if (version == _suggestVersion) SuggestInfo.Text = S.Format("Rules_SuggestFailed", ex.Message);
        }
    }

    private void Suggest_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SuggestBox.SelectedItem is not SaveSuggestion suggestion) return;
        _root = suggestion.Root;
        _path = suggestion.Path;
        PatternBox.SelectedItem = null;
        PatternBox.Text = suggestion.Pattern;
        RecursiveBox.IsChecked = suggestion.Recursive;
        FolderText.Text = suggestion.DisplayPath;
        Show("");
        RefreshPreview();
        UpdateButtons();
    }

    private string FolderDisplay() =>
        _root == null ? S.Get("Rules_NoFolder")
        : SaveRulesService.ResolveFolder(new SaveRule(_root, _path, "*", true), _game?.InstallDir ?? "") ?? _path;

    private void Choose_Click(object sender, RoutedEventArgs e)
    {
        if (_game == null) { Show(S.Get("Rules_PickGameFirst"), error: true); return; }
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = S.Format("Rules_ChooseTitle", _game.Name) };
        if (dialog.ShowDialog() != true) return;

        var hit = SaveRulesService.Classify(dialog.FolderName, _game.InstallDir);
        if (hit == null)
        {
            Show(S.Get("Rules_FolderOutside"), error: true);
            return;
        }
        _root = hit.Value.Root;
        _path = hit.Value.Path;
        SuggestBox.SelectedIndex = -1;
        FolderText.Text = dialog.FolderName;
        Show("");
        RefreshPreview();
        UpdateButtons();
    }

    private void Pattern_Changed(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded) return;
        RefreshPreview();
        UpdateButtons();
    }

    private async void RefreshPreview()
    {
        var version = ++_previewVersion;
        var rule = CurrentRule();
        if (rule == null || _game == null)
        {
            RuleSummary.Text = S.Get("Rules_PickToPreview");
            StatsText.Text = "";
            SampleText.Text = "";
            return;
        }
        RuleSummary.Text = $"{rule.Root} › {(rule.Path.Length == 0 ? "(raiz)" : rule.Path)} › {rule.Pattern}"
            + (rule.Recursive ? S.Get("Rules_PlusSubfolders") : "");
        StatsText.Text = S.Get("Rules_Reading");
        SampleText.Text = "";

        var install = _game.InstallDir;
        try
        {
            var stats = await Task.Run(() => SaveRulesService.Stats(rule, install));
            if (version != _previewVersion) return;
            StatsText.Text = stats.FileCount == 0
                ? S.Get("Rules_NoMatch")
                : S.Format("Rules_Stats", stats.FileCount + (stats.Truncated ? "+" : ""), FileUtils.FormatSize(stats.TotalBytes));
            SampleText.Text = string.Join("\n", stats.Sample);
        }
        catch (Exception ex)
        {
            if (version == _previewVersion) StatsText.Text = ex.Message;
        }
    }

    private async void Backup_Click(object sender, RoutedEventArgs e)
    {
        var rule = CurrentRule();
        if (rule == null || _game == null) return;
        var id = _game.AppId;
        var install = _game.InstallDir;
        await RunBusy(async () =>
        {
            var result = await Task.Run(() => SaveRulesService.BackupSaves(id, rule, install));
            Show(S.Format("Rules_BackedUp", result.FileCount, FileUtils.FormatSize(result.TotalBytes), result.Dir));
        });
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        var rule = CurrentRule();
        if (rule == null || _game == null) return;
        var confirmed = await Dialog.ConfirmAsync(S.Get("Rules_Write"),
            S.Format("Rules_ConfirmWrite", _game.Name, $"{rule.Root} › {rule.Path} › {rule.Pattern}"));
        if (!confirmed) return;

        var id = _game.AppId;
        var replacing = _own;
        await RunBusy(async () =>
        {
            await Task.Run(() => SaveRulesService.ApplyRule(id, rule, replacing));
            _own = rule with { Path = rule.Path.Replace('/', '\\') };
            Show(S.Get("Rules_Written"));
            await ReloadBackupsAsync();
        });
    }

    private async void Restore_Click(object sender, RoutedEventArgs e)
    {
        if (BackupBox.SelectedItem is not RuleBackup backup) return;
        if (!await Dialog.ConfirmAsync(S.Get("Rules_Restore"), S.Format("Rules_ConfirmRestore", backup.Label)))
            return;
        await RunBusy(async () =>
        {
            await Task.Run(() => SaveRulesService.RestoreRuleBackup(backup.Name));
            Show(S.Get("Rules_Restored"));
        });
    }

    private async Task ReloadBackupsAsync()
    {
        try
        {
            BackupBox.ItemsSource = await Task.Run(SaveRulesService.ListRuleBackups);
            if (BackupBox.Items.Count > 0) BackupBox.SelectedIndex = 0;
        }
        catch { }
    }

    private async Task RunBusy(Func<Task> work)
    {
        _busy = true;
        UpdateButtons();
        try { await work(); }
        catch (Exception ex) { Show(ex.Message, error: true); }
        finally
        {
            _busy = false;
            UpdateButtons();
        }
    }
}
