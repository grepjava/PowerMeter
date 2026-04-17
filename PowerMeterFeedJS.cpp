// PowerMeterFeedJS.cpp
//
// SierraChart ACSIL study - writes live DOM data to the PowerMeter
// Win32 overlay via a named Windows shared-memory segment.
//
// Jigsaw-like simplified mode:
//   col0Red  (TBV)      - Executed Bid Volume: sell-aggressor trades (sc.BidVolume[idx])
//   col0Blue (TAV)      - Executed Ask Volume: buy-aggressor trades (sc.AskVolume[idx])
//   col1Red  (Bear P/S) - plain bid-pull sum + ask-stack sum
//   col1Blue (Bull P/S) - plain ask-pull sum + bid-stack sum
//   col2Red  (ASK)      - total ask-side DOM volume across configured levels
//   col2Blue (BID)      - total bid-side DOM volume across configured levels
//
// Column 0 uses sc.BidVolume[idx] / sc.AskVolume[idx] – Sierra Chart's standard
// bar-level bid/ask execution arrays. PowerMeter.cpp applies delta accumulation
// and persistent reset on top of these raw values.
//
// *** Place this file in your SierraChart ACS_Source directory ***
// Compile from SierraChart: Analysis > Build Custom Studies DLL

#include "sierrachart.h"
#include <windows.h>

SCDLLName("PowerMeter Feed JS")

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
    float  col1Red;         // Bear P/S
    float  col1Blue;        // Bull P/S
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

// -----------------------------------------------------------------------------
SCSFExport scsf_PowerMeterFeedJS(SCStudyInterfaceRef sc)
{
    // Subgraphs (DRAWSTYLE_IGNORE - output is IPC only, not plotted on chart)
    SCSubgraphRef SG_ExecBid = sc.Subgraph[0]; // col0Red  - TBV (Executed Bid Vol, sell-aggressor)
    SCSubgraphRef SG_ExecAsk = sc.Subgraph[1]; // col0Blue - TAV (Executed Ask Vol, buy-aggressor)
    SCSubgraphRef SG_BearPS    = sc.Subgraph[2]; // col1Red  - Bear P/S
    SCSubgraphRef SG_BullPS    = sc.Subgraph[3]; // col1Blue - Bull P/S
    SCSubgraphRef SG_AskDOM    = sc.Subgraph[4]; // col2Red  - ASK DOM vol
    SCSubgraphRef SG_BidDOM    = sc.Subgraph[5]; // col2Blue - BID DOM vol

    // Inputs
    SCInputRef i_NumLevels       = sc.Input[0];
    SCInputRef i_RecentVolLevels = sc.Input[1];
    SCInputRef i_UseBestLevel    = sc.Input[2];
    SCInputRef i_DepthLevels     = sc.Input[3];
    SCInputRef i_SnapshotLevels  = sc.Input[4];
    SCInputRef i_DebugLog        = sc.Input[5];

    // -------------------------------------------------------------------------
    if (sc.SetDefaults)
    {
        sc.GraphName = "PowerMeter Feed JS";
        sc.StudyDescription =
            "Writes live DOM and execution data to the PowerMeter Win32 overlay via "
            "the named shared memory segment Local\\PowerMeterLiveData. "
            "Column 0 uses sc.BidVolume[idx] / sc.AskVolume[idx] for executed-at-bid "
            "(sell-aggressor) and executed-at-ask (buy-aggressor) volume per bar. "
            "Column 1 uses plain Pull/Stack sums without persistence, burst, "
            "continuation, decay, or extra weighting. "
            "Column 2 uses plain DOM snapshot totals. "
            "PowerMeter.cpp applies delta accumulation and persistent reset for Column 0.";

        sc.GraphRegion         = 0;
        sc.AutoLoop            = 0;
        sc.UpdateAlways        = 1;
        sc.UsesMarketDepthData = 1;

        SG_ExecBid.Name      = "TBV - Executed Bid Vol (sell-aggressor)";
        SG_ExecBid.DrawStyle = DRAWSTYLE_IGNORE;

        SG_ExecAsk.Name      = "TAV - Executed Ask Vol (buy-aggressor)";
        SG_ExecAsk.DrawStyle = DRAWSTYLE_IGNORE;

        SG_BearPS.Name         = "Bear P/S (plain bid pulls + ask stacks)";
        SG_BearPS.DrawStyle    = DRAWSTYLE_IGNORE;

        SG_BullPS.Name         = "Bull P/S (plain ask pulls + bid stacks)";
        SG_BullPS.DrawStyle    = DRAWSTYLE_IGNORE;

        SG_AskDOM.Name         = "ASK DOM Total Vol";
        SG_AskDOM.DrawStyle    = DRAWSTYLE_IGNORE;

        SG_BidDOM.Name         = "BID DOM Total Vol";
        SG_BidDOM.DrawStyle    = DRAWSTYLE_IGNORE;

        i_NumLevels.Name = "DOM Levels Per Side Cap (0 = No Cap)";
        i_NumLevels.SetInt(10);

        i_RecentVolLevels.Name = "(unused) Legacy Recent Vol Levels";
        i_RecentVolLevels.SetInt(5);

        i_UseBestLevel.Name = "Include Best Bid/Ask Level (level 0)";
        i_UseBestLevel.SetYesNo(0);

        i_DepthLevels.Name = "Depth Levels (P/S)";
        i_DepthLevels.SetInt(4);

        i_SnapshotLevels.Name = "Snapshot Levels (DOM)";
        i_SnapshotLevels.SetInt(6);

        i_DebugLog.Name = "Debug Log";
        i_DebugLog.SetYesNo(0);

        return;
    }

    // -------------------------------------------------------------------------
    // Release OS handles on study removal / SC shutdown
    // -------------------------------------------------------------------------
    if (sc.LastCallToFunction)
    {
        void*& pIpcRaw = sc.GetPersistentPointer(1);
        if (pIpcRaw != NULL)
        {
            PMIpcContext* ctx = reinterpret_cast<PMIpcContext*>(pIpcRaw);
            if (ctx->pData != NULL)
            {
                UnmapViewOfFile(ctx->pData);
                ctx->pData = NULL;
            }
            if (ctx->hMapFile != NULL)
            {
                CloseHandle(ctx->hMapFile);
                ctx->hMapFile = NULL;
            }
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
    const bool debugLog    = (i_DebugLog.GetYesNo() != 0);

    int depthLevels = i_DepthLevels.GetInt();
    if (depthLevels < 1)
        depthLevels = 1;

    int snapshotLevels = i_SnapshotLevels.GetInt();
    if (snapshotLevels < 1)
        snapshotLevels = 1;

    if (numLevels > 0)
    {
        if (depthLevels > numLevels)
            depthLevels = numLevels;
        if (snapshotLevels > numLevels)
            snapshotLevels = numLevels;
    }

    // -------------------------------------------------------------------------
    // DOM availability check
    // -------------------------------------------------------------------------
    const int bidLevelsAvail = sc.GetBidMarketDepthNumberOfLevels();
    const int askLevelsAvail = sc.GetAskMarketDepthNumberOfLevels();

    if (bidLevelsAvail <= 0 || askLevelsAvail <= 0)
        return;

    const int startLevel = includeBest ? 0 : 1;

    s_MarketDepthEntry de;

    // -------------------------------------------------------------------------
    // Column 0: Executed Bid/Ask Volume (Trade Execution style)
    //
    // Source: sc.BidVolume[idx] / sc.AskVolume[idx]
    //   sc.BidVolume[idx] = volume executed at bid (sell-aggressors hitting bid)
    //   sc.AskVolume[idx] = volume executed at ask (buy-aggressors lifting ask)
    //
    // Sierra Chart's standard bar-level bid/ask execution arrays.
    // No inactivity-clear timer. No GetRecentBidVolumeAtPrice used.
    // PowerMeter.cpp applies delta accumulation and persistent reset on top.
    // -------------------------------------------------------------------------
    const double execBidVol = static_cast<double>(sc.BidVolume[idx]);  // sell-aggressor
    const double execAskVol = static_cast<double>(sc.AskVolume[idx]);  // buy-aggressor

    // -------------------------------------------------------------------------
    // Column 1: Plain Pull/Stack directional composite
    //
    // Bear P/S = bid pulls + ask stacks
    // Bull P/S = ask pulls + bid stacks
    //
    // No decay, no burst logic, no continuation logic, no persistence weighting.
    // -------------------------------------------------------------------------
    double bearPSSum   = 0.0;
    double bullPSSum   = 0.0;
    double totalAskVol = 0.0;
    double totalBidVol = 0.0;

    // Ask side
    for (int level = startLevel; level < startLevel + depthLevels && level < askLevelsAvail; ++level)
    {
        if (!sc.GetAskMarketDepthEntryAtLevel(de, level))
            continue;

        const int val = sc.GetAskMarketDepthStackPullValueAtPrice(static_cast<float>(de.AdjustedPrice));

        if (val < 0)
        {
            // Ask pulling -> bullish
            bullPSSum += static_cast<double>(-val);
        }
        else if (val > 0)
        {
            // Ask stacking -> bearish
            bearPSSum += static_cast<double>(val);
        }
    }

    // Bid side
    for (int level = startLevel; level < startLevel + depthLevels && level < bidLevelsAvail; ++level)
    {
        if (!sc.GetBidMarketDepthEntryAtLevel(de, level))
            continue;

        const int val = sc.GetBidMarketDepthStackPullValueAtPrice(static_cast<float>(de.AdjustedPrice));

        if (val < 0)
        {
            // Bid pulling -> bearish
            bearPSSum += static_cast<double>(-val);
        }
        else if (val > 0)
        {
            // Bid stacking -> bullish
            bullPSSum += static_cast<double>(val);
        }
    }

    // -------------------------------------------------------------------------
    // Column 2: Snapshot (DOM totals)
    // -------------------------------------------------------------------------
    for (int level = startLevel; level < startLevel + snapshotLevels && level < askLevelsAvail; ++level)
    {
        if (sc.GetAskMarketDepthEntryAtLevel(de, level))
            totalAskVol += static_cast<double>(de.Quantity);
    }

    for (int level = startLevel; level < startLevel + snapshotLevels && level < bidLevelsAvail; ++level)
    {
        if (sc.GetBidMarketDepthEntryAtLevel(de, level))
            totalBidVol += static_cast<double>(de.Quantity);
    }

    // Write subgraphs
    SG_ExecBid[idx] = static_cast<float>(execBidVol);
    SG_ExecAsk[idx] = static_cast<float>(execAskVol);
    SG_BearPS[idx]    = static_cast<float>(bearPSSum);
    SG_BullPS[idx]    = static_cast<float>(bullPSSum);
    SG_AskDOM[idx]    = static_cast<float>(totalAskVol);
    SG_BidDOM[idx]    = static_cast<float>(totalBidVol);

    // -------------------------------------------------------------------------
    // Shared memory IPC - create on first call, reuse on every tick
    // -------------------------------------------------------------------------
    void*& pIpcRaw = sc.GetPersistentPointer(1);

    if (pIpcRaw == NULL)
    {
        pIpcRaw = sc.AllocateMemory(sizeof(PMIpcContext));
        if (pIpcRaw == NULL)
            return;

        PMIpcContext* ctx = reinterpret_cast<PMIpcContext*>(pIpcRaw);
        ctx->hMapFile = NULL;
        ctx->pData    = NULL;
    }

    PMIpcContext* ipc = reinterpret_cast<PMIpcContext*>(pIpcRaw);

    if (ipc->hMapFile == NULL)
    {
        ipc->hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(PMSharedData)),
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

        InterlockedIncrement(&d->sequence); // write start (odd)

        d->col0Red   = static_cast<float>(execBidVol);
        d->col0Blue  = static_cast<float>(execAskVol);
        d->col1Red   = static_cast<float>(bearPSSum);
        d->col1Blue  = static_cast<float>(bullPSSum);
        d->col2Red   = static_cast<float>(totalAskVol);
        d->col2Blue  = static_cast<float>(totalBidVol);
        d->tickCount = GetTickCount();

        InterlockedIncrement(&d->sequence); // write done (even)
    }

    // -------------------------------------------------------------------------
    // Debug log
    // -------------------------------------------------------------------------
    if (debugLog)
    {
        SCString msg;
        msg.Format(
            "PMJS | ExecAsk=%.0f ExecBid=%.0f | BearPS=%.1f BullPS=%.1f | "
            "AskDOM=%.0f BidDOM=%.0f | IPC=%s",
            execAskVol,
            execBidVol,
            bearPSSum,
            bullPSSum,
            totalAskVol,
            totalBidVol,
            (ipc->pData != NULL) ? "OK" : "FAIL");
        sc.AddMessageToLog(msg, 0);
    }
}