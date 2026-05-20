# How to Build Dexe

Two outputs to build: **Dexe.dll** (C++) and **DexeUI.exe** (C#).  
All dependencies are already included — no package managers or external downloads needed.

---

## Downloads you need

| What | Link | Notes |
|------|------|-------|
| Visual Studio 2022 Community | https://visualstudio.microsoft.com/downloads/ | Free. Pick the **Desktop development with C++** workload during install |
| WebView2 Runtime | https://developer.microsoft.com/en-us/microsoft-edge/webview2/ | Required to run DexeUI.exe on some PCs |

---

## Step 1 — Build Dexe.dll

All DLL source files are inside the `dll-source/` folder.

1. Open Visual Studio 2022
2. Go to **File → Open → Project/Solution**
3. Navigate into `dll-source/` and open **`Xeno.sln`**
4. Visual Studio will load the pre-configured project — all source files and include paths are already set up
5. Set the build mode to **Release** and **x64** using the two dropdowns in the toolbar
6. Press **Ctrl + Shift + B** (Build Solution)
7. The output file `bin\Dexe.dll` will appear when it succeeds

> **Note:** The project is already configured — it includes `Xeno.cpp`, all Luau compiler sources, zstd, and the resource file. You do not need to add files or set include paths manually.

> If you ever need to start from scratch: create a new **Dynamic-Link Library (DLL)** C++ project in Visual Studio, add all the `.cpp` files from `dll-source/` and `dll-source/deps/` to Solution Explorer, set the Additional Include Directories to `deps` and `deps\Luau`, and build as Release x64. The DLL will appear in `x64\Release\`.

---

## Step 2 — Build DexeUI.exe

1. Open **`DexeUI.cpp`** in Visual Studio 2022
2. Make sure the toolbar shows **Release** and **x64**
3. Press **Ctrl + Shift + B** (Build Solution)
4. The file `DexeUI.exe` will be created when it succeeds

> If Visual Studio asks to restore NuGet packages, let it finish and build again.

---

## Step 3 — Run it

1. Copy `dll-source\bin\Dexe.dll` into the same folder as `DexeUI.exe`
2. Open Roblox first
3. Launch **DexeUI.exe**
4. Dexe will detect Roblox and attach automatically

---

## Folder layout

```text
dll-source/
  Xeno.sln          ← open this to build the DLL
  Xeno.vcxproj
  Xeno.cpp          ← main DLL source
  deps/
    zstd/
      zstd.h
      zstd.c
    Luau/           ← compiler headers
    LuauSrc/        ← compiler sources
    httplib.h
    json.hpp
    base64.hpp
    client.lua
    Xeno.rc
    resource.h

DexeUI.cpp          ← C# UI source (open separately in Visual Studio)
reference/          ← XAML source files and preview images
```
