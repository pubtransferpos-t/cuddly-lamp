> Originally archived by [M-r7z](https://github.com/M-r7z) — rebranded as Dexe

> [!WARNING]
> Dexe is detected by Roblox's anti-cheat (Byfron). Use an alt account if you run it. The author is not liable for bans.

# Dexe

Dexe is a Windows-only Roblox script executor with a C++ DLL backend and a C# WPF frontend.

## Features
- Fast execution
- Multi-instance support
- Supports most scripts, including Lua Armor scripts
- Virtual filesystem support
- HttpSpy
- Real address lookup
- SetGlobal / GetGlobal across clients
- Instance spoofing

## Custom API examples
```lua
local address = Dexe.get_real_address(game:GetService("ScriptContext"))
print("Script context address:", string.format("0x%x", address))

Dexe.spoof_instance(game:GetService("CoreGui"), 0)
Dexe.HttpSpy()

Dexe.SetGlobal("__test", {
    test_text = "hello, world!"
})
local t = Dexe.GetGlobal("__test")
print(t.test_text)

print(Dexe.PID)
print(Dexe.GUID)
```

## Dependencies
All dependencies are bundled in `dll-source/deps/` — no external downloads needed.

- cpp-httplib
- xxHash
- zstd
- Luau compiler
- nlohmann/json
- base64
- Microsoft.Web.WebView2 (auto-restored by Visual Studio)

## Building
See `howtomake.md` for the full build guide, or `howtomake(simple).md` for a beginner-friendly version.

### Quick summary
1. Open `dll-source/Xeno.sln` in Visual Studio 2022 → Release / x64 → Ctrl+Shift+B → outputs `dll-source\bin\Dexe.dll`
2. Open `DexeUI.cpp` in Visual Studio 2022 → Release / x64 → Ctrl+Shift+B → outputs `DexeUI.exe`
3. Copy `Dexe.dll` next to `DexeUI.exe`, open Roblox first, then launch the UI

## Folder layout
```
dll-source/       ← everything needed to build Dexe.dll
  Xeno.sln
  Xeno.vcxproj
  Xeno.cpp
  deps/

DexeUI.cpp        ← C# UI source
reference/        ← XAML source files and preview images
```

## Credits
Thanks to M-r7z, Incognito, TaaprWareV2, and nhisoka for the original work and references.
