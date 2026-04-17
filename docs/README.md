# PowerMeter — User Guide

> **Version 1.1** | Windows 10 / 11 (64-bit) | Sierra Chart compatible

---

## Table of Contents

1. [What Is PowerMeter?](#1-what-is-powermeter)
2. [System Requirements](#2-system-requirements)
3. [What's in the Package](#3-whats-in-the-package)
4. [Quick Start (5 minutes)](#4-quick-start-5-minutes)
5. [Installing the Overlay (PowerMeter.exe)](#5-installing-the-overlay-powermeterexe)
6. [Installing the ACSIL Study in Sierra Chart](#6-installing-the-acsil-study-in-sierra-chart)
7. [Configuring the Study](#7-configuring-the-study)
8. [Reading the Overlay](#8-reading-the-overlay)
9. [Choosing Between the Two Study Variants](#9-choosing-between-the-two-study-variants)
10. [Study Parameter Reference](#10-study-parameter-reference)
11. [Rebuilding the DLLs from Source](#11-rebuilding-the-dlls-from-source)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. What Is PowerMeter?

PowerMeter is a two-component real-time market analysis tool for Sierra Chart:

| Component | What it does |
|---|---|
| **PowerMeter.exe** | Always-on-top Win32 overlay that displays three live bar-meters |
| **ACSIL Study** (DLL) | Sierra Chart study that reads live execution and DOM data and pushes it to the overlay via Windows shared memory |

The overlay shows three columns of data, each with a **red** (bearish / sell-side) bar and a **blue** (bullish / buy-side) bar. The relative size of the bars indicates directional pressure in the market at that moment.

### Vertical layout (default)

```
+------------------------------+
|  [R] [<>]           [pin][X] |  <- Reset / Orient / Pin / Close
|                              |
|  TBV   ASK   Bear P/S        |  <- top labels
|  ####  ####  ####            |  red  (sell-aggressor / bearish)
|  --------------------------  |  <- 50% midline (yellow, spans all columns)
|  ####  ####  ####            |  blue (buy-aggressor / bullish)
|  TAV   BID   Bull P/S        |  <- bottom labels
+------------------------------+
```

### Horizontal layout (press the green orient button to toggle)

```
+--------------------------------------------------------+
|  [R] [<>]                                   [pin] [X] |
|                         TBV                            |
|  #################### | ############################  |
|                         TAV                            |
|                         ASK                            |
|  ################################ | ###############    |
|                         BID                            |
|                      Bear P/S                          |
|  ##################################################    |
|                      Bull P/S                          |
+--------------------------------------------------------+
```

The vertical yellow line at the horizontal 50% centre spans all three rows in horizontal mode.
Each bar is the same thickness in both orientations.

> **No SierraChart connection?** The overlay shows animated demo data automatically and switches to live data as soon as the study is loaded.

---

## 2. System Requirements

| Item | Requirement |
|---|---|
| OS | Windows 10 or Windows 11 (64-bit) |
| Sierra Chart | Any recent version with Market Depth (DOM) data enabled |
| Sierra Chart data feed | Any feed that provides Level 2 / DOM data (CME, Rithmic, Tradovate, etc.) |
| GPU / display | Direct2D-capable GPU (any GPU from 2010 onward) |
| .NET / runtimes | **None** — PowerMeter.exe is a native Win32 app with no runtime dependencies |

---

## 3. What's in the Package

```
PowerMeter_v1.1/
+-- PowerMeter.exe              <- The overlay application (just run it)
+-- README.md                   <- This file
|
+-- ACSIL/
|   +-- PowerMeterFeed_64.dll   <- Pre-built ACSIL study (custom algorithm)
|   +-- PowerMeterFeedJS_64.dll <- Pre-built ACSIL study (Jigsaw-style)
|   +-- build_acsil.ps1         <- Script to rebuild DLLs if needed
|   +-- src/
|       +-- PowerMeterFeed.cpp
|       +-- PowerMeterFeedJS.cpp
|
+-- docs/
    +-- README.md               <- This file
    +-- ALGORITHM_NOTES.md      <- Technical description of both algorithms
```

---

## 4. Quick Start (5 minutes)

1. **Run the overlay** — Double-click `PowerMeter.exe`. A narrow dark panel appears. Drag it to a corner of your screen.
2. **Copy the DLL** — Copy `ACSIL\PowerMeterFeed_64.dll` (or `PowerMeterFeedJS_64.dll`) to `C:\SierraChart\Data\`.
3. **Add the study in Sierra Chart** — Open a chart with DOM data. Go to **Analysis → Add Custom Study**, search for **"PowerMeter Feed"**, and click OK.
4. **Watch the bars move** — The overlay connects automatically within 1–2 seconds and starts showing live data.

---

## 5. Installing the Overlay (PowerMeter.exe)

### Placement
`PowerMeter.exe` is fully portable — no installer is required. Place it wherever is convenient, for example:

- `C:\PowerMeter\PowerMeter.exe`
- `%USERPROFILE%\Desktop\PowerMeter.exe`

### Starting with Windows
To have it launch automatically:

1. Press **Win + R**, type `shell:startup`, press **Enter**.
2. Create a shortcut to `PowerMeter.exe` in the Startup folder.

### Window controls

| Control | Action |
|---|---|
| **R** button (top-left, red) | Resets Col 0 accumulator to zero with full rebase — persistent across live updates |
| **Orient** button (top-left, green) | Toggles between vertical (220 x 760) and horizontal stacked (700 x 350) layout |
| **Pin** button (top-right, turns gold) | Toggles always-on-top mode |
| **X** button (top-right, red) | Closes the overlay |
| **Drag the top strip** | Moves the window anywhere on screen |

Window position, pin state, and orientation are saved to the registry (`HKCU\Software\PowerMeter`) and restored on next launch.

---

## 6. Installing the ACSIL Study in Sierra Chart

### Step 1 — Copy the DLL

Choose **one** variant (see [Section 9](#9-choosing-between-the-two-study-variants)):

| Variant | DLL file | Description |
|---|---|---|
| Custom algorithm | `PowerMeterFeed_64.dll` | Full-featured: decay, persistence, burst detection, continuation |
| Jigsaw-style | `PowerMeterFeedJS_64.dll` | Plain pull/stack sums — simpler, more direct |

Copy the chosen DLL to:

```
C:\SierraChart\Data\
```

> If Sierra Chart is installed in a different drive/folder, copy the DLL to the `Data` subfolder inside **your** Sierra Chart installation directory (e.g., `D:\SierraChart\Data\`).

> **Running both at the same time is not supported.** Both studies write to the same shared memory object (`Local\PowerMeterLiveData`). Only load one.

### Step 2 — Enable Market Depth in Sierra Chart

The study requires Level 2 / DOM data:

1. Open the chart you want to analyse (e.g., `ESH25 - CME Globex`).
2. **Right-click the chart → Chart Settings → Market Depth tab**.
3. Set **Number of Bid Levels** and **Number of Ask Levels** to at least **12** (20 recommended).
4. Click OK.

### Step 3 — Add the Study

1. In Sierra Chart menu: **Analysis → Add Custom Study**.
2. In the search box type **"PowerMeter"**.
3. Select **"PowerMeter Feed"** (or "PowerMeter Feed JS") and click **Add**.
4. The study appears in the Studies list. Click **OK** to apply.
5. Because `sc.AutoLoop = 0` and `sc.UpdateAlways = 1`, the study fires on every DOM update — no bar close is needed.

### Step 4 — Verify the Connection

Enable the **Debug Log** input (set to **Yes**) temporarily. In the Sierra Chart **Message Log** (Analysis → Message Log) you should see lines like:

```
PM | ExecAsk=1420 ExecBid=980 | BearPS=142.3 BullPS=188.5 | AskDOM=2340 BidDOM=3100 | IPC=OK
```

`IPC=OK` confirms the overlay has connected. Turn **Debug Log** back to **No** for live trading.

---

## 7. Configuring the Study

Open the study settings (click the study in the Studies list → Settings):

### Recommended starting configuration

| Input | Recommended value | Why |
|---|---|---|
| DOM Levels Per Side Cap | 10 | Limits how deep the DOM scan goes |
| Include Best Bid/Ask Level | Yes | Level 0 (best bid/ask) is usually the most informative |
| Depth Levels (P/S) | 6 | How many DOM levels to scan for pulls and stacks |
| Snapshot Levels (DOM) | 12 | DOM levels included in the total volume bars |
| Stack Persistence (ms) | 900 | How long a stack must hold before its weight ramps up |
| Pull Burst Window (ms) | 900 | Window to detect rapid successive pulls |
| Debug Log | No | Keep off during live trading |

> For the JS variant only `DOM Levels Per Side Cap`, `Include Best Bid/Ask Level`, `Depth Levels (P/S)`, `Snapshot Levels (DOM)`, and `Debug Log` are available.

---

## 8. Reading the Overlay

### Column layout (left to right)

```
Col 0  Col 2  Col 1
 TBV    ASK    Bear P/S
 ---    ---    ---     <- 50% mid-line (yellow)
 TAV    BID    Bull P/S
```

| Column | Red | Blue | What it shows |
|---|---|---|---|
| **Col 0** | TBV — Traded Bid Volume | TAV — Traded Ask Volume | Executed trade volume split by aggressor side, accumulated since last reset. **Red = sell-aggressor trades** (hitting the bid). **Blue = buy-aggressor trades** (lifting the ask). Source: `sc.BidVolume` / `sc.AskVolume` (bar-level execution arrays, same as footprint chart). |
| **Col 1** | Bear P/S | Bull P/S | Composite Pull/Stack score. Red dominating = bearish DOM pressure (bid pulls + ask stacks). Blue dominating = bullish DOM pressure (ask pulls + bid stacks). |
| **Col 2** | ASK DOM total | BID DOM total | Raw resting volume on the ask vs bid side across the configured snapshot levels. A much larger BID total suggests a defensive buy wall; larger ASK total suggests supply overhead. |

### Interpreting signals

- **Red > Blue across all columns**: concentrated selling pressure — potential downside.
- **Blue > Red across all columns**: concentrated buying pressure — potential upside.
- **Col 0 and Col 1 agree, Col 2 opposite**: order flow fighting a passive wall — watch for absorption or sweep.
- **Bars close to 50/50**: balanced market, low conviction either way.

### The Reset button and Col 0 persistence

The **R** button performs a persistent reset for Col 0:

- The local accumulator (Traded Bid / Traded Ask counts) is zeroed.
- The previous-raw-value anchor is rebased to the current live reading so the very next tick produces a delta of zero.
- Cols 1 and 2 are also zeroed visually on reset.

This means the reset **holds** — it does not snap back to the session total on the next timer tick.

### Demo mode
When no live data is available (PowerMeter.exe started before the SC study, or after Sierra Chart closes), the bars animate randomly. This confirms the overlay is running.

---

## 9. Choosing Between the Two Study Variants

| Feature | PowerMeterFeed (Custom) | PowerMeterFeedJS (Jigsaw-style) |
|---|---|---|
| Column 0 (Executed Vol) | Same | Same |
| Column 1 P/S — exponential depth decay | Configurable | Flat (all levels equal weight) |
| Column 1 P/S — pull weight / stack weight | Separate sliders | Fixed at 1.0 |
| Column 1 P/S — stack persistence ramp | Yes | No |
| Column 1 P/S — pull burst detection | Yes | No |
| Column 1 P/S — continuation confirmation | Yes | No |
| Column 1 P/S — minimum thresholds | Yes | No |
| Column 2 (DOM totals) | Same | Same |
| Number of configurable inputs | 18 | 5 |

**Use `PowerMeterFeedJS`** if you:
- Are new to DOM analysis and want a clean, unfiltered signal.
- Prefer results closest to how Jigsaw Depth & Sales reads DOM pull/stack data.
- Want fewer settings to tune.

**Use `PowerMeterFeed`** if you:
- Want signal filtering to reduce noise from tiny pull events.
- Want to give more weight to stacks that persist over time.
- Want burst detection to flag rapid successive DOM pulls.
- Are comfortable tuning the decay, weight, and timing parameters.

---

## 10. Study Parameter Reference

### PowerMeterFeed inputs

| # | Input name | Type | Default | Description |
|---|---|---|---|---|
| 0 | DOM Levels Per Side Cap | Int | 10 | Global cap applied to all level counts. 0 = no cap. |
| 1 | Include Best Bid/Ask Level | Yes/No | Yes | Whether level 0 (best bid/ask) is included in DOM scans. |
| 2 | Pull Sum Decay Constant k | Float | 0.10 | Exponential weight `exp(-k x level_offset)`. 0 = flat. Higher k = inner levels dominate. |
| 3 | Debug Log | Yes/No | No | Writes a line to SC Message Log on every tick. Enable briefly to verify IPC=OK. |
| 4 | Pull Weight | Float | 1.1 | Multiplier applied to all pull contributions in Col 1. |
| 5 | Stack Weight | Float | 1.0 | Base multiplier applied to stack contributions before persistence ramp. |
| 6 | Depth Levels (P/S) | Int | 6 | How many DOM levels (per side) are scanned for P/S calculation. |
| 7 | Snapshot Levels (DOM) | Int | 12 | How many DOM levels are summed for Col 2 total volumes. |
| 8 | Stack Persistence (ms) | Int | 900 | Time (ms) a stack must hold before weight ramps toward Persistent Weight. |
| 9 | Stack Weight When Persistent | Float | 1.6 | Final weight after full persistence window. Ramps linearly from Stack Weight. |
| 10 | Pull Burst Window (ms) | Int | 900 | Time window to count pull events per DOM level for burst detection. |
| 11 | Pull Burst Count Threshold | Int | 3 | Number of pulls within the window required to trigger the burst multiplier. |
| 12 | Pull Burst Boost | Float | 1.2 | Multiplier applied to a pull when the burst threshold is met. |
| 13 | Continuation Window (ms) | Int | 1300 | Time window for direction continuation tracking. |
| 14 | Continuation Boost | Float | 1.4 | Multiplier applied to the dominant side when dominant for 2+ consecutive ticks. |
| 15 | Minimum Pull Threshold | Int | 15 | Pulls with absolute value below this are ignored (noise filter). |
| 16 | Minimum Stack Threshold | Int | 20 | Stacks with value below this are ignored (noise filter). |
| 17 | Use Continuation Filter | Yes/No | Yes | Master switch for continuation boost. |

### PowerMeterFeedJS inputs

| # | Input name | Type | Default | Description |
|---|---|---|---|---|
| 0 | DOM Levels Per Side Cap | Int | 10 | Same as above. |
| 1 | Include Best Bid/Ask Level | Yes/No | No | JS default is No (skips level 0 as in standard Jigsaw behaviour). |
| 2 | Depth Levels (P/S) | Int | 4 | Fewer levels than the custom variant by default. |
| 3 | Snapshot Levels (DOM) | Int | 6 | Fewer snapshot levels by default. |
| 4 | Debug Log | Yes/No | No | Same as above. |

---

## 11. Rebuilding the DLLs from Source

The pre-built DLLs target x64 Windows. If you need to rebuild:

### Option A — PowerShell script (easiest)

Requirements: Visual Studio 2022 or 2026 with the **Desktop development with C++** workload.

```powershell
# From the ACSIL\ folder inside the package:
.\build_acsil.ps1
```

The script:
1. Detects MSVC automatically via `vswhere`.
2. Compiles each `.cpp` with the exact flags from Sierra Chart's own `VisualCCompile.Bat`.
3. Writes the DLLs to `C:\SierraChart\Data\` (or specify `-SCRoot "D:\SierraChart"`).

### Option B — Sierra Chart's built-in compiler

1. Copy the `.cpp` files from `ACSIL\src\` to `C:\SierraChart\ACS_Source\`.
2. In Sierra Chart: **Analysis → Build Custom Studies DLL**.
3. The DLLs appear in `C:\SierraChart\Data\` automatically.

### Option C — Visual Studio project

Open `ACSIL\PowerMeterFeed.vcxproj` or `ACSIL\PowerMeterFeedJS.vcxproj` in Visual Studio and build with **Release | x64**.

---

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Overlay stays in demo mode | Study not loaded or IPC not connected | Check DLL is in `C:\SierraChart\Data\`, study is applied to an active chart with DOM data. Enable Debug Log and check for `IPC=OK`. |
| Study does not appear in Add Custom Study | DLL not in Data folder | Copy `.dll` to `C:\SierraChart\Data\` and restart Sierra Chart. |
| "Failed to load study" error in SC | DLL built for wrong architecture | Confirm the DLL is 64-bit (`_64.dll` suffix). Rebuild with `build_acsil.ps1`. |
| Bars always show 0 | DOM data not enabled | Right-click chart → Chart Settings → Market Depth → set Bid/Ask levels to 12+. |
| Col 0 (TBV/TAV) stays near zero | Bar just started or no trades yet | `sc.BidVolume` / `sc.AskVolume` reset at each new bar. The accumulator picks up as trades execute. |
| Col 0 does not reset persistently | Old version of overlay | Ensure you are running the current build. Reset now rebases the delta anchor — it will not snap back on the next tick. |
| Col 1 (P/S) is very noisy | Thresholds too low | Increase `Minimum Pull Threshold` and `Minimum Stack Threshold` (start: 15 and 20). |
| Two studies loaded simultaneously | Both writing to same shared memory | Remove one study. Only one PowerMeterFeed variant should be active at a time. |
| Overlay crashes / disappears | OS DPI scaling issue | Right-click `PowerMeter.exe` → Properties → Compatibility → Override DPI scaling → "Application". |
| Window position lost after restart | Registry permission issue | Run `PowerMeter.exe` once as Administrator to allow registry write, then run normally. |

---

*For technical details about the algorithms, see `docs\ALGORITHM_NOTES.md`.*
