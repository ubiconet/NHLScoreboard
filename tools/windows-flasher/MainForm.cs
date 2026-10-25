using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace NhlScoreboardFlasher;

internal sealed class MainForm : Form
{
    private const string ApplicationOffset = "0x10000";
    private readonly ComboBox _ports = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly TextBox _firmware = new() { ReadOnly = true };
    private readonly Button _browse = new() { Text = "Choose firmware..." };
    private readonly Button _refresh = new() { Text = "Refresh ports" };
    private readonly Button _flash = new() { Text = "Flash", Enabled = false };
    private readonly Label _status = new() { AutoSize = true, Text = "Looking for ESP32 serial ports..." };
    private readonly ProgressBar _progress = new() {
        Minimum = 0, Maximum = 100, Value = 0, Dock = DockStyle.Fill,
    };
    private readonly TextBox _log = new() {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
        Font = new Font(FontFamily.GenericMonospace, 9.0f), BackColor = SystemColors.Window,
    };
    private readonly System.Windows.Forms.Timer _scanTimer = new() { Interval = 1500 };
    private static readonly Regex ProgressPercent = new(@"(?<!\d)(\d{1,3})\s*%",
        RegexOptions.Compiled);
    private bool _refreshing;
    private bool _flashing;

    public MainForm()
    {
        Text = "NHL Scoreboard USB Flasher";
        MinimumSize = new Size(680, 430);
        StartPosition = FormStartPosition.CenterScreen;

        var layout = new TableLayoutPanel {
            Dock = DockStyle.Fill, Padding = new Padding(14), ColumnCount = 3, RowCount = 6,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        layout.Controls.Add(new Label { Text = "Detected port:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
        layout.Controls.Add(_ports, 1, 0);
        layout.Controls.Add(_refresh, 2, 0);
        layout.Controls.Add(new Label { Text = "Firmware image:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        layout.Controls.Add(_firmware, 1, 1);
        layout.Controls.Add(_browse, 2, 1);
        layout.Controls.Add(_status, 0, 2);
        layout.SetColumnSpan(_status, 3);
        layout.Controls.Add(_progress, 0, 3);
        layout.SetColumnSpan(_progress, 3);
        layout.Controls.Add(_log, 0, 4);
        layout.SetColumnSpan(_log, 3);
        layout.Controls.Add(_flash, 2, 5);
        Controls.Add(layout);

        _browse.Click += (_, _) => ChooseFirmware();
        _refresh.Click += async (_, _) => await RefreshPortsAsync();
        _flash.Click += async (_, _) => await FlashAsync();
        _ports.SelectedIndexChanged += (_, _) => UpdateFlashButton();
        _scanTimer.Tick += async (_, _) => await RefreshPortsAsync();
        Shown += async (_, _) => {
            _scanTimer.Start();
            await RefreshPortsAsync();
        };
    }

    private void ChooseFirmware()
    {
        using var dialog = new OpenFileDialog {
            Filter = "ESP32 firmware (*.bin)|*.bin",
            Title = "Select an NHL Scoreboard release image",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        _firmware.Text = dialog.FileName;
        var isFactory = IsFactoryImage(dialog.FileName);
        _status.Text = isFactory
            ? "Factory image selected: suitable for a blank or existing board."
            : "Application image selected: requires the NHL Scoreboard partition layout.";
        UpdateFlashButton();
    }

    private async Task RefreshPortsAsync()
    {
        if (_refreshing) return;
        _refreshing = true;
        try {
            var platformio = FindPlatformIo();
            if (platformio is null) {
                _status.Text = "PlatformIO was not found. Install PlatformIO Core, then restart this app.";
                return;
            }

            var result = await RunProcessAsync(platformio, "device", "list", "--json-output");
            if (result.ExitCode != 0) {
                _status.Text = "PlatformIO could not list serial ports.";
                AppendLog(result.Output);
                return;
            }

            var previous = (_ports.SelectedItem as SerialPortItem)?.Port;
            var ports = ParsePorts(result.Output);
            _ports.BeginUpdate();
            _ports.Items.Clear();
            foreach (var port in ports) _ports.Items.Add(port);
            _ports.EndUpdate();
            _ports.SelectedItem = ports.FirstOrDefault(port => port.Port == previous) ?? ports.FirstOrDefault();
            _status.Text = ports.Count == 0
                ? "No serial devices detected. Connect the ESP32-S3, then refresh."
                : $"{ports.Count} serial device(s) detected. Select the ESP32-S3 port.";
        }
        catch (Exception ex) {
            _status.Text = "Port detection failed.";
            AppendLog(ex.Message);
        }
        finally {
            _refreshing = false;
            UpdateFlashButton();
        }
    }

    private async Task FlashAsync()
    {
        if (_ports.SelectedItem is not SerialPortItem port || string.IsNullOrWhiteSpace(_firmware.Text)) return;
        var tools = FindPlatformIoTools();
        if (tools is null) {
            _status.Text = "PlatformIO's Python/esptool installation was not found.";
            return;
        }

        var factory = IsFactoryImage(_firmware.Text);
        var offset = factory ? "0x0" : ApplicationOffset;
        _scanTimer.Stop();
        _flash.Enabled = false;
        _browse.Enabled = false;
        _refresh.Enabled = false;
        _log.Clear();
        _progress.Value = 0;
        _flashing = true;
        _status.Text = $"Flashing {Path.GetFileName(_firmware.Text)} to {port.Port}...";
        try {
            var logProgress = new Progress<string>(HandleFlashOutput);
            var result = await RunProcessAsync(tools.Python, new[] {
                tools.Esptool, "--chip", "esp32s3", "--port", port.Port,
                "--baud", "921600", "--before", "default_reset", "--after", "hard_reset",
                "write_flash", "--flash_mode", "qio", "--flash_freq", "80m",
                "--flash_size", "8MB", offset, _firmware.Text,
            }, logProgress);
            _flashing = false;
            if (result.ExitCode == 0) _progress.Value = 100;
            _status.Text = result.ExitCode == 0
                ? $"Flash complete: {port.Port} reset successfully."
                : $"Flash failed (exit code {result.ExitCode}). See the log below.";
        }
        catch (Exception ex) {
            _flashing = false;
            _status.Text = "Flash could not be started.";
            AppendLog(ex.Message);
        }
        finally {
            _browse.Enabled = true;
            _refresh.Enabled = true;
            _scanTimer.Start();
            UpdateFlashButton();
        }
    }

    private void UpdateFlashButton() =>
        _flash.Enabled = !_refreshing && _ports.SelectedItem is SerialPortItem &&
                         File.Exists(_firmware.Text) && FindPlatformIoTools() is not null;

    private static bool IsFactoryImage(string path) =>
        Path.GetFileNameWithoutExtension(path).EndsWith("_factory", StringComparison.OrdinalIgnoreCase);

    private static string? FindPlatformIo()
    {
        var core = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".platformio");
        var executable = Path.Combine(core, "penv", "Scripts", "platformio.exe");
        return File.Exists(executable) ? executable : null;
    }

    private static PlatformIoTools? FindPlatformIoTools()
    {
        var core = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".platformio");
        var python = Path.Combine(core, "penv", "Scripts", "python.exe");
        var esptool = Path.Combine(core, "packages", "tool-esptoolpy", "esptool.py");
        return File.Exists(python) && File.Exists(esptool) ? new PlatformIoTools(python, esptool) : null;
    }

    private static List<SerialPortItem> ParsePorts(string json)
    {
        using var document = JsonDocument.Parse(json);
        var ports = new List<SerialPortItem>();
        foreach (var device in document.RootElement.EnumerateArray()) {
            if (!device.TryGetProperty("port", out var portProperty)) continue;
            var port = portProperty.GetString();
            if (string.IsNullOrWhiteSpace(port)) continue;
            var description = device.TryGetProperty("description", out var descriptionProperty)
                ? descriptionProperty.GetString() : null;
            var hwid = device.TryGetProperty("hwid", out var hwidProperty) ? hwidProperty.GetString() : null;
            ports.Add(new SerialPortItem(port, description ?? "Serial device", hwid ?? string.Empty));
        }
        return ports.OrderBy(item => item.Port).ToList();
    }

    private void HandleFlashOutput(string line)
    {
        AppendLog(line);
        if (!_flashing) return;
        var match = ProgressPercent.Match(line);
        if (!match.Success || !int.TryParse(match.Groups[1].Value, out var percent) ||
            percent < 0 || percent > 100) {
            return;
        }

        _progress.Value = percent;
        _status.Text = $"Flashing {Path.GetFileName(_firmware.Text)} ({percent}%)...";
    }

    private static Task<ProcessResult> RunProcessAsync(string fileName, params string[] arguments) =>
        RunProcessAsync(fileName, arguments, null);

    private static async Task<ProcessResult> RunProcessAsync(
        string fileName, string[] arguments, IProgress<string>? outputProgress)
    {
        var start = new ProcessStartInfo(fileName) {
            UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new InvalidOperationException($"Could not start {fileName}.");
        var output = Task.WhenAll(
            ReadStreamAsync(process.StandardOutput, outputProgress),
            ReadStreamAsync(process.StandardError, outputProgress));
        await process.WaitForExitAsync();
        var streams = await output;
        return new ProcessResult(process.ExitCode, streams[0] + streams[1]);
    }

    private static async Task<string> ReadStreamAsync(StreamReader reader, IProgress<string>? outputProgress)
    {
        var all = new System.Text.StringBuilder();
        var line = new System.Text.StringBuilder();
        var buffer = new char[256];
        int count;
        while ((count = await reader.ReadAsync(buffer, 0, buffer.Length)) != 0) {
            for (var i = 0; i < count; ++i) {
                var character = buffer[i];
                all.Append(character);
                if (character is '\r' or '\n') {
                    if (line.Length > 0) {
                        outputProgress?.Report(line.ToString());
                        line.Clear();
                    }
                } else {
                    line.Append(character);
                }
            }
        }
        if (line.Length > 0) outputProgress?.Report(line.ToString());
        return all.ToString();
    }

    private void AppendLog(string message)
    {
        if (string.IsNullOrWhiteSpace(message)) return;
        _log.AppendText(message.TrimEnd() + Environment.NewLine);
    }

    private sealed record SerialPortItem(string Port, string Description, string HardwareId)
    {
        public override string ToString() => $"{Port} — {Description} {HardwareId}".Trim();
    }

    private sealed record PlatformIoTools(string Python, string Esptool);
    private sealed record ProcessResult(int ExitCode, string Output);
}
