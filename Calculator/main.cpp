#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>

#define IDM_MODE_BASIC    1001
#define IDM_MODE_PROG     1002
#define IDM_EDIT_CLEAR    1003
#define IDM_ALWAYS_ON_TOP 1004
#define IDM_VIEW_HEX      1005
#define IDM_VIEW_DEC      1006
#define IDM_VIEW_OCT      1007
#define IDM_VIEW_BIN      1008

#define IDC_DISPLAY       2000
#define IDC_BITDISPLAY    2001
#define IDC_COMBO_TYPE    2002

#define IDC_BTN_BASE      3000
#define IDC_BTN_PROG_BASE 3100

#define IDC_ACCEL_COPY    4007
#define IDC_ACCEL_PASTE   4008

#define IDI_CALCICON      5000

#define MIN_W       320
#define MIN_H_BASIC 400
#define MIN_H_PROG  520
#define PAD         5

enum class CalcMode { Basic, Programmer };
enum class DataType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Half, Float, Double };
enum class DispBase { Hex, Dec, Oct, Bin };

static const wchar_t* DATA_TYPE_NAMES[] = {
    L"Int 8", L"UInt 8", L"Int 16", L"UInt 16",
    L"Int 32", L"UInt 32", L"Int 64", L"UInt 64",
    L"Float 16", L"Float 32", L"Float 64"
};

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF, f;
    if (exp == 0) {
        if (!mant) { f = sign << 31; }
        else { exp = 1; while (!(mant & 0x400)) { mant <<= 1; exp--; } mant &= 0x3FF; f = (sign << 31) | ((exp + 112) << 23) | (mant << 13); }
    }
    else if (exp == 31) { f = (sign << 31) | 0x7F800000 | (mant << 13); }
    else { f = (sign << 31) | ((exp + 112) << 23) | (mant << 13); }
    float res; memcpy(&res, &f, 4); return res;
}
static uint16_t FloatToHalf(float fv) {
    uint32_t f; memcpy(&f, &fv, 4);
    uint32_t sign = (f >> 31), exp = (f >> 23) & 0xFF, mant = f & 0x7FFFFF;
    if (exp == 0xFF) return (uint16_t)((sign << 15) | 0x7C00 | (mant ? 1 : 0));
    int ne = (int)exp - 127 + 15;
    if (ne >= 31) return (uint16_t)((sign << 15) | 0x7C00);
    if (ne <= 0) { if (ne < -10) return (uint16_t)(sign << 15); mant = (mant | 0x800000) >> (1 - ne); return (uint16_t)((sign << 15) | (mant >> 13)); }
    return (uint16_t)((sign << 15) | (ne << 10) | (mant >> 13));
}

struct CalcState {
    CalcMode  mode = CalcMode::Basic;
    DataType  dtype = DataType::Double;
    DispBase  base = DispBase::Dec;
    std::wstring inputStr;
    std::wstring history;
    bool newInput = true;
    bool justCalc = false;
    double   operand = 0.0;
    uint64_t operandBits = 0;
    wchar_t  op = 0;
    uint64_t rawBits = 0;
    bool alwaysOnTop = false;
};
static CalcState g_calc;

static HWND g_hWnd = nullptr;
static HWND g_hDisplay = nullptr;
static HWND g_hBitDisp = nullptr;
static HWND g_hCombo = nullptr;
static HACCEL g_hAccel = nullptr;
static HWND g_hToolTip = nullptr;
static HMENU g_hMenuMode = nullptr;
static HMENU g_hMenuView = nullptr;
static HMENU g_hMenuEdit = nullptr;

static int g_hoverBit = -1;

struct BtnInfo { HWND hwnd; std::wstring label; int id; };
static std::vector<BtnInfo> g_basicBtns, g_progBtns;

static HFONT g_fontHistory = nullptr;
static HFONT g_fontResult = nullptr;

static std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf); auto pos = p.rfind(L'\\');
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    return p + L"config.ini";
}
static void SaveConfig() {
    std::wstring ini = GetIniPath(); const wchar_t* s = L"Calc";
    RECT r; GetWindowRect(g_hWnd, &r);
    auto wi = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(s, k, std::to_wstring(v).c_str(), ini.c_str());
    };
    wi(L"X", r.left); wi(L"Y", r.top);
    wi(L"W", r.right - r.left); wi(L"H", r.bottom - r.top);
    wi(L"Mode", (int)g_calc.mode);
    wi(L"DType", (int)g_calc.dtype);
    wi(L"Base", (int)g_calc.base);
    wi(L"AOT", g_calc.alwaysOnTop ? 1 : 0);
}
static void LoadConfig() {
    std::wstring ini = GetIniPath(); const wchar_t* s = L"Calc";
    auto gi = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(s, k, d, ini.c_str()); };
    int x = gi(L"X", 100), y = gi(L"Y", 100), w = gi(L"W", 400), h = gi(L"H", 600);
    int minH = (g_calc.mode == CalcMode::Basic) ? MIN_H_BASIC : MIN_H_PROG;
    if (w < MIN_W) w = MIN_W; if (h < minH) h = minH;
    SetWindowPos(g_hWnd, nullptr, x, y, w, h, SWP_NOZORDER);
    g_calc.mode = (CalcMode)gi(L"Mode", 0);
    g_calc.dtype = (DataType)gi(L"DType", 10);
    g_calc.base = (DispBase)gi(L"Base", 1);
    g_calc.alwaysOnTop = gi(L"AOT", 0) != 0;
}

static int BitCount() {
    switch (g_calc.dtype) {
    case DataType::Int8:  case DataType::UInt8:                       return 8;
    case DataType::Int16: case DataType::UInt16: case DataType::Half: return 16;
    case DataType::Int32: case DataType::UInt32: case DataType::Float:return 32;
    default:                                                           return 64;
    }
}
static bool IsFloat() {
    return g_calc.dtype == DataType::Half || g_calc.dtype == DataType::Float || g_calc.dtype == DataType::Double;
}
static bool IsSigned() {
    return g_calc.dtype == DataType::Int8 || g_calc.dtype == DataType::Int16 ||
        g_calc.dtype == DataType::Int32 || g_calc.dtype == DataType::Int64;
}
static double BitsToDouble() {
    switch (g_calc.dtype) {
    case DataType::Int8: { int8_t  v; memcpy(&v, &g_calc.rawBits, 1); return v; }
    case DataType::UInt8: { uint8_t v; memcpy(&v, &g_calc.rawBits, 1); return v; }
    case DataType::Int16: { int16_t  v; memcpy(&v, &g_calc.rawBits, 2); return v; }
    case DataType::UInt16: { uint16_t v; memcpy(&v, &g_calc.rawBits, 2); return v; }
    case DataType::Int32: { int32_t  v; memcpy(&v, &g_calc.rawBits, 4); return v; }
    case DataType::UInt32: { uint32_t v; memcpy(&v, &g_calc.rawBits, 4); return v; }
    case DataType::Int64: { int64_t  v; memcpy(&v, &g_calc.rawBits, 8); return (double)v; }
    case DataType::UInt64: return (double)g_calc.rawBits;
    case DataType::Half: { uint16_t v = (uint16_t)g_calc.rawBits; return HalfToFloat(v); }
    case DataType::Float: { float  v; memcpy(&v, &g_calc.rawBits, 4); return v; }
    case DataType::Double: { double v; memcpy(&v, &g_calc.rawBits, 8); return v; }
    } return 0;
}
static void DoubleToBits(double d) {
    g_calc.rawBits = 0;
    switch (g_calc.dtype) {
    case DataType::Int8: { int8_t  v = (int8_t)d;   memcpy(&g_calc.rawBits, &v, 1); break; }
    case DataType::UInt8: { uint8_t v = (uint8_t)d;  memcpy(&g_calc.rawBits, &v, 1); break; }
    case DataType::Int16: { int16_t  v = (int16_t)d; memcpy(&g_calc.rawBits, &v, 2); break; }
    case DataType::UInt16: { uint16_t v = (uint16_t)d;memcpy(&g_calc.rawBits, &v, 2); break; }
    case DataType::Int32: { int32_t  v = (int32_t)d; memcpy(&g_calc.rawBits, &v, 4); break; }
    case DataType::UInt32: { uint32_t v = (uint32_t)d;memcpy(&g_calc.rawBits, &v, 4); break; }
    case DataType::Int64: { int64_t  v = (int64_t)d; memcpy(&g_calc.rawBits, &v, 8); break; }
    case DataType::UInt64: { g_calc.rawBits = (uint64_t)d; break; }
    case DataType::Half: { float f = (float)d; uint16_t h = FloatToHalf(f); g_calc.rawBits = h; break; }
    case DataType::Float: { float  v = (float)d;  memcpy(&g_calc.rawBits, &v, 4); break; }
    case DataType::Double: { memcpy(&g_calc.rawBits, &d, 8); break; }
    }
    int bc = BitCount();
    if (bc < 64) g_calc.rawBits &= (1ULL << bc) - 1;
}
static void MaskBits() {
    int bc = BitCount();
    if (bc < 64) g_calc.rawBits &= (1ULL << bc) - 1;
}
static double GetCurrentValue() {
    if (!g_calc.inputStr.empty()) return _wtof(g_calc.inputStr.c_str());
    if (g_calc.mode == CalcMode::Programmer) return BitsToDouble();
    return 0.0;
}

static std::wstring FormatBitsInBase(uint64_t val) {
    switch (g_calc.base) {
    case DispBase::Hex: {
        wchar_t buf[32]; int d = BitCount() / 4;
        swprintf_s(buf, L"0x%0*llX", d, (unsigned long long)val);
        return buf;
    }
    case DispBase::Oct: {
        if (!val) return L"o0";
        std::wstring s; uint64_t t = val;
        while (t) { s = (wchar_t)(L'0' + t % 8) + s; t /= 8; }
        return L"o" + s;
    }
    case DispBase::Bin: {
        if (val == 0) return L"b0";
        std::wstring s = L"b";
        bool started = false;
        int bc = BitCount();
        for (int i = bc - 1; i >= 0; i--) {
            bool bit = (val >> i) & 1;
            if (bit) started = true;
            if (started) s += bit ? L'1' : L'0';
        }
        return s;
    }
    default: { 
        uint64_t saved = g_calc.rawBits;
        g_calc.rawBits = val;
        std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
        g_calc.rawBits = saved;
        return ss.str();
    }
    }
}

static std::wstring FormatValue() {
    if (g_calc.mode == CalcMode::Basic)
        return g_calc.inputStr.empty() ? L"0" : g_calc.inputStr;

    uint64_t val = g_calc.rawBits;
    if (g_calc.base == DispBase::Dec) {
        if (!g_calc.inputStr.empty() && !g_calc.justCalc) return g_calc.inputStr;
        std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
        return ss.str();
    }
    if (!g_calc.inputStr.empty() && !g_calc.justCalc) {
        switch (g_calc.base) {
        case DispBase::Hex: return L"0x" + g_calc.inputStr;
        case DispBase::Oct: return L"o" + g_calc.inputStr;
        case DispBase::Bin: return L"b" + g_calc.inputStr;
        default: return g_calc.inputStr;
        }
    }
    return FormatBitsInBase(val);
}

static std::wstring FormatStoredLine() {
    if (g_calc.mode != CalcMode::Programmer) return L"";
    if (g_calc.base == DispBase::Dec) return L"";
    
    std::wostringstream ss;
    if (IsFloat()) {
        double v = BitsToDouble();
        ss << L"Stored: " << std::setprecision(15) << v;
    }
    else {
        if (IsSigned()) {
            switch (g_calc.dtype) {
            case DataType::Int8:  { int8_t  v; memcpy(&v, &g_calc.rawBits, 1); ss << L"Stored: " << (int)v; break; }
            case DataType::Int16: { int16_t v; memcpy(&v, &g_calc.rawBits, 2); ss << L"Stored: " << (int)v; break; }
            case DataType::Int32: { int32_t v; memcpy(&v, &g_calc.rawBits, 4); ss << L"Stored: " << v; break; }
            case DataType::Int64: { int64_t v; memcpy(&v, &g_calc.rawBits, 8); ss << L"Stored: " << v; break; }
            default: ss << L"Stored: " << (int64_t)g_calc.rawBits; break;
            }
        }
        else {
            ss << L"Stored: " << (uint64_t)g_calc.rawBits;
        }
    }
    return ss.str();
}

static bool HasPrecisionLoss(std::wstring& actual) {
    if (!IsFloat() || g_calc.base != DispBase::Dec || g_calc.inputStr.empty()) return false;
    double orig = _wtof(g_calc.inputStr.c_str()), stored = BitsToDouble();
    if (orig != stored) {
        std::wostringstream ss; ss << std::setprecision(15) << stored; actual = ss.str(); return true;
    }
    return false;
}

static LRESULT CALLBACK DisplayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); FillRect(hdc, &rc, wb); DeleteObject(wb);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        HPEN op = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, 0, 0, nullptr); LineTo(hdc, rc.right - 1, 0);
        LineTo(hdc, rc.right - 1, rc.bottom - 1); LineTo(hdc, 0, rc.bottom - 1); LineTo(hdc, 0, 0);
        SelectObject(hdc, op); DeleteObject(pen);
        SetBkMode(hdc, TRANSPARENT);

        int totalH = rc.bottom - rc.top;

        if (g_calc.mode == CalcMode::Programmer) {
            int zoneH = totalH / 4;

            if (g_fontHistory) {
                HFONT of = (HFONT)SelectObject(hdc, g_fontHistory);
                SetTextColor(hdc, RGB(120, 120, 120));
                RECT hr = { rc.left + 8, rc.top + 4, rc.right - 8, rc.top + zoneH };
                DrawTextW(hdc, g_calc.history.c_str(), -1, &hr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, of);
            }
            if (g_fontResult) {
                HFONT of = (HFONT)SelectObject(hdc, g_fontResult);
                SetTextColor(hdc, RGB(0, 0, 0));
                std::wstring val = FormatValue();
                RECT vr = { rc.left + 8, rc.top + zoneH, rc.right - 8, rc.bottom - zoneH };
                DrawTextW(hdc, val.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, of);
            }
            {
                std::wstring stored = FormatStoredLine();
                if (!stored.empty()) {
                    HFONT sf = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                    HFONT of2 = (HFONT)SelectObject(hdc, sf);
                    SetTextColor(hdc, RGB(200, 0, 0));
                    RECT sr = { rc.left + 4, rc.bottom - zoneH, rc.right - 4, rc.bottom - 2 };
                    DrawTextW(hdc, stored.c_str(), -1, &sr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SelectObject(hdc, of2); DeleteObject(sf);
                }
            }
            if (IsFloat() && g_calc.base == DispBase::Dec) {
                std::wstring actual;
                if (HasPrecisionLoss(actual)) {
                    HFONT sf = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                    HFONT of2 = (HFONT)SelectObject(hdc, sf);
                    SetTextColor(hdc, RGB(200, 0, 0));
                    std::wstring w = L"\u2248 " + actual;
                    RECT wr = { rc.left + 4, rc.bottom - zoneH, rc.right - 4, rc.bottom - 2 };
                    DrawTextW(hdc, w.c_str(), -1, &wr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, of2); DeleteObject(sf);
                }
            }
        }
        else {
            if (g_fontHistory) {
                HFONT of = (HFONT)SelectObject(hdc, g_fontHistory);
                SetTextColor(hdc, RGB(120, 120, 120));
                RECT hr = { rc.left + 8, rc.top + 4, rc.right - 8, rc.top + totalH / 2 };
                DrawTextW(hdc, g_calc.history.c_str(), -1, &hr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, of);
            }
            if (g_fontResult) {
                HFONT of = (HFONT)SelectObject(hdc, g_fontResult);
                SetTextColor(hdc, RGB(0, 0, 0));
                std::wstring val = FormatValue();
                RECT vr = { rc.left + 8, rc.top + totalH / 2, rc.right - 8, rc.bottom - 4 };
                DrawTextW(hdc, val.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, of);
            }
        }
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int BitsPerRow() { return (BitCount() <= 8) ? 8 : 16; }
static int BitNumRows() { return BitCount() / BitsPerRow(); }

static COLORREF BitColor(int bi) {
    if (!IsFloat()) return RGB(220, 220, 220);
    if (g_calc.dtype == DataType::Half) {
        if (bi == 15) return RGB(255, 160, 160);
        if (bi >= 10) return RGB(160, 255, 160);
        return RGB(160, 190, 255);              
    }
    if (g_calc.dtype == DataType::Float) {
        if (bi == 31) return RGB(255, 160, 160);
        if (bi >= 23) return RGB(160, 255, 160);
        return RGB(160, 190, 255);
    }
    if (bi == 63) return RGB(255, 160, 160);
    if (bi >= 52) return RGB(160, 255, 160);
    return RGB(160, 190, 255);
}
static std::wstring BitTooltip(int bi) {
    wchar_t buf[80];
    if (!IsFloat()) {
        if (IsSigned() && bi == BitCount() - 1) return L"Sign bit";
        swprintf_s(buf, L"Bit %d  (2^%d)", bi, bi); return buf;
    }
    if (g_calc.dtype == DataType::Half) {
        if (bi == 15) return L"Sign bit";
        if (bi >= 10) { swprintf_s(buf, L"Exponent: 2^%d  (Bias: 15)", bi - 10); return buf; }
        swprintf_s(buf, L"Mantissa: 2^-%d", 10 - bi); return buf;
    }
    if (g_calc.dtype == DataType::Float) {
        if (bi == 31) return L"Sign bit";
        if (bi >= 23) { swprintf_s(buf, L"Exponent: 2^%d  (Bias: 127)", bi - 23); return buf; }
        swprintf_s(buf, L"Mantissa: 2^-%d", 23 - bi); return buf;
    }
    if (bi == 63) return L"Sign bit";
    if (bi >= 52) { swprintf_s(buf, L"Exponent: 2^%d  (Bias: 1023)", bi - 52); return buf; }
    swprintf_s(buf, L"Mantissa: 2^-%d", 52 - bi); return buf;
}

static LRESULT CALLBACK BitDisplayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdcReal = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;
        HDC hdc = CreateCompatibleDC(hdcReal);
        HBITMAP bmp = CreateCompatibleBitmap(hdcReal, W, H);
        HBITMAP obmp = (HBITMAP)SelectObject(hdc, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
        FillRect(hdc, &rc, bg); DeleteObject(bg);

        int bc = BitCount();
        if (bc > 0) {
            int bpr = BitsPerRow(), nrows = BitNumRows();
            const int LABEL_H = 13;
            float rowH = (float)(H - 2 * PAD) / nrows;
            float boxH = rowH - LABEL_H - 2.0f;
            if (boxH < 4) boxH = 4;
            float boxW = (float)(W - 2 * PAD) / bpr;

            int valSz = (int)(boxW < boxH ? boxW : boxH);
            valSz = (int)(valSz * 0.6f);
            if (valSz < 6) valSz = 6; if (valSz > 14) valSz = 14;
            int lblSz = (int)(LABEL_H * 0.75f);
            if (lblSz < 5) lblSz = 5;

            HFONT fLbl = CreateFontW(-lblSz, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Courier New");
            HFONT fVal = CreateFontW(-valSz, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Courier New");
            SetBkMode(hdc, TRANSPARENT);

            for (int row = 0; row < nrows; row++) {
                int startBit = bc - row * bpr - 1;
                float rowY = PAD + row * rowH;

                for (int col = 0; col < bpr; col++) {
                    int bi = startBit - col;
                    if (bi < 0) break;
                    float x = PAD + col * boxW;

                    HFONT of = (HFONT)SelectObject(hdc, fLbl);
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT lr = { (int)x, (int)rowY, (int)(x + boxW), (int)(rowY + LABEL_H) };
                    wchar_t idx[8]; swprintf_s(idx, L"%d", bi);
                    DrawTextW(hdc, idx, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, of);

                    RECT box = { (int)x + 1, (int)(rowY + LABEL_H), (int)(x + boxW - 1), (int)(rowY + LABEL_H + boxH) };
                    COLORREF col_bg = BitColor(bi);
                    if (bi == g_hoverBit) {
                        int r = GetRValue(col_bg) - 35; if (r < 0) r = 0;
                        int g2 = GetGValue(col_bg) - 35; if (g2 < 0) g2 = 0;
                        int b = GetBValue(col_bg) - 35; if (b < 0) b = 0;
                        col_bg = RGB(r, g2, b);
                    }
                    HBRUSH bb = CreateSolidBrush(col_bg); FillRect(hdc, &box, bb); DeleteObject(bb);
                    HPEN pen2 = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
                    HPEN op2 = (HPEN)SelectObject(hdc, pen2);
                    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
                    SelectObject(hdc, op2); DeleteObject(pen2);

                    bool set = (g_calc.rawBits >> bi) & 1;
                    of = (HFONT)SelectObject(hdc, fVal);
                    SetTextColor(hdc, set ? RGB(0, 0, 0) : RGB(170, 170, 170));
                    wchar_t ch[2] = { set ? L'1' : L'0', 0 };
                    DrawTextW(hdc, ch, 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, of);
                }
            }
            DeleteObject(fLbl); DeleteObject(fVal);
        }

        BitBlt(hdcReal, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obmp); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps); return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, bc = BitCount();
        if (bc <= 0) return 0;
        int bpr = BitsPerRow(), nrows = BitNumRows();
        const int LABEL_H = 13;
        float rowH = (float)(rc.bottom - 2 * PAD) / nrows;
        float boxH = rowH - LABEL_H - 2.0f;
        float boxW = (float)(W - 2 * PAD) / bpr;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

        for (int row = 0; row < nrows; row++) {
            float rowY = PAD + row * rowH;
            if (my >= (int)(rowY + LABEL_H) && my < (int)(rowY + LABEL_H + boxH)) {
                int startBit = bc - row * bpr - 1;
                for (int col = 0; col < bpr; col++) {
                    float x = PAD + col * boxW;
                    if (mx >= (int)(x + 1) && mx < (int)(x + boxW - 1)) {
                        int bi = startBit - col;
                        if (bi >= 0) {
                            g_calc.rawBits ^= (1ULL << bi);
                            MaskBits();
                            std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
                            g_calc.inputStr = ss.str(); g_calc.justCalc = true;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            InvalidateRect(g_hDisplay, nullptr, FALSE);
                        }
                        break;
                    }
                }
            }
        }
        SetFocus(g_hWnd); return 0;
    }

    case WM_MOUSEMOVE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, bc = BitCount();
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        int newHover = -1;
        if (bc > 0) {
            int bpr = BitsPerRow(), nrows = BitNumRows();
            const int LABEL_H = 13;
            float rowH = (float)(rc.bottom - 2 * PAD) / nrows;
            float boxW = (float)(W - 2 * PAD) / bpr;
            for (int row = 0; row < nrows && newHover < 0; row++) {
                float rowY = PAD + row * rowH;
                int startBit = bc - row * bpr - 1;
                for (int col = 0; col < bpr; col++) {
                    float x = PAD + col * boxW;
                    if (mx >= (int)x && mx < (int)(x + boxW) && my >= (int)rowY && my < (int)(rowY + rowH)) {
                        newHover = startBit - col;
                        break;
                    }
                }
            }
        }
        if (newHover != g_hoverBit) { g_hoverBit = newHover; InvalidateRect(hwnd, nullptr, FALSE); }
        if (g_hToolTip && newHover >= 0) {
            std::wstring tip = BitTooltip(newHover);
            TOOLINFOW ti = { sizeof(TOOLINFOW) }; ti.hwnd = hwnd; ti.uId = 1; ti.lpszText = (LPWSTR)tip.c_str();
            SendMessageW(g_hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            POINT pt = { mx, my }; ClientToScreen(hwnd, &pt);
            SendMessageW(g_hToolTip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 12, pt.y + 12));
            SendMessageW(g_hToolTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
        }
        else if (g_hToolTip) {
            TOOLINFOW ti = { sizeof(TOOLINFOW) }; ti.hwnd = hwnd; ti.uId = 1;
            SendMessageW(g_hToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverBit = -1;
        if (g_hToolTip) { TOOLINFOW ti = { sizeof(TOOLINFOW) }; ti.hwnd = hwnd; ti.uId = 1; SendMessageW(g_hToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RefreshDisplay() {
    InvalidateRect(g_hDisplay, nullptr, FALSE);
    if (g_hBitDisp) InvalidateRect(g_hBitDisp, nullptr, FALSE);
}
static void CommitInput() {
    if (g_calc.mode == CalcMode::Basic) return;
    if (g_calc.justCalc) return;
    if (g_calc.inputStr.empty()) return;
    if (g_calc.base == DispBase::Dec) {
        double v = _wtof(g_calc.inputStr.c_str());
        DoubleToBits(v);
    }
    else {
        int base = (g_calc.base == DispBase::Hex) ? 16 : (g_calc.base == DispBase::Oct ? 8 : 2);
        g_calc.rawBits = wcstoull(g_calc.inputStr.c_str(), nullptr, base);
        MaskBits();
    }
}

static void PressClear() {
    g_calc.inputStr.clear(); g_calc.history.clear();
    g_calc.op = 0; g_calc.operand = 0; g_calc.operandBits = 0;
    g_calc.rawBits = 0; g_calc.newInput = true; g_calc.justCalc = false;
    RefreshDisplay();
}
static void PressBackspace() {
    if (g_calc.justCalc) return;
    if (!g_calc.inputStr.empty()) {
        g_calc.inputStr.pop_back();
        if (g_calc.mode == CalcMode::Programmer) {
            if (!g_calc.inputStr.empty()) {
                if (g_calc.base == DispBase::Dec) DoubleToBits(_wtof(g_calc.inputStr.c_str()));
                else {
                    int base = (g_calc.base == DispBase::Hex) ? 16 : (g_calc.base == DispBase::Oct ? 8 : 2);
                    g_calc.rawBits = wcstoull(g_calc.inputStr.c_str(), nullptr, base); MaskBits();
                }
            }
            else { g_calc.rawBits = 0; }
        }
        RefreshDisplay();
    }
}
static void PressDigit(wchar_t ch) {
    if (g_calc.mode == CalcMode::Programmer) {
        if (g_calc.base == DispBase::Bin && ch != L'0' && ch != L'1') return;
        if (g_calc.base == DispBase::Oct && (ch < L'0' || ch > L'7')) return;
        if (g_calc.base == DispBase::Dec && (ch < L'0' || ch > L'9')) return;
        if (g_calc.base == DispBase::Hex) {
            bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F') || (ch >= L'a' && ch <= L'f');
            if (!ok) return;
        }
    }
    else {
        if (ch < L'0' || ch > L'9') return;
    }
    if (g_calc.justCalc || g_calc.newInput) { g_calc.inputStr.clear(); g_calc.justCalc = false; g_calc.newInput = false; }
    g_calc.inputStr += ch;
    if (g_calc.mode == CalcMode::Programmer) {
        if (g_calc.base == DispBase::Dec) DoubleToBits(_wtof(g_calc.inputStr.c_str()));
        else {
            int base = (g_calc.base == DispBase::Hex) ? 16 : (g_calc.base == DispBase::Oct ? 8 : 2);
            g_calc.rawBits = wcstoull(g_calc.inputStr.c_str(), nullptr, base); MaskBits();
        }
    }
    RefreshDisplay();
}
static void PressDot() {
    if (g_calc.mode == CalcMode::Programmer && !IsFloat()) return;
    if (g_calc.justCalc || g_calc.newInput) { g_calc.inputStr = L"0"; g_calc.justCalc = false; g_calc.newInput = false; }
    if (g_calc.inputStr.find(L'.') == std::wstring::npos) {
        if (g_calc.inputStr.empty()) g_calc.inputStr = L"0";
        g_calc.inputStr += L'.';
    }
    RefreshDisplay();
}
static void PressPlusMinus() {
    if (g_calc.mode == CalcMode::Basic) {
        double cur = g_calc.inputStr.empty() ? 0.0 : _wtof(g_calc.inputStr.c_str());
        cur = -cur;
        std::wostringstream ss; ss << std::setprecision(15) << cur;
        g_calc.inputStr = ss.str(); g_calc.justCalc = false; RefreshDisplay(); return;
    }
    CommitInput(); double v = -BitsToDouble(); DoubleToBits(v);
    std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
    g_calc.inputStr = ss.str(); g_calc.justCalc = true; RefreshDisplay();
}
static void PressNot() {
    CommitInput();
    int bc = BitCount();
    uint64_t mask = (bc < 64) ? (1ULL << bc) - 1 : 0xFFFFFFFFFFFFFFFFULL;
    g_calc.rawBits = (~g_calc.rawBits) & mask;
    std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
    g_calc.inputStr = ss.str(); g_calc.justCalc = true; RefreshDisplay();
}

static void PressOperator(wchar_t op_char) {
    if (g_calc.mode == CalcMode::Basic) {
        double cur = (!g_calc.inputStr.empty() && !g_calc.justCalc)
            ? _wtof(g_calc.inputStr.c_str()) : g_calc.operand;
        g_calc.operand = cur; g_calc.op = op_char;
        g_calc.newInput = true; g_calc.justCalc = false; g_calc.inputStr.clear();
        std::wostringstream hist; hist << std::setprecision(15) << cur << L" " << op_char;
        g_calc.history = hist.str(); RefreshDisplay(); return;
    }
    CommitInput();
    g_calc.operand = BitsToDouble(); g_calc.operandBits = g_calc.rawBits;
    std::wstring valStr = FormatValue();
    g_calc.op = op_char; g_calc.newInput = true; g_calc.justCalc = false; g_calc.inputStr.clear();
    g_calc.history = valStr + L" " + op_char;
    RefreshDisplay();
}

static void PressEquals() {
    if (g_calc.mode == CalcMode::Basic) {
        double cur = (!g_calc.inputStr.empty() && !g_calc.justCalc)
            ? _wtof(g_calc.inputStr.c_str()) : g_calc.operand;
        double result = cur;
        if (g_calc.op) {
            double a = g_calc.operand;
            switch (g_calc.op) {
            case L'+': result = a + cur; break;
            case L'-': result = a - cur; break;
            case L'*': result = a * cur; break;
            case L'/':
                if (cur == 0) { g_calc.inputStr = L"Error"; g_calc.justCalc = true; g_calc.op = 0; RefreshDisplay(); return; }
                result = a / cur; break;
            case L'%': result = fmod(a, cur); break;
            }
            std::wostringstream h;
            h << std::setprecision(15) << a << L" " << g_calc.op << L" " << cur << L" =";
            g_calc.history = h.str();
        }
        std::wostringstream ss; ss << std::setprecision(15) << result;
        g_calc.inputStr = ss.str(); g_calc.operand = result; g_calc.justCalc = true; g_calc.op = 0;
        RefreshDisplay(); return;
    }

    CommitInput();
    double cur = BitsToDouble();
    uint64_t cur_bits = g_calc.rawBits;
    double result = cur;
    uint64_t a_bits = g_calc.operandBits;

    if (g_calc.op) {
        double a = g_calc.operand;
        bool isBitwise = false;
        switch (g_calc.op) {
        case L'+': result = a + cur; break;
        case L'-': result = a - cur; break;
        case L'*': result = a * cur; break;
        case L'/':
            if (cur == 0) { g_calc.inputStr = L"Error"; g_calc.justCalc = true; g_calc.op = 0; RefreshDisplay(); return; }
            result = a / cur; break;
        case L'%': result = fmod(a, cur); break;
        case L'A': g_calc.rawBits = a_bits & cur_bits; isBitwise = true; break;
        case L'O': g_calc.rawBits = a_bits | cur_bits; isBitwise = true; break;
        case L'X': g_calc.rawBits = a_bits ^ cur_bits; isBitwise = true; break;
        case L'L': g_calc.rawBits = a_bits << (cur_bits & 63); isBitwise = true; break;
        case L'R': g_calc.rawBits = a_bits >> (cur_bits & 63); isBitwise = true; break;
        }
        if (isBitwise) {
            MaskBits(); result = BitsToDouble(); g_calc.history = L"[bitwise] =";
        }
        else {
            g_calc.history = FormatBitsInBase(a_bits) + L" " + g_calc.op + L" " + FormatBitsInBase(cur_bits) + L" =";
            DoubleToBits(result);
        }
    }
    else {
        DoubleToBits(result);
    }
    std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
    g_calc.inputStr = ss.str(); g_calc.justCalc = true; g_calc.op = 0;
    RefreshDisplay();
}

static void SetBitwiseOp(wchar_t code) {
    CommitInput();
    g_calc.operand = BitsToDouble(); g_calc.operandBits = g_calc.rawBits;
    g_calc.op = code; g_calc.newInput = true; g_calc.justCalc = false; g_calc.inputStr.clear();
    g_calc.history = FormatValue() + L" [bitwise]";
    RefreshDisplay();
}

static void DispatchButton(const std::wstring& label) {
    if (label == L"CE") { PressClear();             return; }
    if (label == L"C" && g_calc.mode == CalcMode::Basic) { PressClear(); return; }
    if (label == L"C" && g_calc.mode == CalcMode::Programmer) {
        if (g_calc.base == DispBase::Hex) { PressDigit(L'C'); return; }
        PressClear(); return;
    }
    if (label == L"BS" || label == L"\u232b") { PressBackspace(); return; }
    if (label == L"=") { PressEquals();    return; }
    if (label == L"+" || label == L"-" || label == L"*" || label == L"/" || label == L"%") {
        PressOperator(label[0]); return;
    }
    if (label == L"." || label == L",") { PressDot();      return; }
    if (label == L"+/-" || label == L"\u00b1") { PressPlusMinus(); return; }
    if (label == L"NOT") { PressNot();          return; }
    if (label == L"AND") { SetBitwiseOp(L'A');  return; }
    if (label == L"OR") { SetBitwiseOp(L'O');  return; }
    if (label == L"XOR") { SetBitwiseOp(L'X');  return; }
    if (label == L"LSH") { SetBitwiseOp(L'L');  return; }
    if (label == L"RSH") { SetBitwiseOp(L'R');  return; }
    if (label.size() == 1) {
        wchar_t c = label[0];
        if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F')) { PressDigit(c); return; }
    }
}

struct GridBtn { std::wstring label; int col, row, colSpan; };

static std::vector<GridBtn> GetBasicGrid() {
    return {
        {L"7", 0,0,1},{L"8", 1,0,1},{L"9", 2,0,1},{L"/",3,0,1},
        {L"4", 0,1,1},{L"5", 1,1,1},{L"6", 2,1,1},{L"*",3,1,1},
        {L"1", 0,2,1},{L"2", 1,2,1},{L"3", 2,2,1},{L"-",3,2,1},
        {L"C", 0,3,1},{L"0", 1,3,1},{L".", 2,3,1},{L"+",3,3,1},
        {L"BS",0,4,1},{L"=", 1,4,3},
    };
}

static std::vector<GridBtn> GetProgGrid() {
    return {
        {L"A",   0,0,1},{L"B",  1,0,1},{L"C",   2,0,1},{L"D",  3,0,1},
        {L"E",   0,1,1},{L"F",  1,1,1},
        {L"7",   0,2,1},{L"8",  1,2,1},{L"9",   2,2,1},{L"/",  3,2,1},
        {L"4",   0,3,1},{L"5",  1,3,1},{L"6",   2,3,1},{L"*",  3,3,1},
        {L"1",   0,4,1},{L"2",  1,4,1},{L"3",   2,4,1},{L"-",  3,4,1},
        {L"CE",  0,5,1},{L"0",  1,5,1},{L".",   2,5,1},{L"+",  3,5,1},
        {L"BS",  0,6,1},{L"=",  1,6,3},
    };
}

static void DestroyButtons(std::vector<BtnInfo>& btns) {
    for (auto& b : btns) if (b.hwnd) DestroyWindow(b.hwnd);
    btns.clear();
}
static void CreateButtons(HWND hwnd, std::vector<BtnInfo>& btns, const std::vector<GridBtn>& grid, int idBase) {
    DestroyButtons(btns);
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
    int id = idBase;
    for (auto& g : grid) {
        HWND hb = CreateWindowExW(0, L"BUTTON", g.label.c_str(),
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10,
            hwnd, (HMENU)(INT_PTR)id, hi, nullptr);
        btns.push_back({ hb, g.label, id++ });
    }
}

static void UpdateButtonStates() {
    if (g_calc.mode != CalcMode::Programmer) return;
    for (auto& b : g_progBtns) {
        bool en = true;
        if (b.label.size() == 1) {
            wchar_t c = b.label[0];
            if (c >= L'0' && c <= L'9') {
                switch (g_calc.base) {
                case DispBase::Bin: en = (c == L'0' || c == L'1'); break;
                case DispBase::Oct: en = (c >= L'0' && c <= L'7'); break;
                default: en = true; break;
                }
            }
            else if (c >= L'A' && c <= L'F') {
                en = (g_calc.base == DispBase::Hex);
            }
        }
        if (b.label == L".")   en = IsFloat();
        if (b.label == L"C" && g_calc.mode == CalcMode::Programmer)
            en = (g_calc.base == DispBase::Hex);
        EnableWindow(b.hwnd, en ? TRUE : FALSE);
    }
}

static void UpdateMenuChecks() {
    if (g_hMenuMode) {
        CheckMenuItem(g_hMenuMode, IDM_MODE_BASIC, MF_BYCOMMAND | (g_calc.mode == CalcMode::Basic ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_hMenuMode, IDM_MODE_PROG, MF_BYCOMMAND | (g_calc.mode == CalcMode::Programmer ? MF_CHECKED : MF_UNCHECKED));
    }
    HMENU hBar = GetMenu(g_hWnd);
    if (hBar) {
        HMENU hFile = GetSubMenu(hBar, 0); 
        if (hFile) CheckMenuItem(hFile, IDM_ALWAYS_ON_TOP, MF_BYCOMMAND | (g_calc.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
    }
    if (g_hMenuView) {
        CheckMenuItem(g_hMenuView, IDM_VIEW_HEX, MF_BYCOMMAND | (g_calc.base == DispBase::Hex ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_hMenuView, IDM_VIEW_DEC, MF_BYCOMMAND | (g_calc.base == DispBase::Dec ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_hMenuView, IDM_VIEW_OCT, MF_BYCOMMAND | (g_calc.base == DispBase::Oct ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_hMenuView, IDM_VIEW_BIN, MF_BYCOMMAND | (g_calc.base == DispBase::Bin ? MF_CHECKED : MF_UNCHECKED));
        UINT state = (g_calc.mode == CalcMode::Programmer) ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(g_hMenuView, IDM_VIEW_HEX, MF_BYCOMMAND | state);
        EnableMenuItem(g_hMenuView, IDM_VIEW_DEC, MF_BYCOMMAND | state);
        EnableMenuItem(g_hMenuView, IDM_VIEW_OCT, MF_BYCOMMAND | state);
        EnableMenuItem(g_hMenuView, IDM_VIEW_BIN, MF_BYCOMMAND | state);
    }
}
static void BuildMenu(HWND hwnd) {
    HMENU hBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_ALWAYS_ON_TOP, L"Always on &Top");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, 0, L"E&xit\tAlt+F4");
    AppendMenuW(hBar, MF_POPUP, (UINT_PTR)hFile, L"&File");

    HMENU hEdit = CreatePopupMenu();
    AppendMenuW(hEdit, MF_STRING, IDM_EDIT_CLEAR, L"&Clear\tEsc");
    AppendMenuW(hBar, MF_POPUP, (UINT_PTR)hEdit, L"&Edit");

    HMENU hMode = CreatePopupMenu();
    AppendMenuW(hMode, MF_STRING | MF_CHECKED, IDM_MODE_BASIC, L"&Basic\tCtrl+1");
    AppendMenuW(hMode, MF_STRING, IDM_MODE_PROG, L"&Programmer\tCtrl+2");
    AppendMenuW(hBar, MF_POPUP, (UINT_PTR)hMode, L"&Mode");

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_HEX, L"&Hexadecimal\tCtrl+H");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_DEC, L"&Decimal\tCtrl+D");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_OCT, L"&Octal\tCtrl+O");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_BIN, L"&Binary\tCtrl+B");
    AppendMenuW(hBar, MF_POPUP, (UINT_PTR)hView, L"&View");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, 0, L"&About DevCalculator");
    AppendMenuW(hBar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");

    SetMenu(hwnd, hBar);
    g_hMenuMode = hMode; g_hMenuEdit = hEdit; g_hMenuView = hView;
    UpdateMenuChecks();
}
static void BuildAccel() {
    ACCEL ac[] = {
        {FVIRTKEY | FCONTROL, '1', IDM_MODE_BASIC},
        {FVIRTKEY | FCONTROL, '2', IDM_MODE_PROG},
        {FVIRTKEY,           VK_ESCAPE, IDM_EDIT_CLEAR},
        {FVIRTKEY | FCONTROL, 'H', IDM_VIEW_HEX},
        {FVIRTKEY | FCONTROL, 'D', IDM_VIEW_DEC},
        {FVIRTKEY | FCONTROL, 'O', IDM_VIEW_OCT},
        {FVIRTKEY | FCONTROL, 'B', IDM_VIEW_BIN},
        {FVIRTKEY | FCONTROL, 'C', IDC_ACCEL_COPY},
        {FVIRTKEY | FCONTROL, 'V', IDC_ACCEL_PASTE},
    };
    g_hAccel = CreateAcceleratorTableW(ac, sizeof(ac) / sizeof(ac[0]));
}

static void UpdateFonts(HWND hwnd) {
    if (g_fontHistory) { DeleteObject(g_fontHistory); g_fontHistory = nullptr; }
    if (g_fontResult) { DeleteObject(g_fontResult);  g_fontResult = nullptr; }
    RECT rc; GetClientRect(hwnd, &rc);
    int dispH = (g_calc.mode == CalcMode::Basic) ? (rc.bottom / 4) : (rc.bottom / 5);
    int hSz = std::max(10, dispH / 4);
    int rSz = std::max(14, dispH / 2);
    g_fontHistory = CreateFontW(-hSz, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontResult = CreateFontW(-rSz, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void LayoutBasic(HWND hwnd, RECT& rc) {
    auto& btns = g_basicBtns;
    auto grid = GetBasicGrid();
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int dispH = H / 4;
    int areaTop = dispH + PAD;
    int areaH = H - areaTop - PAD;
    int areaW = W - 2 * PAD;
    int cols = 4, rows = 5;
    int bH = std::max(28, (areaH - (rows - 1) * PAD) / rows);
    int bW = (areaW - (cols - 1) * PAD) / cols;

    if (g_hDisplay) SetWindowPos(g_hDisplay, nullptr, PAD, PAD, W - 2 * PAD, dispH - PAD, SWP_NOZORDER | SWP_NOACTIVATE);

    HDWP dwp = BeginDeferWindowPos((int)btns.size());
    for (int i = 0; i < (int)btns.size() && i < (int)grid.size(); i++) {
        auto& g = grid[i];
        int x = PAD + g.col * (bW + PAD);
        int y = areaTop + g.row * (bH + PAD);
        int w = bW * g.colSpan + (g.colSpan - 1) * PAD;
        if (g.label == L"=" && g.colSpan > 1) w = areaW - g.col * (bW + PAD);
        dwp = DeferWindowPos(dwp, btns[i].hwnd, nullptr, x, y, w, bH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(dwp);
}

static void LayoutProg(HWND hwnd, RECT& rc) {
    auto& btns = g_progBtns;
    auto grid = GetProgGrid();
    int W = rc.right - rc.left, H = rc.bottom - rc.top;

    int dispH = H / 5;
    int nrows = BitNumRows(); if (nrows < 1) nrows = 1;
    int bitDispH = nrows * 34 + 2 * PAD;
    int comboH = 24;
    int btnTop = PAD + dispH + PAD + bitDispH + PAD + comboH + PAD;
    int areaH = H - btnTop - PAD;
    int areaW = W - 2 * PAD;
    int cols = 4, rows = 7;
    int bH = std::max(5, (areaH - (rows - 1) * PAD) / rows);
    int bW = (areaW - (cols - 1) * PAD) / cols;

    if (g_hDisplay) SetWindowPos(g_hDisplay, nullptr, PAD, PAD, W - 2 * PAD, dispH - PAD, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBitDisp) SetWindowPos(g_hBitDisp, nullptr, PAD, PAD + dispH + PAD, W - 2 * PAD, bitDispH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hCombo)   SetWindowPos(g_hCombo, nullptr, PAD, PAD + dispH + PAD + bitDispH + PAD, W - 2 * PAD, 200, SWP_NOZORDER | SWP_NOACTIVATE);

    HDWP dwp = BeginDeferWindowPos((int)btns.size());
    for (int i = 0; i < (int)btns.size() && i < (int)grid.size(); i++) {
        auto& g = grid[i];
        int x = PAD + g.col * (bW + PAD);
        int y = btnTop + g.row * (bH + PAD);
        int w = bW * g.colSpan + (g.colSpan - 1) * PAD;
        if (g.label == L"=" && g.colSpan > 1) w = areaW - g.col * (bW + PAD);
        dwp = DeferWindowPos(dwp, btns[i].hwnd, nullptr, x, y, w, bH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(dwp);
}

static void LayoutAll(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (g_calc.mode == CalcMode::Basic) LayoutBasic(hwnd, rc);
    else                                LayoutProg(hwnd, rc);
}

static void SetBase(DispBase b) {
    CommitInput();
    g_calc.base = b;
    g_calc.inputStr.clear(); g_calc.justCalc = true;
    UpdateMenuChecks();
    UpdateButtonStates();
    RefreshDisplay();
}

static void SwitchMode(CalcMode m) {
    g_calc.mode = m;
    bool isBasic = (m == CalcMode::Basic);
    for (auto& b : g_basicBtns) ShowWindow(b.hwnd, isBasic ? SW_SHOW : SW_HIDE);
    for (auto& b : g_progBtns)  ShowWindow(b.hwnd, isBasic ? SW_HIDE : SW_SHOW);
    if (g_hBitDisp) ShowWindow(g_hBitDisp, isBasic ? SW_HIDE : SW_SHOW);
    if (g_hCombo)   ShowWindow(g_hCombo, isBasic ? SW_HIDE : SW_SHOW);
    RECT wr; GetWindowRect(g_hWnd, &wr);
    int curW = wr.right - wr.left, curH = wr.bottom - wr.top;
    int minH = isBasic ? MIN_H_BASIC : MIN_H_PROG;
    if (curH < minH) SetWindowPos(g_hWnd, nullptr, 0, 0, curW, minH, SWP_NOMOVE | SWP_NOZORDER);
    UpdateMenuChecks();
    UpdateButtonStates();
    UpdateFonts(g_hWnd);
    LayoutAll(g_hWnd);
    RefreshDisplay();
}

static void UpdateComboType() {
    if (!g_hCombo) return;
    int sel = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return;
    g_calc.dtype = (DataType)sel;
    MaskBits();
    std::wostringstream ss; ss << std::setprecision(15) << BitsToDouble();
    g_calc.inputStr = ss.str(); g_calc.justCalc = true;
    UpdateButtonStates();
    LayoutAll(g_hWnd);
    InvalidateRect(g_hBitDisp, nullptr, FALSE);
    RefreshDisplay();
}

static void CopyToClipboard() {
    std::wstring val = FormatValue();
    if (!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    size_t sz = (val.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (hg) { memcpy(GlobalLock(hg), val.c_str(), sz); GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT, hg); }
    CloseClipboard();
}
static void PasteFromClipboard() {
    if (!OpenClipboard(g_hWnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t* s = (wchar_t*)GlobalLock(h);
        if (s) { g_calc.inputStr = s; DoubleToBits(_wtof(s)); g_calc.justCalc = true; RefreshDisplay(); }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        g_hWnd = hwnd;
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);

        WNDCLASSEXW wcd = { sizeof(WNDCLASSEXW) };
        wcd.lpfnWndProc = DisplayProc; wcd.hInstance = hi;
        wcd.lpszClassName = L"CalcDisplay"; wcd.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcd.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wcd);

        WNDCLASSEXW wcb = { sizeof(WNDCLASSEXW) };
        wcb.lpfnWndProc = BitDisplayProc; wcb.hInstance = hi;
        wcb.lpszClassName = L"BitDisplay"; wcb.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcb.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wcb);

        g_hDisplay = CreateWindowExW(0, L"CalcDisplay", L"", WS_VISIBLE | WS_CHILD,
            PAD, PAD, 300, 80, hwnd, (HMENU)IDC_DISPLAY, hi, nullptr);

        g_hBitDisp = CreateWindowExW(0, L"BitDisplay", L"", WS_CHILD,
            PAD, 90, 300, 70, hwnd, (HMENU)IDC_BITDISPLAY, hi, nullptr);

        g_hToolTip = CreateWindowExW(0, TOOLTIPS_CLASS, nullptr, TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            g_hBitDisp, nullptr, hi, nullptr);
        {
            TOOLINFOW ti = { sizeof(TOOLINFOW) };
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti.hwnd = g_hBitDisp; ti.uId = 1; ti.lpszText = LPSTR_TEXTCALLBACKW;
            RECT r = { 0,0,0,0 }; ti.rect = r;
            SendMessageW(g_hToolTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }

        g_hCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            PAD, 90, 200, 200, hwnd, (HMENU)IDC_COMBO_TYPE, hi, nullptr);
        for (auto* n : DATA_TYPE_NAMES) SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)n);
        SendMessageW(g_hCombo, CB_SETCURSEL, (WPARAM)(int)g_calc.dtype, 0);

        CreateButtons(hwnd, g_basicBtns, GetBasicGrid(), IDC_BTN_BASE);
        CreateButtons(hwnd, g_progBtns, GetProgGrid(), IDC_BTN_PROG_BASE);
        for (auto& b : g_progBtns) ShowWindow(b.hwnd, SW_HIDE);

        BuildMenu(hwnd);
        BuildAccel();
        return 0;
    }

    case WM_SIZE:
        UpdateFonts(hwnd);
        LayoutAll(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mm = (LPMINMAXINFO)lParam;
        mm->ptMinTrackSize.x = MIN_W;
        mm->ptMinTrackSize.y = (g_calc.mode == CalcMode::Basic) ? MIN_H_BASIC : MIN_H_PROG;
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_COMBO_TYPE && code == CBN_SELCHANGE) { UpdateComboType(); SetFocus(hwnd); return 0; }

        if (id >= IDC_BTN_BASE && id < IDC_BTN_PROG_BASE) {
            for (auto& b : g_basicBtns) if (b.id == id) { DispatchButton(b.label); SetFocus(hwnd); return 0; }
        }
        if (id >= IDC_BTN_PROG_BASE && id < IDC_BTN_PROG_BASE + 100) {
            for (auto& b : g_progBtns) if (b.id == id) { DispatchButton(b.label); SetFocus(hwnd); return 0; }
        }

        switch (id) {
        case IDM_MODE_BASIC: SwitchMode(CalcMode::Basic);      break;
        case IDM_MODE_PROG:  SwitchMode(CalcMode::Programmer); break;
        case IDM_EDIT_CLEAR: PressClear();                     break;
        case IDM_VIEW_HEX:   if (g_calc.mode == CalcMode::Programmer) SetBase(DispBase::Hex); break;
        case IDM_VIEW_DEC:   if (g_calc.mode == CalcMode::Programmer) SetBase(DispBase::Dec); break;
        case IDM_VIEW_OCT:   if (g_calc.mode == CalcMode::Programmer) SetBase(DispBase::Oct); break;
        case IDM_VIEW_BIN:   if (g_calc.mode == CalcMode::Programmer) SetBase(DispBase::Bin); break;
        case IDM_ALWAYS_ON_TOP:
            g_calc.alwaysOnTop = !g_calc.alwaysOnTop;
            if (g_calc.alwaysOnTop) {
                LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
                SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            else {
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
                SetWindowLong(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            }
            UpdateMenuChecks();
            break;
        case IDC_ACCEL_COPY:  CopyToClipboard();    break;
        case IDC_ACCEL_PASTE: PasteFromClipboard(); break;
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_BACK:   PressBackspace(); return 0;
        case VK_RETURN: PressEquals();    return 0;
        case VK_ESCAPE: PressClear();     return 0;
        case VK_DELETE: PressClear();     return 0;
        }
        return 0;

    case WM_CHAR: {
        wchar_t ch = (wchar_t)wParam;
        if (ch >= L'0' && ch <= L'9') { PressDigit(ch); return 0; }
        if (g_calc.mode == CalcMode::Programmer && g_calc.base == DispBase::Hex) {
            wchar_t up = towupper(ch);
            if (up >= L'A' && up <= L'F') { PressDigit(up); return 0; }
        }
        switch (ch) {
        case L'+': PressOperator(L'+'); return 0;
        case L'-': PressOperator(L'-'); return 0;
        case L'*': PressOperator(L'*'); return 0;
        case L'/': PressOperator(L'/'); return 0;
        case L'=': PressEquals();       return 0;
        case L'.': case L',': PressDot(); return 0;
        case L'c': case L'C':
            if (g_calc.mode == CalcMode::Programmer && g_calc.base == DispBase::Hex) PressDigit(L'C');
            else PressClear();
            return 0;
        }
        return 0;
    }

    case WM_ACTIVATE: {
        bool active = (LOWORD(wParam) != WA_INACTIVE);
        if (g_calc.alwaysOnTop) {
            LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
            SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd, 0, active ? 255 : (BYTE)(255 * 0.7f), LWA_ALPHA);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam; RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1)); return 1;
    }

    case WM_CLOSE:
        SaveConfig();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_fontHistory) { DeleteObject(g_fontHistory); g_fontHistory = nullptr; }
        if (g_fontResult) { DeleteObject(g_fontResult);  g_fontResult = nullptr; }
        if (g_hAccel)      DestroyAcceleratorTable(g_hAccel);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nShowCmd) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcx = { sizeof(WNDCLASSEXW) };
    wcx.lpfnWndProc = WndProc;
    wcx.hInstance = hInstance;
    wcx.lpszClassName = L"DevCalculator";
    wcx.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcx.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcx.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CALCICON));
    wcx.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CALCICON));
    if (!wcx.hIcon) wcx.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wcx.hIconSm) wcx.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    wcx.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wcx);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW,
        L"DevCalculator", L"DevCalculator",
        WS_OVERLAPPEDWINDOW,
        100, 100, 400, 600,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    LoadConfig();

    if (g_calc.alwaysOnTop) {
        LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    if (g_hCombo) SendMessageW(g_hCombo, CB_SETCURSEL, (WPARAM)(int)g_calc.dtype, 0);

    SwitchMode(g_calc.mode);

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_hAccel && TranslateAcceleratorW(hwnd, g_hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}