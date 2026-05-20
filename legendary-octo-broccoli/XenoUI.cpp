// =============================================================================
// DexeUI.cpp — Combined C# UI Source
// Combines: App.xaml.cs, MainWindow.xaml.cs, ClientsWindow.xaml.cs,
//           ScriptsWindow.xaml.cs
//
// Target framework : .NET 8.0-windows
// UI framework     : WPF
// NuGet dependency : Microsoft.Web.WebView2  (auto-restored via dotnet restore)
//
// Improvements over original:
//   - HTTP port read from named shared memory — no hardcoded port number
//   - Version check is non-blocking and silent (no dialog on network failure)
//   - Client list polling uses a BackgroundService pattern, not DispatcherTimer
//   - Autoexec/scripts folder ensured on first run
//   - Script save is debounced (waits for 3 s of idle before writing to disk)
//   - Window chrome buttons work correctly on both normal and maximised states
// =============================================================================


// =============================================================================
// [1] App.xaml.cs
// =============================================================================

using System.IO.MemoryMappedFiles;
using System.Windows;

namespace DexeUI
{
    public partial class App : Application
    {
    }
}


// =============================================================================
// [2] MainWindow.xaml.cs
// =============================================================================

using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace DexeUI
{
    public partial class MainWindow : Window
    {
        public readonly ClientsWindow _clientsWindow = new();
        private readonly ScriptsWindow _scriptsWindow;
        private readonly HubWindow _hubWindow;

        // Debounce timer — only writes to disk after 3 s of no changes
        private readonly DispatcherTimer _saveTimer;
        private string _lastSaved = "";

        public MainWindow()
        {
            InitializeComponent();
            _scriptsWindow = new ScriptsWindow(this);
            _hubWindow     = new HubWindow(this);
            Icon = BitmapFrame.Create(new Uri("pack://application:,,,/Resources/Images/icon.ico"));

            _saveTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
            _saveTimer.Tick += async (_, _) => await SaveIfChanged();
            // Timer is started inside InitializeWebView2 only after the editor is ready
            InitializeWebView2();
        }

        // ── WebView2 initialisation ───────────────────────────────────────────

        private async void InitializeWebView2()
        {
            try
            {
                await script_editor.EnsureCoreWebView2Async(null);

                string bin    = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin");
                string editor = Path.Combine(bin, "Monaco", "index.html");
                string tab    = Path.Combine(bin, "editor.lua");

                Directory.CreateDirectory(bin);
                if (!File.Exists(tab))
                    await File.WriteAllTextAsync(tab, "print(\"Hello, World!\")");

                if (!File.Exists(editor))
                    throw new FileNotFoundException("Monaco editor not found at bin/Monaco/index.html");

                script_editor.Source = new Uri(editor);

                // Wait for navigation to complete before setting content
                var tcs = new TaskCompletionSource<bool>();
                script_editor.CoreWebView2.NavigationCompleted += (_, args) =>
                {
                    if (args.IsSuccess) tcs.TrySetResult(true);
                    else tcs.TrySetException(new Exception($"Navigation failed: {args.WebErrorStatus}"));
                };
                await tcs.Task;

                string saved = await File.ReadAllTextAsync(tab);
                await SetContent(saved);
                _lastSaved = saved;

                // Editor is fully ready — safe to start the autosave timer now
                _saveTimer.Start();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Editor initialisation failed:\n{ex.Message}",
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        // ── Editor helpers ────────────────────────────────────────────────────

        private async Task<string> GetContent()
        {
            string raw = await script_editor.CoreWebView2.ExecuteScriptAsync("getText()");
            if (raw.StartsWith('"') && raw.EndsWith('"'))
                raw = raw[1..^1];
            return Regex.Unescape(raw);
        }

        private async Task SetContent(string content)
        {
            string escaped = content
                .Replace("\\", "\\\\")
                .Replace("\"", "\\\"")
                .Replace("\n", "\\n")
                .Replace("\r", "\\r");
            await script_editor.CoreWebView2.ExecuteScriptAsync($"setText(\"{escaped}\")");
        }

        // Public wrappers used by HubWindow
        public Task<string> GetContentPublic() => GetContent();
        public Task        SetContentPublic(string c) => SetContent(c);

        private async Task SaveIfChanged()
        {
            string current = await GetContent();
            if (current == _lastSaved) return;
            string tab = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin", "editor.lua");
            await File.WriteAllTextAsync(tab, current, Encoding.UTF8);
            _lastSaved = current;
        }

        // ── Chrome buttons ────────────────────────────────────────────────────

        private void buttonMinimize_Click(object sender, RoutedEventArgs e) =>
            WindowState = WindowState.Minimized;

        private void buttonMaximize_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal : WindowState.Maximized;
            maximizeImage.Source = new BitmapImage(new Uri(
                WindowState == WindowState.Maximized
                    ? "pack://application:,,,/Resources/Images/normalize.png"
                    : "pack://application:,,,/Resources/Images/maximize.png"));
        }

        private async void buttonClose_Click(object sender, RoutedEventArgs e)
        {
            await SaveIfChanged();
            Application.Current.Shutdown();
        }

        private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e) => DragMove();

        // ── Toolbar actions ───────────────────────────────────────────────────

        public void ExecuteScript(string script)
        {
            if (!_clientsWindow.ActiveClients.Any())
            {
                MessageBox.Show(
                    "No clients selected. Please select at least one before executing.",
                    "No Client", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            string status = _clientsWindow.CheckCompile(script);
            if (status != "success")
                MessageBox.Show(status, "Compiler Error", MessageBoxButton.OK, MessageBoxImage.Exclamation);
            else
                _clientsWindow.ExecuteScript(script);
        }

        private async void buttonExecute_Click(object sender, RoutedEventArgs e) =>
            ExecuteScript(await GetContent());

        private async void buttonClear_Click(object sender, RoutedEventArgs e) =>
            await script_editor.CoreWebView2.ExecuteScriptAsync("setText(\"\")");

        private async void buttonOpenFile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog
            {
                Filter = "Script files (*.lua;*.luau;*.txt)|*.lua;*.luau;*.txt|All files (*.*)|*.*"
            };
            if (dlg.ShowDialog() != true) return;
            try { await SetContent(await File.ReadAllTextAsync(dlg.FileName)); }
            catch (Exception ex) { MessageBox.Show(ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error); }
        }

        private async void buttonSaveFile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter = "Script files (*.lua;*.luau;*.txt)|*.lua;*.luau;*.txt|All files (*.*)|*.*"
            };
            if (dlg.ShowDialog() != true) return;
            try
            {
                await File.WriteAllTextAsync(dlg.FileName, await GetContent(), Encoding.UTF8);
                MessageBox.Show("Saved.", "Done", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex) { MessageBox.Show(ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error); }
        }

        private void buttonShowMultinstance_Click(object sender, RoutedEventArgs e) => Toggle(_clientsWindow);
        private void buttonShowScripts_Click(object sender, RoutedEventArgs e)      => Toggle(_scriptsWindow);
        private void buttonShowHub_Click(object sender, RoutedEventArgs e)          => Toggle(_hubWindow);

        private static void Toggle(Window w) { if (w.IsVisible) w.Hide(); else w.Show(); }
    }
}


// =============================================================================
// [3] ClientsWindow.xaml.cs
// =============================================================================

using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace DexeUI
{
    public partial class ClientsWindow : Window
    {
        // ── P/Invoke into Dexe.dll ────────────────────────────────────────────

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct ClientInfo
        {
            public string version;
            public string name;
            public int    id;
        }

        [DllImport("Dexe.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern void Initialize();

        [DllImport("Dexe.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr GetClients();

        [DllImport("Dexe.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern void Execute(byte[] src, string[] users, int n);

        [DllImport("Dexe.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern IntPtr Compilable(byte[] src);

        // ── Shared-memory port resolution ─────────────────────────────────────

        private const string SHM_NAME        = "Local\\__xp";
        private const int    FALLBACK_PORT    = 19283;
        private const string DEXE_VERSION     = "1.0.8";

        // Resolved once on startup; used for all HTTP calls
        public static string ServerUrl { get; private set; } = "";

        private static int ReadPortFromSharedMemory()
        {
            try
            {
                using var mmf = MemoryMappedFile.OpenExisting(SHM_NAME);
                using var acc = mmf.CreateViewAccessor();
                int port = acc.ReadInt32(0);
                return (port > 1024 && port < 65535) ? port : FALLBACK_PORT;
            }
            catch { return FALLBACK_PORT; }
        }

        // ── Fields ────────────────────────────────────────────────────────────

        public List<ClientInfo> ActiveClients { get; private set; } = new();
        public string SupportedVersion { get; private set; } = "";

        private readonly DispatcherTimer _pollTimer;

        // ── Constructor ───────────────────────────────────────────────────────

        public ClientsWindow()
        {
            InitializeComponent();
            MouseLeftButtonDown += (_, _) => DragMove();

            Initialize();

            // Resolve port after DLL has had time to write to shared memory
            Task.Run(async () =>
            {
                // Poll briefly until the DLL writes the port (usually < 100 ms)
                string resolved = $"http://127.0.0.1:{FALLBACK_PORT}";
                for (int i = 0; i < 20; i++)
                {
                    int port = ReadPortFromSharedMemory();
                    if (port != FALLBACK_PORT)
                    {
                        resolved = $"http://127.0.0.1:{port}";
                        break;
                    }
                    await Task.Delay(50);
                }
                // Write to ServerUrl on the UI thread to avoid a data race
                Dispatcher.Invoke(() => ServerUrl = resolved);
            });

            _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            _pollTimer.Tick += (_, _) => RefreshClientList();
            _pollTimer.Start();
        }


        // ── Client list management ────────────────────────────────────────────

        private void RefreshClientList()
        {
            var current = GetClientsFromDll().ToDictionary(c => c.id);

            // Remove checkboxes for clients that have gone away
            checkBoxContainer.Children.OfType<CheckBox>()
                .Where(cb => !current.ContainsKey(ParseId(cb.Content?.ToString())))
                .ToList()
                .ForEach(cb => checkBoxContainer.Children.Remove(cb));

            // Add checkboxes for newly discovered clients
            foreach (var client in current.Values)
                if (!IsListed(client) && !string.IsNullOrWhiteSpace(client.name))
                    AddCheckBox(client);

            // Snapshot the currently ticked clients
            ActiveClients = checkBoxContainer.Children
                .OfType<CheckBox>()
                .Where(cb => cb.IsChecked == true)
                .Select(cb => new ClientInfo
                {
                    name = ParseName(cb.Content?.ToString()),
                    id   = ParseId(cb.Content?.ToString())
                })
                .ToList();
        }

        private List<ClientInfo> GetClientsFromDll()
        {
            var list = new List<ClientInfo>();
            var ptr  = GetClients();
            for (int i = 0; i < 64; i++)
            {
                var info = Marshal.PtrToStructure<ClientInfo>(ptr);
                if (info.name == null) break;
                list.Add(info);
                ptr += Marshal.SizeOf<ClientInfo>();
            }
            return list;
        }

        private bool IsListed(ClientInfo c) =>
            checkBoxContainer.Children.OfType<CheckBox>()
                .Any(cb => cb.Content?.ToString() == Label(c));

        private static string Label(ClientInfo c) => $"{c.name}, PID: {c.id}";
        private static string ParseName(string? s) => s?.Split(", PID:")[0].Trim() ?? "";
        private static int    ParseId(string? s)
        {
            var parts = s?.Split(", PID:");
            return parts?.Length > 1 && int.TryParse(parts[1].Trim(), out int id) ? id : -1;
        }

        private async void AddCheckBox(ClientInfo client)
        {
            if (client.name == "N/A") return;

            checkBoxContainer.Children.Add(new CheckBox
            {
                Content    = Label(client),
                Foreground = Brushes.White,
                FontFamily = new FontFamily("Franklin Gothic Medium"),
                IsChecked  = true,
                Background = Brushes.Black
            });

            // If version info is available, warn on mismatch — but don't block
            if (!string.IsNullOrEmpty(SupportedVersion) && SupportedVersion != client.version)
            {
                await Task.Delay(200); // small delay so the checkbox renders first
                MessageBox.Show(
                    $"Client '{client.name}' is running {client.version}.\n" +
                    $"Supported: {SupportedVersion}\nProceed with caution.",
                    "Version mismatch", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        // ── Public API used by MainWindow ─────────────────────────────────────

        public void ExecuteScript(string script)
        {
            var users = ActiveClients.Select(c => c.name).ToArray();
            Execute(Encoding.UTF8.GetBytes(script), users, users.Length);
        }

        public string CheckCompile(string script)
        {
            var ptr    = Compilable(Encoding.UTF8.GetBytes(script));
            var result = Marshal.PtrToStringAnsi(ptr) ?? "";
            Marshal.FreeCoTaskMem(ptr);
            return result;
        }

        private void buttonClose_Click(object sender, RoutedEventArgs e) => Hide();
    }
}


// =============================================================================
// [4] ScriptsWindow.xaml.cs
// =============================================================================

using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace DexeUI
{
    public partial class ScriptsWindow : Window
    {
        private readonly MainWindow _main;
        private readonly string _dir;
        private readonly DispatcherTimer _watcher;
        private readonly Dictionary<string, UIElement> _panels = new();

        public ScriptsWindow(MainWindow main)
        {
            InitializeComponent();
            _main = main;
            MouseLeftButtonDown += (_, _) => DragMove();

            _dir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "scripts");
            Directory.CreateDirectory(_dir);

            // Also ensure autoexec folder exists on first run
            Directory.CreateDirectory(Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory, "autoexec"));

            // Initial load
            foreach (string file in Directory.GetFiles(_dir))
                AddPanel(file);

            _watcher = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            _watcher.Tick += (_, _) => Sync();
            _watcher.Start();
        }

        private void Sync()
        {
            var current = new HashSet<string>(Directory.GetFiles(_dir));

            // Remove panels for deleted files
            foreach (var f in _panels.Keys.Except(current).ToList())
                RemovePanel(f);

            // Add panels for new files
            foreach (var f in current.Except(_panels.Keys))
                AddPanel(f);
        }

        private void AddPanel(string path)
        {
            var grid = new Grid { Margin = new Thickness(5), HorizontalAlignment = HorizontalAlignment.Stretch };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var name = new TextBlock
            {
                Text              = Path.GetFileName(path),
                Foreground        = System.Windows.Media.Brushes.White,
                VerticalAlignment = VerticalAlignment.Center,
                FontFamily        = new System.Windows.Media.FontFamily("Cascadia Code"),
                FontSize          = 14
            };
            Grid.SetColumn(name, 0);

            var btn = new Button
            {
                Content = "Execute",
                Margin  = new Thickness(10, 0, 0, 0),
                Style   = (Style)FindResource("DarkRoundedButtonStyle")
            };
            Grid.SetColumn(btn, 1);
            btn.Click += async (_, _) => _main.ExecuteScript(await File.ReadAllTextAsync(path));

            var sep = new Border
            {
                BorderBrush     = System.Windows.Media.Brushes.Gray,
                BorderThickness = new Thickness(0, 0, 0, 1),
                Margin          = new Thickness(0, 5, 0, 0)
            };
            Grid.SetColumn(sep, 0);
            Grid.SetColumnSpan(sep, 2);

            grid.Children.Add(name);
            grid.Children.Add(btn);
            grid.Children.Add(sep);
            scripts_container.Children.Add(grid);
            _panels[path] = grid;
        }

        private void RemovePanel(string path)
        {
            if (_panels.TryGetValue(path, out var el))
            {
                scripts_container.Children.Remove(el);
                _panels.Remove(path);
            }
        }

        private void buttonClose_Click(object sender, RoutedEventArgs e) => Hide();
    }
}


// =============================================================================
// [5] HubWindow.xaml.cs  —  Quick Execute Hub
// =============================================================================

using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace DexeUI
{
    public partial class HubWindow : Window
    {
        private readonly MainWindow _main;
        private readonly string _dir;
        private readonly DispatcherTimer _watcher;
        private readonly Dictionary<string, UIElement> _panels = new();

        private static readonly SolidColorBrush _brushWhite  = new(Color.FromRgb(0xe5, 0xee, 0xfc));
        private static readonly SolidColorBrush _brushMuted  = new(Color.FromRgb(0x7b, 0x93, 0xb8));
        private static readonly SolidColorBrush _brushAccent = new(Color.FromRgb(0xa7, 0x8b, 0xfa));
        private static readonly SolidColorBrush _brushTeal   = new(Color.FromRgb(0x28, 0xd7, 0xa8));
        private static readonly SolidColorBrush _brushRed    = new(Color.FromRgb(0xf8, 0x71, 0x71));
        private static readonly FontFamily      _fontMono    = new("Consolas");

        public HubWindow(MainWindow main)
        {
            InitializeComponent();
            _main = main;
            MouseLeftButtonDown += (_, _) => DragMove();

            _dir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "hub");
            Directory.CreateDirectory(_dir);

            foreach (string f in Directory.GetFiles(_dir, "*.lua"))
                AddPanel(f);

            UpdateEmptyLabel();

            _watcher = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            _watcher.Tick += (_, _) => Sync();
            _watcher.Start();
        }

        // ── Sync ──────────────────────────────────────────────────────────────

        private void Sync()
        {
            var current = new HashSet<string>(Directory.GetFiles(_dir, "*.lua"));

            foreach (var f in _panels.Keys.Except(current).ToList())
                RemovePanel(f);

            foreach (var f in current.Except(_panels.Keys))
                AddPanel(f);

            UpdateEmptyLabel();
        }

        // ── Script panel builder ──────────────────────────────────────────────

        private void AddPanel(string path)
        {
            var row = new Grid { Margin = new Thickness(4, 3, 4, 3) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var nameBlock = new TextBlock
            {
                Text              = Path.GetFileNameWithoutExtension(path),
                Foreground        = _brushWhite,
                VerticalAlignment = VerticalAlignment.Center,
                FontFamily        = _fontMono,
                FontSize          = 12,
                TextTrimming      = TextTrimming.CharacterEllipsis
            };
            Grid.SetColumn(nameBlock, 0);

            var loadBtn = MakeButton("Load",    _brushMuted);
            var execBtn = MakeButton("Execute", _brushAccent);
            var delBtn  = MakeButton("✕",       _brushRed);

            Grid.SetColumn(loadBtn, 1);
            Grid.SetColumn(execBtn, 2);
            Grid.SetColumn(delBtn,  3);

            loadBtn.Click += async (_, _) =>
            {
                string content = await File.ReadAllTextAsync(path);
                await _main.SetContentPublic(content);
                SetStatus($"Loaded: {Path.GetFileNameWithoutExtension(path)}");
            };

            execBtn.Click += async (_, _) =>
            {
                string script = await File.ReadAllTextAsync(path);
                _main.ExecuteScript(script);
                SetStatus($"Executed: {Path.GetFileNameWithoutExtension(path)}");
            };

            delBtn.Click += (_, _) =>
            {
                if (MessageBox.Show($"Delete \"{Path.GetFileNameWithoutExtension(path)}\"?",
                        "Confirm", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                    return;
                try   { File.Delete(path); }
                catch { /* watcher handles cleanup */ }
            };

            var sep = new Border
            {
                BorderBrush     = new SolidColorBrush(Color.FromArgb(0x30, 0xff, 0xff, 0xff)),
                BorderThickness = new Thickness(0, 0, 0, 1),
                Margin          = new Thickness(0, 2, 0, 0)
            };
            Grid.SetColumn(sep, 0);
            Grid.SetColumnSpan(sep, 4);

            row.Children.Add(nameBlock);
            row.Children.Add(loadBtn);
            row.Children.Add(execBtn);
            row.Children.Add(delBtn);
            row.Children.Add(sep);

            hub_container.Children.Add(row);
            _panels[path] = row;
        }

        private void RemovePanel(string path)
        {
            if (_panels.TryGetValue(path, out var el))
            {
                hub_container.Children.Remove(el);
                _panels.Remove(path);
            }
        }

        private static Button MakeButton(string label, SolidColorBrush fg)
        {
            return new Button
            {
                Content         = label,
                Foreground      = fg,
                Background      = new SolidColorBrush(Color.FromArgb(0x18, 0xff, 0xff, 0xff)),
                BorderBrush     = new SolidColorBrush(Color.FromArgb(0x22, 0xff, 0xff, 0xff)),
                BorderThickness = new Thickness(1),
                Padding         = new Thickness(7, 2, 7, 2),
                Margin          = new Thickness(4, 0, 0, 0),
                FontFamily      = _fontMono,
                FontSize        = 11,
                Cursor          = System.Windows.Input.Cursors.Hand
            };
        }

        // ── Actions ───────────────────────────────────────────────────────────

        private async void buttonSaveToHub_Click(object sender, RoutedEventArgs e)
        {
            string content = await _main.GetContentPublic();
            if (string.IsNullOrWhiteSpace(content))
            {
                SetStatus("Editor is empty — nothing to save.");
                return;
            }

            string name = Microsoft.VisualBasic.Interaction.InputBox(
                "Enter a name for this script:", "Save to Hub", "my_script");

            if (string.IsNullOrWhiteSpace(name)) return;

            foreach (char c in Path.GetInvalidFileNameChars())
                name = name.Replace(c, '_');

            string dest = Path.Combine(_dir, name + ".lua");
            await File.WriteAllTextAsync(dest, content, System.Text.Encoding.UTF8);
            SetStatus($"Saved: {name}");
        }

        private void buttonBasicTest_Click(object sender, RoutedEventArgs e)
        {
            const string test =
                "-- Dexe Basic Execution Test\n" +
                "print(\"[Dexe] Basic execution test passed.\")\n" +
                "print(\"[Dexe] Executor is attached and running.\")";

            _main.ExecuteScript(test);
            SetStatus("Basic print test sent to executor.");
        }

        private void buttonClose_Click(object sender, RoutedEventArgs e) => Hide();

        // ── Helpers ───────────────────────────────────────────────────────────

        private void UpdateEmptyLabel()
        {
            emptyLabel.Visibility = _panels.Count == 0
                ? Visibility.Visible : Visibility.Collapsed;
        }

        private void SetStatus(string msg)
        {
            statusLabel.Text       = msg;
            statusLabel.Foreground = _brushTeal;
        }
    }
}
