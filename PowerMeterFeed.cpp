// PowerMeterFeed.cpp
//
// SierraChart ACSIL study - writes live DOM data to the PowerMeter
// Win32 overlay via a named Windows shared-memory segment.
//
// *** Place this file in your SierraChart ACS_Source directory ***
// Compile from SierraChart: Analysis > Build Custom Studies DLL
//
// Shared memory object : Local\PowerMeterLiveData
// The PowerMeter.exe must be running on the same Windows machine.
//
// Column mapping (matches PowerMeter.cpp g_columns):
//   col0Red  (TBV)      - Executed Bid Volume: sell-aggressor trades (sc.BidVolume[idx])
//   col0Blue (TAV)      - Executed Ask Volume: buy-aggressor trades (sc.AskVolume[idx])
//   col1Red  (Bear P/S) - weighted bid-pull sum (bearish DOM orders pulled)
//   col1Blue (Bull P/S) - weighted ask-pull sum (bullish DOM orders pulled)
//   col2Red  (ASK)      - total ask-side DOM volume across configured levels
//   col2Blue (BID)      - total bid-side DOM volume across configured levels
//
// Column 0 uses sc.BidVolume[idx] / sc.AskVolume[idx] – Sierra Chart's standard
// bar-level bid/ask execution arrays (same source as footprint charts).
// PowerMeter.cpp applies delta accumulation and persistent reset on top.

#include "sierrachart.h"
#include <math.h>
#include <windows.h>

SCDLLName("PowerMeter Feed")

// -----------------------------------------------------------------------------
// Shared memory layout - must match the PMSharedData struct in PowerMeter.cpp
// -----------------------------------------------------------------------------
#define PM_SHM_NAME L"Local\\PowerMeterLiveData"

#pragma pack(push, 4)
struct PMSharedData
{
    volatile LONG sequence; // odd while writing, even when stable (seqlock)
    float  col0Red;         // TBV  - Executed at Bid Volume (sell-aggressor, sc.BidVolume[idx])
    float  col0Blue;        // TAV  - Executed at Ask Volume (buy-aggressor, sc.AskVolume[idx])
    float  col1Red;         // Bear P/S - bid pull sum
    float  col1Blue;        // Bull P/S - ask pull sum
    float  col2Red;         // ASK DOM total volume
    float  col2Blue;        // BID DOM total volume
    DWORD  tickCount;       // GetTickCount() at last write
};
#pragma pack(pop)

// IPC handle wrapper kept alive across UpdateAlways ticks via persistent memory
struct PMIpcContext
{
    HANDLE        hMapFile;
    PMSharedData* pData;
};

// Per-level stack persistence tracker (ask and bid sides, max 20 levels each)
struct StackTracker
{
    float lastValue = 0.0f;
    float lastPrice = 0.0f;
    DWORD startTime = 0;
};

struct BurstTracker
{
    DWORD firstTime = 0;
    DWORD lastTime = 0;
    int   count = 0;
};

struct DirectionTracker
{
    DWORD firstSignalTime = 0;
    DWORD lastSignalTime = 0;
    int   consecutiveCount = 0;
};

struct PMDomTrackers
{
    StackTracker     askStack[20];
    StackTracker     bidStack[20];
    BurstTracker     bearPullBurst[20];
    BurstTracker     bullPullBurst[20];
    DirectionTracker bearContinuation;
    DirectionTracker bullContinuation;
};

void UpdateBurstTracker(BurstTracker& tracker, DWORD now, DWORD windowMs)
{
    if (tracker.count == 0 || (now - tracker.lastTime) > windowMs)
    {
        tracker.firstTime = now;
        tracker.lastTime = now;
        tracker.count = 1;
        return;
    }
    tracker.lastTime = now;
    tracker.count++;
}

double GetBurstMultiplier(const BurstTracker& tracker, int threshold, double boost)
{
    return (tracker.count >= threshold) ? boost : 1.0;
}

void UpdateDirectionTracker(DirectionTracker& tracker, bool active, DWORD now, DWORD windowMs)
{
    if (!active)
    {
        if (tracker.lastSignalTime != 0 && (now - tracker.lastSignalTime) > windowMs)
        {
            tracker.firstSignalTime = 0;
            tracker.lastSignalTime = 0;
            tracker.consecutiveCount = 0;
        }
        return;
    }
    if (tracker.consecutiveCount == 0 || (now - tracker.lastSignalTime) > windowMs)
    {
        tracker.firstSignalTime = now;
        tracker.lastSignalTime = now;
        tracker.consecutiveCount = 1;
        return;
    }
    tracker.lastSignalTime = now;
    tracker.consecutiveCount++;
}

double GetContinuationMultiplier(const DirectionTracker& tracker, double boost)
{
    return (tracker.consecutiveCount >= 2) ? boost : 1.0;
}

// -----------------------------------------------------------------------------
SCSFExport scsf_PowerMeterFeed(SCStudyInterfaceRef sc)
{
    // Subgraphs (DRAWSTYLE_IGNORE - output is IPC only, not plotted on chart)
    SCSubgraphRef SG_ExecBid = sc.Subgraph[0]; // col0Red  - TBV (Executed Bid Vol, sell-aggressor)
    SCSubgraphRef SG_ExecAsk = sc.Subgraph[1]; // col0Blue - TAV (Executed Ask Vol, buy-aggressor)
    SCSubgraphRef SG_BearPS = sc.Subgraph[2]; // col1Red  - Bear P/S
    SCSubgraphRef SG_BullPS = sc.Subgraph[3]; // col1Blue - Bull P/S
    SCSubgraphRef SG_AskDOM = sc.Subgraph[4]; // col2Red  - ASK DOM vol
    SCSubgraphRef SG_BidDOM = sc.Subgraph[5]; // col2Blue - BID DOM vol

    // Inputs
    SCInputRef i_NumLevels = sc.Input[0];
    SCInputRef i_RecentVolLevels = sc.Input[1];  // levels to sum Recent Bid/Ask Vol (col 0)
    SCInputRef i_UseBestLevel = sc.Input[2];
    SCInputRef i_DecayK = sc.Input[3];
    SCInputRef i_DebugLog = sc.Input[4];
    SCInputRef i_PullWeight = sc.Input[5];
    SCInputRef i_StackWeight = sc.Input[6];
    SCInputRef i_DepthLevels = sc.Input[7];
    SCInputRef i_SnapshotLevels = sc.Input[8];
    SCInputRef i_StackPersistMs = sc.Input[9];
    SCInputRef i_StackPersistWeight = sc.Input[10];
    SCInputRef i_PullBurstWindowMs = sc.Input[11];
    SCInputRef i_PullBurstCountThresh = sc.Input[12];
    SCInputRef i_PullBurstBoost = sc.Input[13];
    SCInputRef i_ContinueWindowMs = sc.Input[14];
    SCInputRef i_ContinueBoost = sc.Input[15];
    SCInputRef i_PullMinThreshold = sc.Input[16];
    SCInputRef i_StackMinThreshold = sc.Input[17];
    SCInputRef i_UseContinuationFilter = sc.Input[18];

    // -------------------------------------------------------------------------
    if (sc.SetDefaults)
    {
        sc.GraphName = "PowerMeter Feed";
        sc.StudyDescription =
            "Writes live DOM and execution data to the PowerMeter Win32 overlay via "
            "the named shared memory segment Local\\PowerMeterLiveData. "
            "Column 0 uses sc.BidVolume[idx] / sc.AskVolume[idx] for executed-at-bid "
            "(sell-aggressor) and executed-at-ask (buy-aggressor) volume per bar. "
            "PowerMeter.cpp applies delta accumulation and persistent reset. "
            "All subgraphs are DRAWSTYLE_IGNORE; output is IPC only.";

        sc.GraphRegion = 0;
        sc.AutoLoop = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;

        SG_ExecBid.Name = "TBV - Executed Bid Vol (sell-aggressor)";
        SG_ExecBid.DrawStyle = DRAWSTYLE_IGNORE;

        SG_ExecAsk.Name = "TAV - Executed Ask Vol (buy-aggressor)";
        SG_ExecAsk.DrawStyle = DRAWSTYLE_IGNORE;

        SG_BearPS.Name = "Bear P/S (bid pulls + ask stacks)";
        SG_BearPS.DrawStyle = DRAWSTYLE_IGNORE;

        SG_BullPS.Name = "Bull P/S (ask pulls + bid stacks)";
        SG_BullPS.DrawStyle = DRAWSTYLE_IGNORE;

        SG_AskDOM.Name = "ASK DOM Total Vol";
        SG_AskDOM.DrawStyle = DRAWSTYLE_IGNORE;

        SG_BidDOM.Name = "BID DOM Total Vol";
        SG_BidDOM.DrawStyle = DRAWSTYLE_IGNORE;

        i_NumLevels.Name = "DOM Levels Per Side Cap (0 = No Cap)";
        i_NumLevels.SetInt(10);

        i_RecentVolLevels.Name = "(unused) Legacy Recent Vol Levels";
        i_RecentVolLevels.SetInt(5);

        i_UseBestLevel.Name = "Include Best Bid/Ask Level (level 0)";
        i_UseBestLevel.SetYesNo(1);

        i_DecayK.Name = "Pull Sum Decay Constant k (0=flat, 0.25=default)";
        i_DecayK.SetFloat(0.10f);

        i_DebugLog.Name = "Debug Log";
        i_DebugLog.SetYesNo(0);

        i_PullWeight.Name = "Bear/Bull P/S - Pull Weight (default 1.0)";
        i_PullWeight.SetFloat(1.1f);

        i_StackWeight.Name = "Bear/Bull P/S - Stack Weight (default 1.0)";
        i_StackWeight.SetFloat(1.0f);

        i_DepthLevels.Name = "Depth Levels (P/S)";
        i_DepthLevels.SetInt(6);

        i_SnapshotLevels.Name = "Snapshot Levels (DOM)";
        i_SnapshotLevels.SetInt(12);

        i_StackPersistMs.Name = "Stack Persistence (ms)";
        i_StackPersistMs.SetInt(900);

        i_StackPersistWeight.Name = "Stack Weight When Persistent";
        i_StackPersistWeight.SetFloat(1.6f);

        i_PullBurstWindowMs.Name = "Pull Burst Window (ms)";
        i_PullBurstWindowMs.SetInt(900);

        i_PullBurstCountThresh.Name = "Pull Burst Count Threshold";
        i_PullBurstCountThresh.SetInt(3);

        i_PullBurstBoost.Name = "Pull Burst Boost";
        i_PullBurstBoost.SetFloat(1.2f);

        i_ContinueWindowMs.Name = "Continuation Window (ms)";
        i_ContinueWindowMs.SetInt(1300);

        i_ContinueBoost.Name = "Continuation Boost";
        i_ContinueBoost.SetFloat(1.4f);

        i_PullMinThreshold.Name = "Minimum Pull Threshold";
        i_PullMinThreshold.SetInt(15);

        i_StackMinThreshold.Name = "Minimum Stack Threshold";
        i_StackMinThreshold.SetInt(20);

        i_UseContinuationFilter.Name = "Use Continuation Filter";
        i_UseContinuationFilter.SetYesNo(1);

        return;
    }

    // -------------------------------------------------------------------------
    // Release OS handles on study removal / SC shutdown
    // -------------------------------------------------------------------------
    if (sc.LastCallToFunction)
    {
        void*& pIpcRaw = sc.GetPersistentPointer(2);
        if (pIpcRaw != NULL)
        {
            PMIpcContext* ctx = reinterpret_cast<PMIpcContext*>(pIpcRaw);
            if (ctx->pData != NULL) { UnmapViewOfFile(ctx->pData);   ctx->pData = NULL; }
            if (ctx->hMapFile != NULL) { CloseHandle(ctx->hMapFile);    ctx->hMapFile = NULL; }
        }
        return;
    }

    sc.SetUseMarketDepthPullingStackingData(1);

    if (sc.ArraySize < 1)
        return;

    const int idx = sc.ArraySize - 1;

    // -------------------------------------------------------------------------
    // Read inputs
    // -------------------------------------------------------------------------
    int numLevels = i_NumLevels.GetInt();

    (void)i_RecentVolLevels.GetInt(); // input slot reserved for backward compatibility

    const bool includeBest = (i_UseBestLevel.GetYesNo() != 0);
    const bool debugLog = (i_DebugLog.GetYesNo() != 0);

    double decayK = (double)i_DecayK.GetFloat();
    if (decayK < 0.0) decayK = 0.0;
    if (decayK > 2.0) decayK = 2.0;
    const bool useDecay = (decayK > 0.0);

    double pullWeight = (double)i_PullWeight.GetFloat();
    double stackWeight = (double)i_StackWeight.GetFloat();
    if (pullWeight < 0.0) pullWeight = 0.0;
    if (stackWeight < 0.0) stackWeight = 0.0;

    int depthLevels = i_DepthLevels.GetInt();
    int snapshotLevels = i_SnapshotLevels.GetInt();
    if (depthLevels < 1) depthLevels = 1;
    if (snapshotLevels < 1) snapshotLevels = 1;
    if (numLevels > 0)
    {
        if (depthLevels > numLevels) depthLevels = numLevels;
        if (snapshotLevels > numLevels) snapshotLevels = numLevels;
    }

    const DWORD  persistMs = (DWORD)(i_StackPersistMs.GetInt() > 0 ? i_StackPersistMs.GetInt() : 0);
    double       persistWeight = (double)i_StackPersistWeight.GetFloat();
    if (persistWeight < 0.0) persistWeight = 0.0;

    const DWORD  pullBurstWindowMs = (DWORD)(i_PullBurstWindowMs.GetInt() > 0 ? i_PullBurstWindowMs.GetInt() : 900);
    const int    pullBurstCountThresh = (i_PullBurstCountThresh.GetInt() > 0 ? i_PullBurstCountThresh.GetInt() : 3);
    const double pullBurstBoost = (double)i_PullBurstBoost.GetFloat();
    const DWORD  continueWindowMs = (DWORD)(i_ContinueWindowMs.GetInt() > 0 ? i_ContinueWindowMs.GetInt() : 1300);
    const double continueBoost = (double)i_ContinueBoost.GetFloat();
    const int    pullMinThreshold = i_PullMinThreshold.GetInt();
    const int    stackMinThreshold = i_StackMinThreshold.GetInt();
    const bool   useContinuationFilter = (i_UseContinuationFilter.GetYesNo() != 0);

    // -------------------------------------------------------------------------
    // DOM availability check (needed before any DOM calls)
    // -------------------------------------------------------------------------
    const int bidLevelsAvail = sc.GetBidMarketDepthNumberOfLevels();
    const int askLevelsAvail = sc.GetAskMarketDepthNumberOfLevels();

    if (bidLevelsAvail <= 0 || askLevelsAvail <= 0)
        return;

    const int startLevel = includeBest ? 0 : 1;

    const DWORD        now = GetTickCount();
    s_MarketDepthEntry de;

    // -------------------------------------------------------------------------
    // Column 0: Executed Bid/Ask Volume (Trade Execution style)
    //
    // Source: sc.BidVolume[idx] / sc.AskVolume[idx]
    //   sc.BidVolume[idx] = volume executed at bid (sell-aggressors hitting bid)
    //                       for the current Sierra Chart bar.
    //   sc.AskVolume[idx] = volume executed at ask (buy-aggressors lifting ask)
    //                       for the current Sierra Chart bar.
    //
    // These are Sierra Chart's standard bar-level bid/ask execution arrays –
    // the same source used by footprint charts and DOM execution columns.
    // They do NOT use GetRecentBidVolumeAtPrice / GetRecentAskVolumeAtPrice
    // and have no inactivity-clear timer.
    //
    // Raw bar-level values are sent to shared memory as-is.
    // PowerMeter.cpp applies delta accumulation and persistent reset, clamping
    // negative deltas to zero to absorb bar-boundary resets cleanly.
    // -------------------------------------------------------------------------
    const double execBidVol = (double)sc.BidVolume[idx];  // sell-aggressor (executed at bid)
    const double execAskVol = (double)sc.AskVolume[idx];  // buy-aggressor  (executed at ask)

    // -------------------------------------------------------------------------
    // DOM directional P/S composites + total DOM volumes
    //
    // Bear P/S = bid pulls + ask stacks
    // Bull P/S = ask pulls + bid stacks
    // -------------------------------------------------------------------------
    void*& pDomRaw = sc.GetPersistentPointer(3);
    if (pDomRaw == NULL)
    {
        pDomRaw = sc.AllocateMemory(sizeof(PMDomTrackers));
        if (pDomRaw == NULL) return;
        memset(pDomRaw, 0, sizeof(PMDomTrackers));
    }
    else if (sc.IsFullRecalculation)
    {
        memset(pDomRaw, 0, sizeof(PMDomTrackers));
    }
    PMDomTrackers* trk = reinterpret_cast<PMDomTrackers*>(pDomRaw);

    double bearPSSum = 0.0;
    double bullPSSum = 0.0;
    double totalAskVol = 0.0;
    double totalBidVol = 0.0;

    // ---- Loop A: Depth (P/S) ------------------------------------------------
    // Pulls are immediate. Stacks ramp from stackWeight to persistWeight
    // linearly over persistMs milliseconds.

    for (int level = startLevel; level < startLevel + depthLevels && level < askLevelsAvail; ++level)
    {
        if (!sc.GetAskMarketDepthEntryAtLevel(de, level)) continue;
        const double w = useDecay ? exp(-decayK * (double)(level - startLevel)) : 1.0;
        const int    val = sc.GetAskMarketDepthStackPullValueAtPrice((float)de.AdjustedPrice);
        const float  price = (float)de.AdjustedPrice;
        const int    tIdx = level - startLevel;
        if (tIdx < 0 || tIdx >= 20) continue;

        if (val < 0)
        {
            // Ask pulling -> bullish
            const int absPull = -val;
            if (absPull < pullMinThreshold) continue;
            UpdateBurstTracker(trk->bullPullBurst[tIdx], now, pullBurstWindowMs);
            const double burstMult = GetBurstMultiplier(trk->bullPullBurst[tIdx], pullBurstCountThresh, pullBurstBoost);
            bullPSSum += (double)absPull * w * pullWeight * burstMult;
        }
        else if (val > 0)
        {
            // Ask stacking -> bearish
            if (val < stackMinThreshold) continue;
            StackTracker& tracker = trk->askStack[tIdx];
            if (tracker.lastValue <= 0.0f || tracker.lastPrice != price)
                tracker.startTime = now;
            tracker.lastPrice = price;
            tracker.lastValue = (float)val;
            const DWORD  duration = now - tracker.startTime;
            double effectiveWeight = stackWeight;
            if (persistMs > 0)
            {
                double t = (double)duration / (double)persistMs;
                if (t > 1.0) t = 1.0;
                effectiveWeight = stackWeight + t * (persistWeight - stackWeight);
            }
            bearPSSum += (double)val * w * effectiveWeight;
        }
        else
        {
            // Empty ask level: clear stack and bull-burst tracker (ask pulls are bullish)
            trk->askStack[tIdx].lastValue = 0.0f;
            trk->askStack[tIdx].lastPrice = 0.0f;
            trk->bullPullBurst[tIdx].count = 0;
            trk->bullPullBurst[tIdx].lastTime = 0;
        }
    }

    for (int level = startLevel; level < startLevel + depthLevels && level < bidLevelsAvail; ++level)
    {
        if (!sc.GetBidMarketDepthEntryAtLevel(de, level)) continue;
        const double w = useDecay ? exp(-decayK * (double)(level - startLevel)) : 1.0;
        const int    val = sc.GetBidMarketDepthStackPullValueAtPrice((float)de.AdjustedPrice);
        const float  price = (float)de.AdjustedPrice;
        const int    tIdx = level - startLevel;
        if (tIdx < 0 || tIdx >= 20) continue;

        if (val < 0)
        {
            // Bid pulling -> bearish
            const int absPull = -val;
            if (absPull < pullMinThreshold) continue;
            UpdateBurstTracker(trk->bearPullBurst[tIdx], now, pullBurstWindowMs);
            const double burstMult = GetBurstMultiplier(trk->bearPullBurst[tIdx], pullBurstCountThresh, pullBurstBoost);
            bearPSSum += (double)absPull * w * pullWeight * burstMult;
        }
        else if (val > 0)
        {
            // Bid stacking -> bullish
            if (val < stackMinThreshold) continue;
            StackTracker& tracker = trk->bidStack[tIdx];
            if (tracker.lastValue <= 0.0f || tracker.lastPrice != price)
                tracker.startTime = now;
            tracker.lastPrice = price;
            tracker.lastValue = (float)val;
            const DWORD  duration = now - tracker.startTime;
            double effectiveWeight = stackWeight;
            if (persistMs > 0)
            {
                double t = (double)duration / (double)persistMs;
                if (t > 1.0) t = 1.0;
                effectiveWeight = stackWeight + t * (persistWeight - stackWeight);
            }
            bullPSSum += (double)val * w * effectiveWeight;
        }
        else
        {
            // Empty bid level: clear stack and bear-burst tracker (bid pulls are bearish)
            trk->bidStack[tIdx].lastValue = 0.0f;
            trk->bidStack[tIdx].lastPrice = 0.0f;
            trk->bearPullBurst[tIdx].count = 0;
            trk->bearPullBurst[tIdx].lastTime = 0;
        }
    }

    // ---- Loop B: Snapshot (DOM totals) --------------------------------------

    for (int level = startLevel; level < startLevel + snapshotLevels && level < askLevelsAvail; ++level)
    {
        if (sc.GetAskMarketDepthEntryAtLevel(de, level))
            totalAskVol += (double)de.Quantity;
    }

    for (int level = startLevel; level < startLevel + snapshotLevels && level < bidLevelsAvail; ++level)
    {
        if (sc.GetBidMarketDepthEntryAtLevel(de, level))
            totalBidVol += (double)de.Quantity;
    }

    // ---- DOM imbalance (reserved for future use) -----------------------------
    const double domTotal = totalAskVol + totalBidVol;
    const double domImbalance = (domTotal > 0.0) ? (totalBidVol / domTotal) : 0.0;
    (void)domImbalance;

    // ---- Continuation confirmation -------------------------------------------
    const bool bearActive = (bearPSSum > bullPSSum) && (bearPSSum > 0.0);
    const bool bullActive = (bullPSSum > bearPSSum) && (bullPSSum > 0.0);

    UpdateDirectionTracker(trk->bearContinuation, bearActive, now, continueWindowMs);
    UpdateDirectionTracker(trk->bullContinuation, bullActive, now, continueWindowMs);

    if (useContinuationFilter)
    {
        bearPSSum *= GetContinuationMultiplier(trk->bearContinuation, continueBoost);
        bullPSSum *= GetContinuationMultiplier(trk->bullContinuation, continueBoost);
    }

    // Write subgraphs (readable from other SC studies if desired)
    SG_ExecBid[idx] = (float)execBidVol;
    SG_ExecAsk[idx] = (float)execAskVol;
    SG_BearPS[idx] = (float)bearPSSum;
    SG_BullPS[idx] = (float)bullPSSum;
    SG_AskDOM[idx] = (float)totalAskVol;
    SG_BidDOM[idx] = (float)totalBidVol;

    // -------------------------------------------------------------------------
    // Shared memory IPC - create on first call, reuse on every tick
    // -------------------------------------------------------------------------
    void*& pIpcRaw = sc.GetPersistentPointer(2);

    if (pIpcRaw == NULL)
    {
        pIpcRaw = sc.AllocateMemory(sizeof(PMIpcContext));
        if (pIpcRaw == NULL) return;
        PMIpcContext* ctx = reinterpret_cast<PMIpcContext*>(pIpcRaw);
        ctx->hMapFile = NULL;
        ctx->pData = NULL;
    }

    PMIpcContext* ipc = reinterpret_cast<PMIpcContext*>(pIpcRaw);

    if (ipc->hMapFile == NULL)
    {
        ipc->hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            (DWORD)sizeof(PMSharedData),
            PM_SHM_NAME);

        if (ipc->hMapFile != NULL)
        {
            ipc->pData = reinterpret_cast<PMSharedData*>(
                MapViewOfFile(ipc->hMapFile, FILE_MAP_WRITE, 0, 0, sizeof(PMSharedData)));

            if (ipc->pData == NULL)
            {
                CloseHandle(ipc->hMapFile);
                ipc->hMapFile = NULL;
            }
            else
            {
                memset(ipc->pData, 0, sizeof(PMSharedData));
            }
        }
    }

    if (ipc->pData != NULL)
    {
        PMSharedData* d = ipc->pData;

        InterlockedIncrement(&d->sequence); // mark write start (sequence becomes odd)

        d->col0Red  = (float)execBidVol;
        d->col0Blue = (float)execAskVol;
        d->col1Red = (float)bearPSSum;
        d->col1Blue = (float)bullPSSum;
        d->col2Red = (float)totalAskVol;
        d->col2Blue = (float)totalBidVol;
        d->tickCount = GetTickCount();

        InterlockedIncrement(&d->sequence); // mark write done (sequence becomes even)
    }

    // -------------------------------------------------------------------------
    if (debugLog)
    {
        SCString msg;
        msg.Format(
            "PM | ExecAsk=%.0f ExecBid=%.0f | BearPS=%.1f BullPS=%.1f | "
            "AskDOM=%.0f BidDOM=%.0f | pw=%.2f sw=%.2f | "
            "bearCont=%d bullCont=%d | IPC=%s",
            execAskVol, execBidVol,
            bearPSSum, bullPSSum,
            totalAskVol, totalBidVol,
            pullWeight, stackWeight,
            trk->bearContinuation.consecutiveCount,
            trk->bullContinuation.consecutiveCount,
            (ipc->pData != NULL) ? "OK" : "FAIL");
        sc.AddMessageToLog(msg, 0);
    }
}
