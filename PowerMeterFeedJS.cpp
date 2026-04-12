// PowerMeterFeedJS.cpp
//
// SierraChart ACSIL study - writes live DOM data to the PowerMeter
// Win32 overlay via a named Windows shared-memory segment.
//
// Jigsaw-like simplified mode:
//   col0Red  (RBV)      - Recent Bid Volume summed across N DOM levels
//   col0Blue (RAV)      - Recent Ask Volume summed across N DOM levels
//   col1Red  (Bear P/S) - plain bid-pull sum + ask-stack sum
//   col1Blue (Bull P/S) - plain ask-pull sum + bid-stack sum
//   col2Red  (ASK)      - total ask-side DOM volume across configured levels
//   col2Blue (BID)      - total bid-side DOM volume across configured levels
//
// Column 0 uses sc.GetRecentBidVolumeAtPrice() / sc.GetRecentAskVolumeAtPrice()
// which match the DOM Recent Bid/Ask Volume columns exactly. Reset behaviour is
// controlled by Chart Settings > Market Depth > "Clear Recent Bid Ask Volume
// Inactive Time in Milliseconds" (recommended: 2500).
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
    float  col0Red;         // RBV  - Recent Bid Vol (DOM)
    float  col0Blue;        // RAV  - Recent Ask Vol (DOM)
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
    SCSubgraphRef SG_RecentBid = sc.Subgraph[0]; // col0Red  - RBV (Recent Bid Vol)
    SCSubgraphRef SG_RecentAsk = sc.Subgraph[1]; // col0Blue - RAV (Recent Ask Vol)
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
            "Writes live DOM data to the PowerMeter Win32 overlay via the named "
            "shared memory segment Local\\PowerMeterLiveData. "
            "Column 0 uses sc.GetRecentBidVolumeAtPrice / sc.GetRecentAskVolumeAtPrice "
            "matching the DOM Recent Bid/Ask Volume columns. "
            "Column 1 uses plain Pull/Stack sums without persistence, burst, "
            "continuation, decay, or extra weighting. "
            "Column 2 uses plain DOM snapshot totals. "
            "Reset behaviour for Column 0 is controlled by Chart Settings > "
            "Market Depth > 'Clear Recent Bid Ask Volume Inactive Time in Milliseconds'.";

        sc.GraphRegion         = 0;
        sc.AutoLoop            = 0;
        sc.UpdateAlways        = 1;
        sc.UsesMarketDepthData = 1;

        SG_RecentBid.Name      = "RBV - Recent Bid Vol (DOM)";
        SG_RecentBid.DrawStyle = DRAWSTYLE_IGNORE;

        SG_RecentAsk.Name      = "RAV - Recent Ask Vol (DOM)";
        SG_RecentAsk.DrawStyle = DRAWSTYLE_IGNORE;

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

        i_RecentVolLevels.Name = "Recent Vol Levels Per Side (col 0)";
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

    int recentVolLevels = i_RecentVolLevels.GetInt();
    if (recentVolLevels < 1)
        recentVolLevels = 1;

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
        if (recentVolLevels > numLevels)
            recentVolLevels = numLevels;
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
    // Column 0: Recent Bid/Ask Volume (DOM Recent columns)
    //
    // Ask side: LastTradePrice upward
    // Bid side: LastTradePrice downward
    // -------------------------------------------------------------------------
    double recentAskVol = 0.0; // RAV - buyers lifting ask
    double recentBidVol = 0.0; // RBV - sellers hitting bid

    for (int i = 0; i < recentVolLevels; ++i)
    {
        const float price = sc.LastTradePrice + static_cast<float>(i * sc.TickSize);
        recentAskVol += static_cast<double>(sc.GetRecentAskVolumeAtPrice(price));
    }

    for (int i = 0; i < recentVolLevels; ++i)
    {
        const float price = sc.LastTradePrice - static_cast<float>(i * sc.TickSize);
        recentBidVol += static_cast<double>(sc.GetRecentBidVolumeAtPrice(price));
    }

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
    SG_RecentBid[idx] = static_cast<float>(recentBidVol);
    SG_RecentAsk[idx] = static_cast<float>(recentAskVol);
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

        d->col0Red   = static_cast<float>(recentBidVol);
        d->col0Blue  = static_cast<float>(recentAskVol);
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
            "PMJS | RecentAsk=%.0f RecentBid=%.0f | BearPS=%.1f BullPS=%.1f | "
            "AskDOM=%.0f BidDOM=%.0f | IPC=%s",
            recentAskVol,
            recentBidVol,
            bearPSSum,
            bullPSSum,
            totalAskVol,
            totalBidVol,
            (ipc->pData != NULL) ? "OK" : "FAIL");
        sc.AddMessageToLog(msg, 0);
    }
}