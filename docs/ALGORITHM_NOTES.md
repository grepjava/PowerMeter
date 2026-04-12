# PowerMeter — Algorithm Notes

> Technical reference for `PowerMeterFeed.cpp` and `PowerMeterFeedJS.cpp`

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Shared Memory Protocol](#2-shared-memory-protocol)
3. [PowerMeterFeed — Custom Algorithm](#3-powermetermfeed--custom-algorithm)
   - [Column 0: Recent Volume](#31-column-0-recent-bid--ask-volume)
   - [Column 1: Custom Bear / Bull P/S Composite](#32-column-1-custom-bear--bull-ps-composite)
   - [Column 2: DOM Snapshot Totals](#33-column-2-dom-snapshot-totals)
4. [PowerMeterFeedJS — Jigsaw-Conceptual Mode](#4-powermetermfeedjs--jigsaw-conceptual-mode)
   - [Column 1: Plain Pull / Stack](#41-column-1-plain-pull--stack)
   - [Differences from the custom variant](#42-differences-from-the-custom-variant)
5. [Signal Flow Diagram](#5-signal-flow-diagram)
6. [Parameter Tuning Guide](#6-parameter-tuning-guide)

---

## 1. Architecture Overview

```
Sierra Chart (SC process)
│
├─ Chart with DOM data
│   └─ PowerMeterFeed study  (ACSIL DLL, UpdateAlways=1)
│       │
│       │  every DOM tick:
│       │  1. Read DOM via SC API
│       │  2. Compute 6 float values
│       │  3. Write to shared memory  (seqlock)
│       ▼
│  Named shared memory: "Local\PowerMeterLiveData"
│
PowerMeter.exe  (separate Win32 process)
│   WM_TIMER @ 33 ms
│   ├─ Try OpenFileMappingW   (connects lazily)
│   ├─ Seqlock read → 6 floats
│   ├─ Animate bars toward new targets  (exponential smoothing α=0.13)
│   └─ Direct2D render
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
| 4 | `col0Red` | `float` | RBV — Recent Bid Volume |
| 8 | `col0Blue` | `float` | RAV — Recent Ask Volume |
| 12 | `col1Red` | `float` | Bear P/S composite score |
| 16 | `col1Blue` | `float` | Bull P/S composite score |
| 20 | `col2Red` | `float` | Total ASK DOM volume |
| 24 | `col2Blue` | `float` | Total BID DOM volume |
| 28 | `tickCount` | `DWORD` | `GetTickCount()` at last write |

Total struct size: 32 bytes (with 4-byte alignment).

### Write side (ACSIL study, every DOM tick)

```cpp
InterlockedIncrement(&d->sequence);   // sequence becomes ODD  → writer lock
d->col0Red  = ...;
// ... write all 6 floats ...
d->tickCount = GetTickCount();
InterlockedIncrement(&d->sequence);   // sequence becomes EVEN → write done
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

If the data is older than 5 000 ms, PowerMeter.exe closes the mapping and reverts to demo mode until the writer reconnects.

---

## 3. PowerMeterFeed — Custom Algorithm

`SCDLLName("PowerMeter Feed")` — function: `scsf_PowerMeterFeed`

### 3.1 Column 0: Recent Bid / Ask Volume

**Goal**: show how much volume has printed on the bid vs ask at the inside market recently, exactly as Sierra Chart's DOM "Recent Bid Volume" / "Recent Ask Volume" columns would show.

```
RAV  =  Σ  sc.GetRecentAskVolumeAtPrice( LastTradePrice + i × TickSize )
         i = 0 … recentVolLevels-1

RBV  =  Σ  sc.GetRecentBidVolumeAtPrice( LastTradePrice - i × TickSize )
         i = 0 … recentVolLevels-1
```

- Both sides start at `LastTradePrice` (level 0) because both bid and ask volume accumulate at the last traded price.  
- The walk is symmetric: ask side goes **up** the ladder; bid side goes **down**.
- The reset window is controlled by the Sierra Chart chart setting  
  **"Clear Recent Bid Ask Volume Inactive Time in Milliseconds"**  
  (recommended: 2 500 ms to match Jigsaw Depth & Sales default).

### 3.2 Column 1: Custom Bear / Bull P/S Composite

This is the most complex part of the custom study. It combines four distinct signal-enhancement layers on top of the raw pull/stack API values.

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
| Ask | Ask pulling → **Bull** pressure | Ask stacking → **Bear** pressure |
| Bid | Bid pulling → **Bear** pressure | Bid stacking → **Bull** pressure |

```
Bear P/S  =  bid pulls  +  ask stacks
Bull P/S  =  ask pulls  +  bid stacks
```

#### Layer 1 — Exponential depth decay

Levels deeper in the book are less immediately relevant. Each level's contribution is multiplied by:

```
w = exp( -k × (level - startLevel) )
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

A DOM stack is more significant if it holds its ground over time (absorbing incoming order flow). The persistence mechanism gives a stack time-weighted credit:

```
effectiveWeight = stackWeight + t × (persistWeight - stackWeight)

where  t = min(1.0,  timeSinceStackAppeared / persistMs)
```

This ramps from `stackWeight` to `persistWeight` linearly over `persistMs` milliseconds. The stack's start time resets if the stack disappears or moves to a different price.

Default: ramps from **1.0 → 1.6** over **900 ms**.

#### Layer 3 — Pull burst detection

Rapid successive pulls at the same DOM level (e.g., an order being cancelled in chunks) are flagged as a burst and get an additional boost:

```cpp
struct BurstTracker { DWORD firstTime, lastTime; int count; };
```

If `count ≥ pullBurstCountThresh` pulls occur within `pullBurstWindowMs`:

```
contribution × pullBurstBoost   (default × 1.2)
```

The burst counter resets when the level goes quiet for longer than the window.

#### Layer 4 — Continuation confirmation

If the same directional side (bear or bull) has been dominant for at least **2 consecutive ticks** within `continueWindowMs`:

```
dominantSide × continueBoost   (default × 1.4)
```

This rewards persistence of direction and helps filter one-off noise spikes. The `Use Continuation Filter` input is a master on/off switch.

#### Minimum thresholds

Before any of the above is applied, events below the noise floor are dropped:

- `pullMinThreshold` — ignore pulls with `|val| < threshold` (default: 15)
- `stackMinThreshold` — ignore stacks with `val < threshold` (default: 20)

#### Full computation per DOM level (ask side example)

```
if val < 0:                          // pull
    absPull = |val|
    if absPull < pullMinThreshold → skip
    burstMult = GetBurstMultiplier(...)
    bullPSSum += absPull × w × pullWeight × burstMult

if val > 0:                          // stack
    if val < stackMinThreshold → skip
    effectiveWeight = ramp(stackWeight, persistWeight, elapsed/persistMs)
    bearPSSum += val × w × effectiveWeight
```

Then after both loops:

```
if useContinuationFilter:
    bearPSSum × continuationBoost (if bear has been dominant ≥ 2 ticks)
    bullPSSum × continuationBoost (if bull has been dominant ≥ 2 ticks)
```

### 3.3 Column 2: DOM Snapshot Totals

A simple sum of resting quantity at each DOM level:

```
totalAskVol = Σ  de.Quantity   (ask levels startLevel … startLevel + snapshotLevels)
totalBidVol = Σ  de.Quantity   (bid levels startLevel … startLevel + snapshotLevels)
```

This shows the **resting supply/demand balance** across the configured number of levels. It is a static snapshot (not accumulated) and updates every tick.

---

## 4. PowerMeterFeedJS — Jigsaw-Conceptual Mode

`SCDLLName("PowerMeter Feed JS")` — function: `scsf_PowerMeterFeedJS`

### 4.1 Column 1: Plain Pull / Stack

The JS variant reads the same `GetAskMarketDepthStackPullValueAtPrice` /  
`GetBidMarketDepthStackPullValueAtPrice` values but applies **no additional signal processing**:

```
Ask side:
    val < 0  →  bullPSSum += |val|   (ask pull = bullish)
    val > 0  →  bearPSSum +=  val    (ask stack = bearish)

Bid side:
    val < 0  →  bearPSSum += |val|   (bid pull = bearish)
    val > 0  →  bullPSSum +=  val    (bid stack = bullish)
```

No decay, no persistence, no burst detection, no continuation boost, no thresholds.  
All levels contribute equally.

Column 0 and Column 2 are identical to the custom variant.

### 4.2 Differences from the Custom Variant

| Feature | Custom (PowerMeterFeed) | JS (PowerMeterFeedJS) |
|---|---|---|
| Depth decay | `exp(-k × level)` | Flat (weight = 1.0) |
| Pull weighting | Configurable `pullWeight` | Fixed 1.0 |
| Stack weighting | `stackWeight` + persistence ramp | Fixed 1.0 |
| Burst detection | Per-level `BurstTracker` | None |
| Continuation boost | `DirectionTracker` | None |
| Min thresholds | Configurable (pull: 15, stack: 20) | None (all events used) |
| Persistent memory | 3 pointers (IPC + DOM trackers) | 1 pointer (IPC only) |
| Number of inputs | 19 | 6 |

The JS variant is conceptually aligned with how Jigsaw Depth & Sales displays raw pull/stack counts: every DOM change at every configured level counts equally, with no time-based or magnitude-based filtering.

---

## 5. Signal Flow Diagram

```
DOM tick arrives
│
├─ Col 0 (Recent Vol)
│   ├─ Walk LastTradePrice upward N levels  → GetRecentAskVolumeAtPrice → RAV
│   └─ Walk LastTradePrice downward N levels → GetRecentBidVolumeAtPrice → RBV
│
├─ Col 1 (P/S)
│   ├─ For each ask level 0..depthLevels:
│   │   GetAskMarketDepthStackPullValueAtPrice()
│   │   val < 0 → bullish pull  [+ decay w] [+ burst mult]   → bullPSSum
│   │   val > 0 → bearish stack [+ decay w] [+ persistence ramp] → bearPSSum
│   │   val = 0 → reset trackers for this level
│   │
│   ├─ For each bid level 0..depthLevels:
│   │   GetBidMarketDepthStackPullValueAtPrice()
│   │   val < 0 → bearish pull  [+ decay w] [+ burst mult]   → bearPSSum
│   │   val > 0 → bullish stack [+ decay w] [+ persistence ramp] → bullPSSum
│   │   val = 0 → reset trackers for this level
│   │
│   └─ Apply continuation boost if enabled
│
├─ Col 2 (DOM totals)
│   ├─ Sum de.Quantity across ask levels 0..snapshotLevels → totalAskVol
│   └─ Sum de.Quantity across bid levels 0..snapshotLevels → totalBidVol
│
└─ Write to shared memory (seqlock)
    col0Red=RBV, col0Blue=RAV
    col1Red=bearPSSum, col1Blue=bullPSSum
    col2Red=totalAskVol, col2Blue=totalBidVol
```

---

## 6. Parameter Tuning Guide

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
Higher values (1 200+ ms) only boost stacks that are genuinely holding for a long time.

### Continuation boost

Continuation rewards a direction that keeps asserting across multiple DOM update ticks.  
Setting `continueBoost = 1.0` effectively disables it (or use `Use Continuation Filter = No`).  
Setting it above 1.6 may create a lag in direction reversal detection.

### Pull burst boost

Useful for detecting large iceberg orders being pulled in chunks.  
Set `pullBurstCountThresh` to 2 for more sensitivity (any two pulls in the window) or 4+ for only flagging very aggressive pulling.

### When to use JS vs Custom

| Scenario | Recommendation |
|---|---|
| Learning DOM analysis from scratch | JS — fewer variables to interpret |
| Scalping with very fast markets | JS — no lag from persistence/continuation |
| Swing intraday, filtering DOM noise | Custom — thresholds + decay help |
| Studying where limit orders are being pulled | Custom — burst detection highlights pulling activity |
| Matching a Jigsaw Depth & Sales reference | JS |
