#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cwchar>
#include <algorithm>
#include <array>
#include <random>
#include <cmath>
#include "Resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "Advapi32.lib")

template <class T>
void SafeRelease(T** pp)
{
    if (pp && *pp)
    {
        (*pp)->Release();
        *pp = nullptr;
    }
}

struct MeterColumn
{
    const wchar_t* topLabel;
    const wchar_t* bottomLabel;

    float redDisplay;
    float blueDisplay;

    float redTarget;
    float blueTarget;
};

static const UINT_PTR TIMER_ID_ANIMATE = 1;
static const UINT TIMER_INTERVAL_MS = 33;

static HWND g_hWnd = nullptr;

static ID2D1Factory* g_d2dFactory = nullptr;
static ID2D1HwndRenderTarget* g_renderTarget = nullptr;
static ID2D1SolidColorBrush* g_brush = nullptr;
static IDWriteFactory* g_dwriteFactory = nullptr;
static IDWriteTextFormat* g_textFormat      = nullptr;
static IDWriteTextFormat* g_smallTextFormat = nullptr;
static IDWriteTextFormat* g_iconFormat      = nullptr;

static D2D1_RECT_F g_resetButtonRect   = D2D1::RectF(0, 0, 0, 0);
static D2D1_RECT_F g_pinButtonRect     = D2D1::RectF(0, 0, 0, 0);
static D2D1_RECT_F g_closeButtonRect   = D2D1::RectF(0, 0, 0, 0);
static bool        g_isPinned          = false;

// Shared memory layout – must match PowerMeterFeed.cpp exactly.
// col0Red/Blue : rolling sell / buy volume (T&S window)
// col1Red/Blue : bid pull sum (Bear P/S) / ask pull sum (Bull P/S)
// col2Red/Blue : total ASK DOM volume / total BID DOM volume
#pragma pack(push, 4)
struct PMSharedData
{
    volatile LONG sequence; // odd while writing, even when stable
    float  col0Red;
    float  col0Blue;
    float  col1Red;
    float  col1Blue;
    float  col2Red;
    float  col2Blue;
    DWORD  tickCount;       // GetTickCount() at last write
};
#pragma pack(pop)

static HANDLE              g_hSharedMem  = nullptr;
static const PMSharedData* g_pSharedData = nullptr;

static std::array<MeterColumn, 3> g_columns =
{
    MeterColumn{ L"RBV",      L"RAV",      0.0f, 0.0f, 0.0f, 0.0f },
    MeterColumn{ L"Bear P/S", L"Bull P/S", 0.0f, 0.0f, 0.0f, 0.0f },
    MeterColumn{ L"ASK",      L"BID",      0.0f, 0.0f, 0.0f, 0.0f }
};

static std::mt19937 g_rng{ std::random_device{}() };
static int g_demoTick = 0;

HRESULT CreateDeviceIndependentResources()
{
    HRESULT hr = S_OK;

    if (!g_d2dFactory)
    {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
    }

    if (SUCCEEDED(hr) && !g_dwriteFactory)
    {
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    }

    if (SUCCEEDED(hr) && !g_textFormat)
    {
        hr = g_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            17.0f,
            L"en-us",
            &g_textFormat);
    }

    if (SUCCEEDED(hr) && !g_smallTextFormat)
    {
        hr = g_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &g_smallTextFormat);
    }

    if (SUCCEEDED(hr) && !g_iconFormat)
    {
        hr = g_dwriteFactory->CreateTextFormat(
            L"Segoe MDL2 Assets",
            nullptr,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f,
            L"en-us",
            &g_iconFormat);
    }

    if (SUCCEEDED(hr))
    {
        g_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        g_smallTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_smallTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (g_iconFormat)
        {
            g_iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g_iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    return hr;
}

HRESULT CreateDeviceResources(HWND hWnd)
{
    HRESULT hr = S_OK;

    if (!g_renderTarget)
    {
        RECT rc{};
        GetClientRect(hWnd, &rc);

        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(rc.right - rc.left),
            static_cast<UINT32>(rc.bottom - rc.top));

        hr = g_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hWnd, size),
            &g_renderTarget);
    }

    if (SUCCEEDED(hr) && !g_brush)
    {
        hr = g_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White),
            &g_brush);
    }

    return hr;
}

void DiscardDeviceResources()
{
    SafeRelease(&g_brush);
    SafeRelease(&g_renderTarget);
}

void TryOpenSharedMemory()
{
    if (g_hSharedMem)
        return;

    g_hSharedMem = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\PowerMeterLiveData");
    if (!g_hSharedMem)
        return;

    g_pSharedData = static_cast<const PMSharedData*>(
        MapViewOfFile(g_hSharedMem, FILE_MAP_READ, 0, 0, sizeof(PMSharedData)));

    if (!g_pSharedData)
    {
        CloseHandle(g_hSharedMem);
        g_hSharedMem = nullptr;
    }
}

void CloseSharedMemory()
{
    if (g_pSharedData)
    {
        UnmapViewOfFile(g_pSharedData);
        g_pSharedData = nullptr;
    }
    if (g_hSharedMem)
    {
        CloseHandle(g_hSharedMem);
        g_hSharedMem = nullptr;
    }
}

void CleanupResources()
{
    CloseSharedMemory();
    DiscardDeviceResources();
    SafeRelease(&g_iconFormat);
    SafeRelease(&g_smallTextFormat);
    SafeRelease(&g_textFormat);
    SafeRelease(&g_dwriteFactory);
    SafeRelease(&g_d2dFactory);
}

void ResetMetersToZero()
{
    for (auto& c : g_columns)
    {
        c.redDisplay = 0.0f;
        c.blueDisplay = 0.0f;
        c.redTarget = 0.0f;
        c.blueTarget = 0.0f;
    }

    InvalidateRect(g_hWnd, nullptr, FALSE);
}

void SetInitialDemoTargets()
{
    g_columns[0].redTarget = 8643.0f;
    g_columns[0].blueTarget = 9044.0f;

    g_columns[1].redTarget = 284.0f;
    g_columns[1].blueTarget = 280.0f;

    g_columns[2].redTarget = 184.0f;
    g_columns[2].blueTarget = 33.0f;
}

void MaybeRefreshDemoTargets()
{
    ++g_demoTick;
    if (g_demoTick % 45 != 0)
    {
        return;
    }

    auto randRange = [](float lo, float hi) -> float
        {
            std::uniform_real_distribution<float> dist(lo, hi);
            return dist(g_rng);
        };

    g_columns[0].redTarget = randRange(3000.0f, 12000.0f);
    g_columns[0].blueTarget = randRange(3000.0f, 12000.0f);

    g_columns[1].redTarget = randRange(80.0f, 900.0f);
    g_columns[1].blueTarget = randRange(80.0f, 900.0f);

    g_columns[2].redTarget = randRange(20.0f, 500.0f);
    g_columns[2].blueTarget = randRange(20.0f, 500.0f);
}

void AnimateMeters()
{
    constexpr float smoothing = 0.13f;

    for (auto& c : g_columns)
    {
        c.redDisplay += (c.redTarget - c.redDisplay) * smoothing;
        c.blueDisplay += (c.blueTarget - c.blueDisplay) * smoothing;

        if (std::fabs(c.redTarget - c.redDisplay) < 0.05f)
            c.redDisplay = c.redTarget;

        if (std::fabs(c.blueTarget - c.blueDisplay) < 0.05f)
            c.blueDisplay = c.blueTarget;
    }
}

void DrawTextCentered(const wchar_t* text, const D2D1_RECT_F& rect, bool useSmallFont = false, bool useIconFont = false)
{
    IDWriteTextFormat* fmt = useIconFont  ? g_iconFormat
                           : useSmallFont ? g_smallTextFormat
                                          : g_textFormat;
    g_renderTarget->DrawTextW(
        text,
        static_cast<UINT32>(wcslen(text)),
        fmt,
        rect,
        g_brush);
}

void DrawVerticalDigits(int value, const D2D1_RECT_F& rect, bool anchorTop)
{
    wchar_t buf[32]{};
    swprintf_s(buf, L"%d", value);

    const float  digitHeight = 18.0f;
    const size_t len         = wcslen(buf);
    const float  totalHeight = static_cast<float>(len) * digitHeight;

    float y = anchorTop ? rect.top + 32.0f
                        : rect.bottom - totalHeight - 32.0f;

    for (size_t i = 0; i < len; ++i)
    {
        wchar_t ch[2] = { buf[i], 0 };
        D2D1_RECT_F r = D2D1::RectF(rect.left, y, rect.right, y + digitHeight);
        DrawTextCentered(ch, r, false);
        y += digitHeight;
    }
}

void FillRoundedRect(const D2D1_RECT_F& rect, float radiusX, float radiusY, const D2D1_COLOR_F& color)
{
    g_brush->SetColor(color);
    g_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, radiusX, radiusY), g_brush);
}

void FillGradientRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& colorTop, const D2D1_COLOR_F& colorBottom)
{
    D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, colorTop    },
        { 1.0f, colorBottom }
    };

    ID2D1GradientStopCollection* pStops = nullptr;
    if (FAILED(g_renderTarget->CreateGradientStopCollection(stops, 2, &pStops)) || !pStops)
        return;

    ID2D1LinearGradientBrush* pGradBrush = nullptr;
    const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {
        D2D1::Point2F(rect.left, rect.top),
        D2D1::Point2F(rect.left, rect.bottom)
    };
    const HRESULT hr = g_renderTarget->CreateLinearGradientBrush(props, pStops, &pGradBrush);
    pStops->Release();

    if (SUCCEEDED(hr) && pGradBrush)
    {
        g_renderTarget->FillRectangle(rect, pGradBrush);
        pGradBrush->Release();
    }
}

void DrawRoundedRect(const D2D1_RECT_F& rect, float radiusX, float radiusY, const D2D1_COLOR_F& color, float stroke = 1.0f)
{
    g_brush->SetColor(color);
    g_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rect, radiusX, radiusY), g_brush, stroke);
}

void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float stroke = 1.0f)
{
    g_brush->SetColor(color);
    g_renderTarget->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush, stroke);
}

void DrawResetButton()
{
    const D2D1_COLOR_F border(0.78f, 0.80f, 0.84f, 1.0f);
    const D2D1_COLOR_F fill(0.22f, 0.40f, 0.86f, 1.0f);
    const D2D1_COLOR_F inner(0.29f, 0.49f, 0.95f, 1.0f);
    const D2D1_COLOR_F text(0.98f, 0.98f, 1.0f, 1.0f);

    FillRoundedRect(g_resetButtonRect, 4.0f, 4.0f, fill);
    DrawRoundedRect(g_resetButtonRect, 4.0f, 4.0f, border, 1.1f);

    D2D1_RECT_F innerRect = D2D1::RectF(
        g_resetButtonRect.left + 1.5f,
        g_resetButtonRect.top + 1.5f,
        g_resetButtonRect.right - 1.5f,
        g_resetButtonRect.bottom - 10.0f);

    FillRoundedRect(innerRect, 3.0f, 3.0f, inner);

    g_brush->SetColor(text);
    DrawTextCentered(L"R", g_resetButtonRect, true);
}

void DrawPinButton()
{
    const D2D1_COLOR_F border(0.78f, 0.80f, 0.84f, 1.0f);
    const D2D1_COLOR_F fill  = g_isPinned
        ? D2D1::ColorF(0.78f, 0.58f, 0.10f, 1.0f)
        : D2D1::ColorF(0.22f, 0.40f, 0.86f, 1.0f);
    const D2D1_COLOR_F inner = g_isPinned
        ? D2D1::ColorF(0.88f, 0.68f, 0.20f, 1.0f)
        : D2D1::ColorF(0.29f, 0.49f, 0.95f, 1.0f);
    const D2D1_COLOR_F text(0.98f, 0.98f, 1.0f, 1.0f);

    FillRoundedRect(g_pinButtonRect, 4.0f, 4.0f, fill);
    DrawRoundedRect(g_pinButtonRect, 4.0f, 4.0f, border, 1.1f);

    D2D1_RECT_F innerRect = D2D1::RectF(
        g_pinButtonRect.left + 1.5f,
        g_pinButtonRect.top + 1.5f,
        g_pinButtonRect.right - 1.5f,
        g_pinButtonRect.bottom - 10.0f);

    FillRoundedRect(innerRect, 3.0f, 3.0f, inner);

    g_brush->SetColor(text);
    DrawTextCentered(L"\xE718", g_pinButtonRect, false, true);
}

void DrawCloseButton()
{
    const D2D1_COLOR_F border(0.78f, 0.80f, 0.84f, 1.0f);
    const D2D1_COLOR_F fill(0.72f, 0.14f, 0.14f, 1.0f);
    const D2D1_COLOR_F inner(0.82f, 0.22f, 0.22f, 1.0f);
    const D2D1_COLOR_F text(0.98f, 0.98f, 1.0f, 1.0f);

    FillRoundedRect(g_closeButtonRect, 4.0f, 4.0f, fill);
    DrawRoundedRect(g_closeButtonRect, 4.0f, 4.0f, border, 1.1f);

    D2D1_RECT_F innerRect = D2D1::RectF(
        g_closeButtonRect.left + 1.5f,
        g_closeButtonRect.top + 1.5f,
        g_closeButtonRect.right - 1.5f,
        g_closeButtonRect.bottom - 10.0f);

    FillRoundedRect(innerRect, 3.0f, 3.0f, inner);

    g_brush->SetColor(text);
    DrawTextCentered(L"X", g_closeButtonRect, true);
}

void DrawMeterColumn(const D2D1_RECT_F& outerRect, const MeterColumn& c)
{
    const D2D1_COLOR_F shellFill(0.92f, 0.92f, 0.94f, 1.0f);
    const D2D1_COLOR_F shellBorder(0.68f, 0.68f, 0.70f, 1.0f);
    const D2D1_COLOR_F topBg(0.98f, 0.98f, 0.99f, 1.0f);
    const D2D1_COLOR_F red(0.72f, 0.16f, 0.12f, 0.95f);
    const D2D1_COLOR_F blue(0.15f, 0.24f, 0.78f, 0.95f);
    const D2D1_COLOR_F tick(0.16f, 0.16f, 0.16f, 0.58f);
    const D2D1_COLOR_F text(0.97f, 0.97f, 0.98f, 1.0f);
    const D2D1_COLOR_F labelText(0.96f, 0.96f, 0.98f, 1.0f);

    D2D1_RECT_F topLabelRect = D2D1::RectF(
        outerRect.left - 8.0f,
        outerRect.top - 28.0f,
        outerRect.right + 8.0f,
        outerRect.top - 6.0f);

    D2D1_RECT_F bottomLabelRect = D2D1::RectF(
        outerRect.left - 10.0f,
        outerRect.bottom + 6.0f,
        outerRect.right + 10.0f,
        outerRect.bottom + 34.0f);

    g_brush->SetColor(labelText);
    DrawTextCentered(c.topLabel, topLabelRect, true);
    DrawTextCentered(c.bottomLabel, bottomLabelRect, true);

    FillRoundedRect(outerRect, 2.0f, 2.0f, shellFill);
    DrawRoundedRect(outerRect, 2.0f, 2.0f, shellBorder, 1.0f);

    D2D1_RECT_F innerRect = D2D1::RectF(
        outerRect.left + 2.5f,
        outerRect.top + 2.5f,
        outerRect.right - 2.5f,
        outerRect.bottom - 2.5f);

    FillRoundedRect(innerRect, 1.5f, 1.5f, topBg);

    const float meterHeight = innerRect.bottom - innerRect.top;
    const float total = c.redDisplay + c.blueDisplay;

    float redRatio = 0.0f;

    if (total > 0.0001f)
    {
        redRatio = c.redDisplay / total;
    }

    const float splitY = innerRect.top + (meterHeight * redRatio);

    D2D1_RECT_F redRect = D2D1::RectF(
        innerRect.left,
        innerRect.top,
        innerRect.right,
        splitY);

    D2D1_RECT_F blueRect = D2D1::RectF(
        innerRect.left,
        splitY,
        innerRect.right,
        innerRect.bottom);

    if (total <= 0.0001f)
    {
        const D2D1_COLOR_F neutral(0.50f, 0.50f, 0.54f, 0.50f);
        g_brush->SetColor(neutral);
        g_renderTarget->FillRectangle(innerRect, g_brush);
    }
    else
    {
        if ((redRect.bottom - redRect.top) > 0.5f)
            FillGradientRect(redRect,
                D2D1::ColorF(0.88f, 0.50f, 0.48f, 0.20f),   // top : faded pink
                D2D1::ColorF(0.65f, 0.10f, 0.08f, 0.95f));  // bottom : deep crimson

        if ((blueRect.bottom - blueRect.top) > 0.5f)
            FillGradientRect(blueRect,
                D2D1::ColorF(0.12f, 0.20f, 0.85f, 0.95f),   // top : deep navy
                D2D1::ColorF(0.05f, 0.08f, 0.50f, 0.55f));  // bottom : dark faded blue
    }

    const int tickCount = 5;
    for (int i = 1; i <= tickCount; ++i)
    {
        const float y = innerRect.top + ((innerRect.bottom - innerRect.top) * i / (tickCount + 1.0f));
        DrawLine(innerRect.left + 10.0f, y, innerRect.right - 10.0f, y, tick, 1.0f);
    }

    DrawLine(
        innerRect.left,
        splitY,
        innerRect.right,
        splitY,
        D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.45f),
        1.0f);

    g_brush->SetColor(text);

    if ((redRect.bottom - redRect.top) > 26.0f)
        DrawVerticalDigits(static_cast<int>(std::lround(c.redDisplay)),  redRect,  false);

    if ((blueRect.bottom - blueRect.top) > 26.0f)
        DrawVerticalDigits(static_cast<int>(std::lround(c.blueDisplay)), blueRect, true);
}

void OnPaint(HWND hWnd)
{
    HRESULT hr = CreateDeviceResources(hWnd);
    if (FAILED(hr))
    {
        return;
    }

    RECT rc{};
    GetClientRect(hWnd, &rc);

    const float width = static_cast<float>(rc.right - rc.left);
    const float height = static_cast<float>(rc.bottom - rc.top);

    g_renderTarget->BeginDraw();
    g_renderTarget->Clear(D2D1::ColorF(0.03f, 0.03f, 0.05f, 1.0f));

    D2D1_RECT_F outerPanel = D2D1::RectF(14.0f, 14.0f, width - 14.0f, height - 14.0f);
    FillRoundedRect(outerPanel, 8.0f, 8.0f, D2D1::ColorF(0.02f, 0.02f, 0.05f, 1.0f));
    DrawRoundedRect(outerPanel, 8.0f, 8.0f, D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f), 1.0f);

    g_resetButtonRect   = D2D1::RectF(22.0f, 22.0f, 43.0f, 43.0f);
    g_pinButtonRect     = D2D1::RectF(width - 66.0f, 22.0f, width - 45.0f, 43.0f);
    g_closeButtonRect   = D2D1::RectF(width - 43.0f, 22.0f, width - 22.0f, 43.0f);
    DrawResetButton();
    DrawPinButton();
    DrawCloseButton();

    const float top = 90.0f;
    const float bottom = height - 48.0f;
    const float colWidth = 48.0f;
    const float gap = 10.0f;
    const float startX = 26.0f;

    const int colOrder[3] = { 0, 2, 1 };
    for (int i = 0; i < 3; ++i)
    {
        const float left = startX + i * (colWidth + gap);
        const D2D1_RECT_F rect = D2D1::RectF(left, top, left + colWidth, bottom);
        DrawMeterColumn(rect, g_columns[colOrder[i]]);
    }

    const D2D1_COLOR_F globalYellow(0.95f, 0.82f, 0.18f, 1.0f);
    const float globalLeft = startX - 8.0f;
    const float globalRight = startX + (3.0f * colWidth) + (2.0f * gap) + 8.0f;
    const float globalY = top + ((bottom - top) * 0.50f);

    DrawLine(globalLeft, globalY, globalRight, globalY, globalYellow, 2.5f);

    hr = g_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        DiscardDeviceResources();
    }
}

void OnResize(UINT width, UINT height)
{
    if (g_renderTarget)
    {
        g_renderTarget->Resize(D2D1::SizeU(width, height));
    }
}

void UpdateDemo()
{
    TryOpenSharedMemory();

    bool usedLive = false;
    if (g_pSharedData)
    {
        const DWORD age = GetTickCount() - g_pSharedData->tickCount;
        if (age < 5000)
        {
            // Seqlock read: snapshot sequence before and after; retry if odd (mid-write).
            const LONG seq1 = g_pSharedData->sequence;
            MemoryBarrier();
            if ((seq1 & 1) == 0)
            {
                const float c0r = g_pSharedData->col0Red;
                const float c0b = g_pSharedData->col0Blue;
                const float c1r = g_pSharedData->col1Red;
                const float c1b = g_pSharedData->col1Blue;
                const float c2r = g_pSharedData->col2Red;
                const float c2b = g_pSharedData->col2Blue;
                MemoryBarrier();
                const LONG seq2 = g_pSharedData->sequence;

                if (seq1 == seq2)
                {
                    g_columns[0].redTarget  = c0r;
                    g_columns[0].blueTarget = c0b;
                    g_columns[1].redTarget  = c1r;
                    g_columns[1].blueTarget = c1b;
                    g_columns[2].redTarget  = c2r;
                    g_columns[2].blueTarget = c2b;
                    usedLive = true;
                }
            }
        }
        else
        {
            CloseSharedMemory();
        }
    }

    if (!usedLive)
    {
        MaybeRefreshDemoTargets();
    }

    AnimateMeters();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

bool PointInRectF(float x, float y, const D2D1_RECT_F& r)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetInitialDemoTargets();
        SetTimer(hWnd, TIMER_ID_ANIMATE, TIMER_INTERVAL_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID_ANIMATE)
        {
            UpdateDemo();
        }
        return 0;

    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        const float fx = static_cast<float>(pt.x);
        const float fy = static_cast<float>(pt.y);
        if (fy < 50.0f
            && !PointInRectF(fx, fy, g_resetButtonRect)
            && !PointInRectF(fx, fy, g_pinButtonRect)
            && !PointInRectF(fx, fy, g_closeButtonRect))
        {
            return HTCAPTION;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    case WM_LBUTTONDOWN:
    {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));

        if (PointInRectF(x, y, g_resetButtonRect))
        {
            ResetMetersToZero();
        }
        else if (PointInRectF(x, y, g_pinButtonRect))
        {
            g_isPinned = !g_isPinned;
            SetWindowPos(hWnd, g_isPinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (PointInRectF(x, y, g_closeButtonRect))
        {
            DestroyWindow(hWnd);
        }
        return 0;
    }

    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_PAINT:
    case WM_DISPLAYCHANGE:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        OnPaint(hWnd);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
    {
        RECT rc{};
        if (GetWindowRect(hWnd, &rc))
        {
            HKEY hKey;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PowerMeter", 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
            {
                const DWORD wx = static_cast<DWORD>(static_cast<LONG>(rc.left));
                const DWORD wy = static_cast<DWORD>(static_cast<LONG>(rc.top));
                const DWORD wp = g_isPinned ? 1u : 0u;
                RegSetValueExW(hKey, L"WindowX",  0, REG_DWORD, reinterpret_cast<const BYTE*>(&wx), sizeof(DWORD));
                RegSetValueExW(hKey, L"WindowY",  0, REG_DWORD, reinterpret_cast<const BYTE*>(&wy), sizeof(DWORD));
                RegSetValueExW(hKey, L"IsPinned", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wp), sizeof(DWORD));
                RegCloseKey(hKey);
            }
        }
        KillTimer(hWnd, TIMER_ID_ANIMATE);
        CleanupResources();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    g_hWnd = nullptr;

    if (FAILED(CreateDeviceIndependentResources()))
    {
        return 0;
    }

    const wchar_t CLASS_NAME[] = L"PowerMeterWindowClass";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_POWERMETER));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

    if (!RegisterClassExW(&wc))
    {
        CleanupResources();
        return 0;
    }

    int startX = CW_USEDEFAULT;
    int startY = CW_USEDEFAULT;
    bool savedPinned = false;
    {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PowerMeter", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
        {
            DWORD val, cb = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"WindowX", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &cb) == ERROR_SUCCESS)
                startX = static_cast<int>(static_cast<LONG>(val));
            cb = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"WindowY", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &cb) == ERROR_SUCCESS)
                startY = static_cast<int>(static_cast<LONG>(val));
            cb = sizeof(DWORD);
            if (RegQueryValueExW(hKey, L"IsPinned", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &cb) == ERROR_SUCCESS)
                savedPinned = (val != 0);
            RegCloseKey(hKey);
        }
        if (startX != CW_USEDEFAULT && startY != CW_USEDEFAULT)
        {
            const POINT pt = { startX, startY };
            if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == nullptr)
            {
                startX = CW_USEDEFAULT;
                startY = CW_USEDEFAULT;
            }
        }
    }

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"Power Meter",
        WS_POPUP,
        startX,
        startY,
        220,
        760,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd)
    {
        CleanupResources();
        return 0;
    }

    g_hWnd = hWnd;

    if (savedPinned)
    {
        g_isPinned = true;
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}