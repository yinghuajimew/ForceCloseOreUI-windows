<div align="center">

<h1 align="center">ForceCloseOreUI — Windows</h1>

<p>Force Minecraft Bedrock (Windows) to drop OreUI and go back to the classic JSON-UI.</p>

<p>
<b>English</b> · <a href="./README.zh-CN.md">中文</a>
</p>

</div>

<br/>

## What this is

An x64 Windows DLL that hooks Minecraft Bedrock's OreUI config initializer (via Microsoft Detours)
and forces every `OreUIConfig` entry to `false`, making the game fall back to JSON-UI.

Because it hooks at runtime rather than patching files, the same DLL works across many game
versions — it only needs a signature that matches.

## Requirements

- Minecraft: Bedrock Edition for **Windows**, x64
- A way to get a DLL into the game process — **this is the part people usually get stuck on**

## ⚠️ You need an injector

**A DLL on its own does nothing.** Windows Minecraft runs inside the UWP/AppContainer sandbox, and
it will not load a DLL just because the file is sitting in a folder. Something has to load it into
`Minecraft.Windows.exe`.

You have two practical options.

### Option A — use a mod-capable launcher (recommended)

The simplest path, because the launcher handles UWP restrictions for you:

**[QYCottage/mc-w10-version-launcher](https://github.com/QYCottage/mc-w10-version-launcher)**

This launcher runs Minecraft **outside** the Windows app sandbox and loads DLLs from a plain `mods`
folder. It also lets you keep several game versions installed side by side.

1. Install the launcher (needs admin rights and Windows Developer Mode).
2. Download a Minecraft version through it.
3. Drop `ForceCloseOreUI.dll` into the `mods` folder of that version.
4. Launch and play.

### Option B — use a standalone injector

If you already run Minecraft some other way, inject the DLL into `Minecraft.Windows.exe`
after the game has started:

- **[ambiennt/MCBE-DLL-Injector](https://github.com/ambiennt/MCBE-DLL-Injector)** —
  a small open-source injector that targets Minecraft Bedrock by default, and
  automatically grants the DLL the *All Application Packages* permission that UWP apps require.
  If it complains about a missing `VCRUNTIME140_1.dll`, install the
  [x64 VC++ redistributable](https://support.microsoft.com/en-us/help/2977003).

Inject **after** the main menu appears. Injecting during startup can race the game's own
initialization.

> [!NOTE]
> Antivirus software often flags DLL injectors as suspicious — that is the injection technique, not
> this project. Since the source is available, check it before running anything.

## Where the config lives

```
%APPDATA%\Minecraft Bedrock\mods\ForceCloseOreUI\
├── config.json        # what to disable; created on first successful hook
├── signatures.json    # the signature + detour variant that worked (auto-managed)
├── debug.log          # session log
└── crash_native.log   # native crash dump, if one happens
```

If `%APPDATA%` is not usable it falls back to a path relative to the game's working directory:
`mods\ForceCloseOreUI\`. With a sandboxed (Microsoft Store) install the same files also appear
under:

```
C:\Users\[user]\AppData\Local\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe\AC\mods\ForceCloseOreUI
```

## What changed in this build

This fork is a substantial rewrite of the upstream DLL. It is now a single self-contained file
(no helper headers) and the hook installation was reworked.

### 1. Fixed: every hook reported "not called" and was discarded

The old flow installed a hook, waited a **fixed 10 seconds**, and if the target had not been called
it detached and tried the next detour variant. But the OreUI config initializer is often only
invoked when the player reaches a particular screen — which can be well past ten seconds after
launch. Correct hooks were destroyed, and everything looked like "no signature matched".

Now the wait is split in two:

- a short **2.5 s** window for the common case
- then an **observation window** of up to **30 s** that confirms the hook instead of deleting it

A hook is only removed once it is genuinely proven wrong. The whole run is capped by a
**5-minute** global budget.

### 2. Fixed: an empty config map was treated as a failed hook

If the target function ran but `mConfigs` was still empty, the old code counted it as "not called"
and detached. It now registers as a **valid** hook — the function was reached, the config just
hasn't been populated yet and will apply on a later call. Only a clearly malformed map
(`> 2000` entries) is treated as a wrong address.

### 3. Faster scan — anchor prefilter

The pattern engine compared every token at every offset. It now finds the **longest run of fixed
bytes** in the pattern and uses it as an anchor, so most positions are rejected after a single
byte comparison.

### 4. More signatures

The built-in fallback list went from 1 pattern to **9**, covering roughly 2025-10 through 1.26.60:

| Signature | Coverage |
|---|---|
| Relaxed (stack offsets wildcarded) | 1.26.40 ~ 1.26.50 |
| 1.26.60 | matches the relaxed one except the last 4 bytes (`45 89 C6`) |
| 1.26.50 | exact |
| 1.26.20 / 1.26.30 | exact |
| 5 early patterns | 2025-10 ~ 2026-03 |

The 1.26.60 pattern was taken from upstream commit `1fd8606`.

## Building

```sh
xmake f -p windows -m release -y
xmake
# -> build/windows/x64/release/ForceCloseOreUI.dll
```

Needs Visual Studio (MSVC) and xmake. CI (`.github/workflows/build.yml`) builds it on
`windows-latest` and uploads the DLL.

## Credits

Based on the original [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI) by QYCottage
and contributors.
