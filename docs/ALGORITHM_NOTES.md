# PowerMeter — Algorithm Notes

> Technical reference for `PowerMeterFeed.cpp` and `PowerMeterFeedJS.cpp`

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Shared Memory Protocol](#2-shared-memory-protocol)
3. [Column 0: Executed Bid / Ask Volume](#3-column-0-executed-bid--ask-volume)
   - [ACSIL source](#31-acsil-source)
   - [Why not GetRecentBidAskVolumeAtPrice](#32-why-not-getrecentbidaskvolumeatprice)
   - [Delta accumulation and persistent reset](#33-delta-accumulation-and-persistent-reset)
4. [PowerMeterFeed — Custom Algorithm](#4-powermetermfeed--custom-algorithm)
   - [Column 1: Custom Bear / Bull P/S Composite](#41-column-1-custom-bear--bull-ps-composite)
   - [Column 2: DOM Snapshot Totals](#42-column-2-dom-snapshot-totals)
5. [PowerMeterFeedJS — Jigsaw-Conceptual Mode](#5-powermetermfeedjs--jigsaw-conceptual-mode)
   - [Column 1: Plain Pull / Stack](#51-column-1-plain-pull--stack)
   - [Differences from the custom variant](#52-differences-from-the-custom-variant)
6. [Signal Flow Diagram](#6-signal-flow-diagram)
7. [Parameter Tuning Guide](#7-parameter-tuning-guide)

---

## 1. Architecture Overview

```
Sierra Chart (SC process)
|
+-- Chart with DOM data
    +-- PowerMeterFeed study  (ACSIL DLL, UpdateAlways=1)
        |
        |  every DOM tick:
        |  1. Read execution arrays via SC API (sc.BidVolume / sc.AskVolume)
        |  2. Read DOM depth via SC API (GetAskMarketDepthStackPullValueAtPrice etc.)
        |  3. Compute 6 float values
        |  4. Write to shared memory  (seqlock)
        v
   Named shared memory: "Local\PowerMeterLiveData"

PowerMeter.exe  (separate Win32 process)
   WM_TIMER @ 33 ms
   +-- Try OpenFileMappingW   (connects lazily)
   +-- Seqlock read -> 6 floats
   +-- Col 0: delta-accumulate with negative-delta clamping
   +-- Set g_isLive = true (seqlock read succeeded and data age < 5000 ms)
   |   else g_isLive = false -> demo mode, status dot turns red
   +-- Animate bars toward new targets  (exponential smoothing alpha=0.13)
   +-- Direct2D render:
       +-- Status dot (top-centre): green = live, red = demo
       +-- Orient button icon: shows TARGET layout (three horizontal bars or three vertical bars)
       +-- Meter columns (vertical or horizontal stacked)
```

The two processes communicate exclusively via a single named memory-mapped file.
Sierra Chart is the **writer**; PowerMeter.exe is the **reader**.
No sockets, no pipes, no COM, no DLLs shared between processes.

---

## 2. Shared Memory Protocol

### Layout (`PMSharedData`, `#pragma pack(push, 4)`)

| Offset | Field | Type | Description |
|---|---|---|---|
| 0 | `sequence` | `volatile LONG` | Seqlock counter. Odd = mid-write; even = stable. |
| 4 | `col0Red` | `float` | TBV — raw sc.BidVolume[idx] (executed-at-bid, bar-cumulative) |
| 8 | `col0Blue` | `float` | TAV — raw sc.AskVolume[idx] (executed-at-ask, bar-cumulative) |
| 12 | `col1Red` | `float` | Bear P/S composite score |
| 16 | `col1Blue` | `float` | Bull P/S composite score |
| 20 | `col2Red` | `float` | Total ASK DOM volume |
| 24 | `col2Blue` | `float` | Total BID DOM volume |
| 28 | `tickCount` | `DWORD` | `GetTickCount()` at last write |

Total struct size: 32 bytes (with 4-byte alignment).

### Write side (ACSIL study, every DOM tick)

```cpp
InterlockedIncrement(&d->sequence);   // sequence becomes ODD  -> writer lock
d->col0Red  = (float)sc.BidVolume[idx];
d->col0Blue = (float)sc.AskVolume[idx];
// ... write remaining 4 floats ...
d->tickCount = GetTickCount();
InterlockedIncrement(&d->sequence);   // sequence becomes EVEN -> write done
```

### Read side (PowerMeter.exe, every 33 ms timer)

```cpp
LONG seq1 = pData->sequence;
MemoryBarrier();
if ((seq1 & 1) == 0) {            // safe to read only if even
    // snapshot all 6 floats
    MemoryBarrier();
    LONG seq2 = pData->sequence;
    if (seq1 == seq2) {           // no write happened during our read
        // use the snapshot
    }
}
```

If the data is older than 5000 ms, PowerMeter.exe closes the mapping and reverts to demo mode until the writer reconnects.

---

## 3. Column 0: Executed Bid / Ask Volume

### 3.1 ACSIL source

```cpp
const int    idx        = sc.ArraySize - 1;      // current (incomplete) bar
const double execBidVol = (double)sc.BidVolume[idx];
const double execAskVol = (double)sc.AskVolume[idx];
```

`sc.BidVolume` and `sc.AskVolume` are Sierra Chart's standard bar-level execution arrays:

- `sc.BidVolume[i]` — cumulative volume of trades that **executed at the bid** (sell-aggressor) within bar `i`.
- `sc.AskVolume[i]` — cumulative volume of trades that **executed at the ask** (buy-aggressor) within bar `i`.

These are the same values shown in Sierra Chart footprint / cluster charts and in the T&S Bid/Ask Vol columns. They are updated on every trade print, carry no inactivity-clear timer, and are purely execution-based.

### 3.2 Why not GetRecentBidAskVolumeAtPrice

`sc.GetRecentBidVolumeAtPrice()` / `sc.GetRecentAskVolumeAtPrice()` are DOM-oriented functions:

- They track volume within a configurable time window per price level (controlled by Chart Settings → "Clear Recent Bid Ask Volume Inactive Time in Milliseconds").
- They clear when a price level has been inactive for that window, making the signal time-dependent rather than execution-cumulative.
- They measure activity *at specific price levels*, not the overall executed imbalance for the current bar.

For a "traded bid vs traded ask" column the correct source is the bar execution arrays, not the recent-volume DOM functions.

### 3.3 Delta accumulation and persistent reset

`sc.BidVolume[idx]` and `sc.AskVolume[idx]` **reset to zero at each new bar**. The raw values sent over shared memory therefore drop to zero at bar boundaries. PowerMeter.exe handles this with delta accumulation:

```
// Col0ResetState (PowerMeter.cpp)
struct Col0ResetState {
    float prevRawRed  = 0;   // last known raw sc.BidVolume from feed
    float prevRawBlue = 0;   // last known raw sc.AskVolume from feed
    float accRed      = 0;   // accumulated executed-bid since last reset
    float accBlue     = 0;   // accumulated executed-ask since last reset
    bool  initialized = false;
};
```

**On each timer tick (UpdateDemo):**

1. Read `c0r = col0Red`, `c0b = col0Blue` from shared memory.
2. If first live sample: set `prevRaw = c0r/c0b`, `acc = 0`, `initialized = true`. No fake delta.
3. Otherwise:
   - `dRed  = c0r - prevRawRed`   — positive on new trades; negative at bar transition
   - `dBlue = c0b - prevRawBlue`
   - Clamp: only add if `dRed > 0` / `dBlue > 0` (bar resets produce negative deltas, which are discarded)
   - `accRed += dRed`, `accBlue += dBlue`
   - `prevRaw = c0r/c0b`
4. `g_columns[0].redTarget = accRed`, `g_columns[0].blueTarget = accBlue`

**On Reset button press (ResetMetersToZero):**

1. Zero `accRed` and `accBlue`.
2. If shared memory is live (age < 5000 ms): set `prevRawRed = col0Red`, `prevRawBlue = col0Blue`, `initialized = true`.
   Otherwise: `initialized = false` (will re-initialise on next live sample).
3. Zero display and target for all columns.

**Why the reset is persistent:** the accumulator is zeroed *and* `prevRaw` is advanced to the current live value. The next delta is therefore `currentRaw - currentRaw = 0`. Subsequent ticks only add incremental execution since the reset moment, so the accumulator never jumps back to the pre-reset session total.

---

## 4. PowerMeterFeed — Custom Algorithm

`SCDLLName("PowerMeter Feed")` — function: `scsf_PowerMeterFeed`

### 4.1 Column 1: Custom Bear / Bull P/S Composite

This is the most complex part of the custom study. It combines four signal-enhancement layers on top of the raw pull/stack API values.

#### Raw pull/stack values

Sierra Chart's `GetAskMarketDepthStackPullValueAtPrice()` and `GetBidMarketDepthStackPullValueAtPrice()` return a signed integer for each DOM price level:

| Value | Meaning |
|---|---|
| `val < 0` | Level is **pulling** (size decreased since last tick) |
| `val > 0` | Level is **stacking** (size increased) |
| `val == 0` | No change at this level |

#### Directional mapping

| Side | Pull (`val < 0`) | Stack (`val > 0`) |
|---|---|---|
| Ask | Ask pulling -> **Bull** pressure | Ask stacking -> **Bear** pressure |
| Bid | Bid pulling -> **Bear** pressure | Bid stacking -> **Bull** pressure |

```
Bear P/S  =  bid pulls  +  ask stacks
Bull P/S  =  ask pulls  +  bid stacks
```

#### Layer 1 — Exponential depth decay

Each level's contribution is multiplied by:

```
w = exp( -k * (level - startLevel) )
```

`k` is the **Decay Constant** input (default 0.10). At the default value:

| Level offset | Weight |
|---|---|
| 0 (best bid/ask) | 1.000 |
| 1 | 0.905 |
| 3 | 0.741 |
| 6 | 0.549 |

Setting `k = 0` disables decay (all levels equal weight).

#### Layer 2 — Stack persistence ramp

A stack that holds its ground over time gets time-weighted credit:

```
effectiveWeight = stackWeight + t * (persistWeight - stackWeight)

where  t = min(1.0,  timeSinceStackAppeared / persistMs)
```

This ramps from `stackWeight` to `persistWeight` linearly over `persistMs` milliseconds.
Default: ramps from **1.0 to 1.6** over **900 ms**.

#### Layer 3 — Pull burst detection

Rapid successive pulls at the same DOM level are flagged as a burst:

```cpp
struct BurstTracker { DWORD firstTime, lastTime; int count; };
```

If `count >= pullBurstCountThresh` pulls occur within `pullBurstWindowMs`:

```
contribution * pullBurstBoost   (default * 1.2)
```

#### Layer 4 — Continuation confirmation

If the same directional side has been dominant for at least **2 consecutive ticks** within `continueWindowMs`:

```
dominantSide * continueBoost   (default * 1.4)
```

#### Minimum thresholds

Before any of the above is applied, events below the noise floor are dropped:

- `pullMinThreshold` — ignore pulls with `|val| < threshold` (default: 15)
- `stackMinThreshold` — ignore stacks with `val < threshold` (default: 20)

#### Full computation per DOM level (ask side example)

```
if val < 0:                          // pull
    absPull = |val|
    if absPull < pullMinThreshold -> skip
    burstMult = GetBurstMultiplier(...)
    bullPSSum += absPull * w * pullWeight * burstMult

if val > 0:                          // stack
    if val < stackMinThreshold -> skip
    effectiveWeight = ramp(stackWeight, persistWeight, elapsed/persistMs)
    bearPSSum += val * w * effectiveWeight
```

Then after both loops:

```
if useContinuationFilter:
    bearPSSum * continuationBoost (if bear has been dominant >= 2 ticks)
    bullPSSum * continuationBoost (if bull has been dominant >= 2 ticks)
```

### 4.2 Column 2: DOM Snapshot Totals

A simple sum of resting quantity at each DOM level:

```
totalAskVol = sum  de.Quantity   (ask levels startLevel ... startLevel + snapshotLevels)
totalBidVol = sum  de.Quantity   (bid levels startLevel ... startLevel + snapshotLevels)
```

This shows the **resting supply/demand balance** across the configured number of levels.
It is a static snapshot (not accumulated) and updates every tick.

---

## 5. PowerMeterFeedJS — Jigsaw-Conceptual Mode

`SCDLLName("PowerMeter Feed JS")` — function: `scsf_PowerMeterFeedJS`

### 5.1 Column 1: Plain Pull / Stack

The JS variant reads the same `GetAskMarketDepthStackPullValueAtPrice` /
`GetBidMarketDepthStackPullValueAtPrice` values but applies **no additional signal processing**:

```
Ask side:
    val < 0  ->  bullPSSum += |val|   (ask pull = bullish)
    val > 0  ->  bearPSSum +=  val    (ask stack = bearish)

Bid side:
    val < 0  ->  bearPSSum += |val|   (bid pull = bearish)
    val > 0  ->  bullPSSum +=  val    (bid stack = bullish)
```

No decay, no persistence, no burst detection, no continuation boost, no thresholds.
All levels contribute equally.

Column 0 and Column 2 are identical to the custom variant.

### 5.2 Differences from the Custom Variant

| Feature | Custom (PowerMeterFeed) | JS (PowerMeterFeedJS) |
|---|---|---|
| Depth decay | `exp(-k * level)` | Flat (weight = 1.0) |
| Pull weighting | Configurable `pullWeight` | Fixed 1.0 |
| Stack weighting | `stackWeight` + persistence ramp | Fixed 1.0 |
| Burst detection | Per-level `BurstTracker` | None |
| Continuation boost | `DirectionTracker` | None |
| Min thresholds | Configurable (pull: 15, stack: 20) | None (all events used) |
| Persistent memory | 3 pointers (IPC + DOM trackers) | 1 pointer (IPC only) |
| Number of inputs | 18 | 5 |

The JS variant is conceptually aligned with how Jigsaw Depth & Sales displays raw pull/stack counts: every DOM change at every configured level counts equally, with no time-based or magnitude-based filtering.

---

## 6. Signal Flow Diagram

```
DOM tick arrives
|
+-- Col 0 (Executed Vol)
|   +-- sc.BidVolume[idx]  -> raw executed-at-bid bar volume -> col0Red  (TBV)
|   +-- sc.AskVolume[idx]  -> raw executed-at-ask bar volume -> col0Blue (TAV)
|       [PowerMeter.exe applies delta accumulation + persistent reset on top]
|
+-- Col 1 (P/S)
|   +-- For each ask level 0..depthLevels:
|   |   GetAskMarketDepthStackPullValueAtPrice()
|   |   val < 0 -> bullish pull  [+ decay w] [+ burst mult]       -> bullPSSum
|   |   val > 0 -> bearish stack [+ decay w] [+ persistence ramp] -> bearPSSum
|   |   val = 0 -> reset trackers for this level
|   |
|   +-- For each bid level 0..depthLevels:
|   |   GetBidMarketDepthStackPullValueAtPrice()
|   |   val < 0 -> bearish pull  [+ decay w] [+ burst mult]       -> bearPSSum
|   |   val > 0 -> bullish stack [+ decay w] [+ persistence ramp] -> bullPSSum
|   |   val = 0 -> reset trackers for this level
|   |
|   +-- Apply continuation boost if enabled
|
+-- Col 2 (DOM totals)
|   +-- Sum de.Quantity across ask levels 0..snapshotLevels -> totalAskVol
|   +-- Sum de.Quantity across bid levels 0..snapshotLevels -> totalBidVol
|
+-- Write to shared memory (seqlock)
    col0Red=execBidVol, col0Blue=execAskVol
    col1Red=bearPSSum,  col1Blue=bullPSSum
    col2Red=totalAskVol, col2Blue=totalBidVol
```

---

## 7. Parameter Tuning Guide

### Starting point

The default values are tuned for **ES (E-mini S&P 500)** on Rithmic / CME data at normal market hours. Adjust for other instruments.

### For a noisier instrument (e.g., CL crude oil)

- Raise `Minimum Pull Threshold` (try 25–40)
- Raise `Minimum Stack Threshold` (try 30–50)
- Lower `Depth Levels (P/S)` to 4

### For a slower instrument (e.g., ZB bonds)

- Lower `Stack Persistence (ms)` to 600
- Raise `Decay Constant k` to 0.20 (inner levels matter more on a thin book)

### Decay constant k

| k | Effect |
|---|---|
| 0.00 | All levels weighted equally |
| 0.10 | (default) Gentle favour of inner levels |
| 0.25 | Strong inner-level focus — outer levels barely count |
| 0.50 | Almost only levels 0–3 matter |

### Stack persistence

Persistence rewards a stack that **absorbs** order flow rather than retreating.
Lower values (400–600 ms) react faster.
Higher values (1200+ ms) only boost stacks that are genuinely holding for a long time.

### Continuation boost

Continuation rewards a direction that keeps asserting across multiple DOM update ticks.
Setting `continueBoost = 1.0` effectively disables it (or use `Use Continuation Filter = No`).

### Col 0 reset cadence

The Reset button is most useful at:

- Session open (reset the overnight / pre-market accumulation)
- After a major news print (clear the event-driven imbalance)
- Whenever you want a fresh read on execution bias from a specific price or time

The rebase mechanism means you can reset as often as you like without affecting the Sierra Chart data source.
