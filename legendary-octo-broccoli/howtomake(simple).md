# How to Build Dexe (Simple Guide)

No experience needed. Just follow each step in order.

---

## What you're building

Two files:
- **Dexe.dll** — the core engine (written in C++)
- **DexeUI.exe** — the window you interact with (written in C#)

Both need to be in the same folder to work.

---

## Before you start — things to install (one time only)

### 1. Visual Studio 2022
This is the program that compiles the code.

1. Go to https://visualstudio.microsoft.com/downloads/
2. Download **Visual Studio Community 2022**
3. Run the installer
4. Tick **Desktop development with C++** and install

### 2. WebView2 Runtime
This is needed to run DexeUI.exe on some PCs.

- If DexeUI.exe crashes on launch, install the Evergreen Runtime from https://developer.microsoft.com/en-us/microsoft-edge/webview2/

---

## Step 1 — Get the zstd files

zstd is the only external library you need to add manually.

1. Go to https://github.com/facebook/zstd/releases/latest
2. Download the file that ends in `.tar.gz`
3. Open the archive and go to `build` → `single_file_libs`
4. Copy `zstd.h` and `zstdlib.c` into `deps\zstd\`
5. Rename `zstdlib.c` to `zstd.c`

---

## Step 2 — Build Dexe.dll

1. Open `Dexe.sln` in Visual Studio
2. Set the build mode to **Release** and **x64**
3. Press **Ctrl + Shift + B**
4. Wait for `bin\Dexe.dll` to appear

---

## Step 3 — Build DexeUI.exe

1. Open `DexeUI.cpp` in Visual Studio
2. Set the build mode to **Release** and **x64**
3. Press **Ctrl + Shift + B**
4. `DexeUI.exe` will be created when the build finishes

---

## Step 4 — Run it

1. Copy `bin\Dexe.dll` into the same folder as `DexeUI.exe`
2. Open Roblox first
3. Launch `DexeUI.exe`
4. Dexe will attach automatically

---

## If something breaks

- Missing `zstd.h` → redo Step 1
- Visual Studio workload missing → install Desktop development with C++
- `DexeUI.exe` crashes → install WebView2 Runtime
- Dexe doesn’t attach → make sure Roblox is already open
