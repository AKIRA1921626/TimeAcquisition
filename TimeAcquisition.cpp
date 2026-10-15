// TimeAcquisition.cpp
// AviUtl2用 汎用プラグイン (.aux2)
// 右クリックメニューからタイムライン全体またはオブジェクト基準の現在フレーム数/秒数をクリップボードにコピーするプラグイン

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>

// AviUtl2 SDK ヘッダー
#pragma warning(push)
#pragma warning(disable: 4828)
#include "aviutl2_sdk/plugin2.h"
#pragma warning(pop)

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

//---------------------------------------------------------------------
// プラグイン基本情報
//---------------------------------------------------------------------
#define PLUGIN_NAME         L"TimeAcquisition"
#define PLUGIN_DISPLAY_NAME L"フレーム・時間取得 (TimeAcquisition)"
#define PLUGIN_VERSION      L"v1.0.1"
#define PLUGIN_INFO         L"右クリックメニューから現在のフレーム数や秒数をコピーするプラグイン v1.0.1"

// プラグインテーブル
static COMMON_PLUGIN_TABLE g_common_plugin_table = {
    PLUGIN_DISPLAY_NAME,
    PLUGIN_INFO
};

// グローバル参照
static HOST_APP_TABLE* g_host = nullptr;
static EDIT_HANDLE* g_edit_handle = nullptr;
static HINSTANCE g_hinst = nullptr;

//---------------------------------------------------------------------
// プラグイン設定
//---------------------------------------------------------------------
struct PluginConfig {
    int frame_origin = 1;       // フレーム起算点: 1 = 1始まり(UI表示基準・デフォルト), 0 = 0始まり
    int sec_precision = 3;      // 秒数の小数桁数: 2〜4 (デフォルト: 3桁ミリ秒)
    bool sec_with_unit = false; // 秒数に "s" 単位を付与するか (デフォルト: false = 数値のみ)
    int notify_mode = 0;        // コピー時の通知: 0 = 通知なし(静かにコピー), 1 = メッセージボックス表示
};

static PluginConfig g_config;

// INIファイルパス取得
static std::wstring GetIniPath() {
    wchar_t path[MAX_PATH] = { 0 };
    if (g_hinst) {
        GetModuleFileNameW(g_hinst, path, MAX_PATH);
        wchar_t* p = wcsrchr(path, L'\\');
        if (p) {
            *(p + 1) = L'\0';
            wcscat_s(path, L"TimeAcquisition.ini");
            return path;
        }
    }
    return L"TimeAcquisition.ini";
}

// 設定読み込み
static void LoadConfig() {
    std::wstring ini = GetIniPath();
    g_config.frame_origin   = GetPrivateProfileIntW(L"Config", L"FrameOrigin", 1, ini.c_str());
    g_config.sec_precision  = GetPrivateProfileIntW(L"Config", L"SecPrecision", 3, ini.c_str());
    g_config.sec_with_unit  = GetPrivateProfileIntW(L"Config", L"SecWithUnit", 0, ini.c_str()) != 0;
    g_config.notify_mode    = GetPrivateProfileIntW(L"Config", L"NotifyMode", 0, ini.c_str());
}

// 設定保存
static void SaveConfig() {
    std::wstring ini = GetIniPath();
    WritePrivateProfileStringW(L"Config", L"FrameOrigin", std::to_wstring(g_config.frame_origin).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Config", L"SecPrecision", std::to_wstring(g_config.sec_precision).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Config", L"SecWithUnit", g_config.sec_with_unit ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"NotifyMode", std::to_wstring(g_config.notify_mode).c_str(), ini.c_str());
}

// 親ウィンドウの安全な取得
static HWND GetHostAppWindowSafe() {
    if (g_edit_handle && g_edit_handle->get_host_app_window) {
        HWND hwnd = g_edit_handle->get_host_app_window();
        if (hwnd && IsWindow(hwnd)) return hwnd;
    }
    HWND fg = GetForegroundWindow();
    if (fg && IsWindow(fg)) return fg;
    return nullptr;
}

//---------------------------------------------------------------------
// クリップボードユーティリティ
//---------------------------------------------------------------------
static bool CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();

    size_t bytes = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hGlob) {
        CloseClipboard();
        return false;
    }

    void* pMem = GlobalLock(hGlob);
    if (pMem) {
        memcpy(pMem, text.c_str(), bytes);
        GlobalUnlock(hGlob);
        SetClipboardData(CF_UNICODETEXT, hGlob);
    }

    CloseClipboard();
    return true;
}

// コピー完了時の通知処理
static void NotifyCopyResult(const std::wstring& label, const std::wstring& copiedText) {
    if (g_config.notify_mode == 1) {
        std::wstring msg = label + L"\nクリップボードにコピーしました:\n\n" + copiedText;
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, msg.c_str(), L"TimeAcquisition", MB_OK | MB_ICONINFORMATION);
    }
}

//---------------------------------------------------------------------
// 時間・フレーム計算フォーマッタ
//---------------------------------------------------------------------

// フレーム数の文字列化
static std::wstring FormatFrameValue(int frameIndex, int origin) {
    int val = frameIndex + origin;
    return std::to_wstring(val);
}

// 秒数の文字列化 (例: "12.345" または "12.345s")
static std::wstring FormatSecondsValue(double sec, int precision, bool withUnit) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(precision) << sec;
    if (withUnit) {
        oss << L"s";
    }
    return oss.str();
}

// タイムコード形式の文字列化 (HH:MM:SS.mmm)
static std::wstring FormatTimeCodeValue(double totalSec) {
    bool negative = false;
    if (totalSec < 0.0) {
        negative = true;
        totalSec = -totalSec;
    }

    long long totalMilliseconds = static_cast<long long>(std::round(totalSec * 1000.0));
    long long ms = totalMilliseconds % 1000;
    long long totalSeconds = totalMilliseconds / 1000;
    long long sec = totalSeconds % 60;
    long long totalMinutes = totalSeconds / 60;
    long long min = totalMinutes % 60;
    long long hour = totalMinutes / 60;

    wchar_t buf[64];
    swprintf_s(buf, L"%s%02lld:%02lld:%02lld.%03lld",
               negative ? L"-" : L"",
               hour, min, sec, ms);
    return buf;
}

// オブジェクトハンドルの取得 (フォーカス -> 選択 -> マウス座標の順に検索)
static OBJECT_HANDLE ResolveTargetObject(EDIT_SECTION* edit) {
    if (!edit) return nullptr;

    OBJECT_HANDLE obj = edit->get_focus_object();
    if (obj) return obj;

    if (edit->get_selected_object_num && edit->get_selected_object) {
        if (edit->get_selected_object_num() > 0) {
            obj = edit->get_selected_object(0);
            if (obj) return obj;
        }
    }

    if (edit->get_mouse_layer_frame && edit->find_object) {
        int mLayer = 0, mFrame = 0;
        if (edit->get_mouse_layer_frame(&mLayer, &mFrame)) {
            obj = edit->find_object(mLayer, mFrame);
            if (obj) return obj;
        }
    }

    return nullptr;
}

//---------------------------------------------------------------------
// 各種コピー処理コールバック (EDIT_SECTION* edit)
//---------------------------------------------------------------------

// 1. タイムライン全体: フレーム数
static void OnCopyTimelineFrame(EDIT_SECTION* edit) {
    if (!edit || !edit->info) return;
    std::wstring text = FormatFrameValue(edit->info->frame, g_config.frame_origin);
    CopyToClipboard(text);
    NotifyCopyResult(L"【タイムライン全体 - フレーム数】", text);
}

// 2. タイムライン全体: 秒数
static void OnCopyTimelineSeconds(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    double sec = static_cast<double>(edit->info->frame) / fps;
    std::wstring text = FormatSecondsValue(sec, g_config.sec_precision, g_config.sec_with_unit);
    CopyToClipboard(text);
    NotifyCopyResult(L"【タイムライン全体 - 秒数】", text);
}

// 3. タイムライン全体: タイムコード (HH:MM:SS.mmm)
static void OnCopyTimelineTimeCode(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    double sec = static_cast<double>(edit->info->frame) / fps;
    std::wstring text = FormatTimeCodeValue(sec);
    CopyToClipboard(text);
    NotifyCopyResult(L"【タイムライン全体 - タイムコード】", text);
}

// 4. オブジェクト基準: 相対フレーム数
static void OnCopyObjectRelativeFrame(EDIT_SECTION* edit) {
    if (!edit || !edit->info) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int relFrame = edit->info->frame - lf.start;
    std::wstring text = FormatFrameValue(relFrame, g_config.frame_origin);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト基準 - 相対フレーム数】", text);
}

// 5. オブジェクト基準: 相対秒数
static void OnCopyObjectRelativeSeconds(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int relFrame = edit->info->frame - lf.start;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    double sec = static_cast<double>(relFrame) / fps;
    std::wstring text = FormatSecondsValue(sec, g_config.sec_precision, g_config.sec_with_unit);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト基準 - 相対秒数】", text);
}

// 6. オブジェクト: 長さ (フレーム数)
static void OnCopyObjectLengthFrame(EDIT_SECTION* edit) {
    if (!edit || !edit->info) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int length = lf.end - lf.start + 1;
    std::wstring text = std::to_wstring(length);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト - 長さ(フレーム数)】", text);
}

// 7. オブジェクト: 長さ (秒数)
static void OnCopyObjectLengthSeconds(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int length = lf.end - lf.start + 1;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    double sec = static_cast<double>(length) / fps;
    std::wstring text = FormatSecondsValue(sec, g_config.sec_precision, g_config.sec_with_unit);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト - 長さ(秒数)】", text);
}

//---------------------------------------------------------------------
// 設定ダイアログ GUI 実装 (Win32 API)
//---------------------------------------------------------------------
static const wchar_t* CONFIG_CLASS_NAME = L"AviUtl2_TimeAcquisition_ConfigClass";

enum ConfigControlId {
    IDC_CFG_GRP_FRAME = 100,
    IDC_CFG_RADIO_FRAME_1BASED,
    IDC_CFG_RADIO_FRAME_0BASED,

    IDC_CFG_GRP_SEC,
    IDC_CFG_STATIC_PRECISION,
    IDC_CFG_COMBO_PRECISION,
    IDC_CFG_CHECK_SEC_UNIT,

    IDC_CFG_GRP_NOTIFY,
    IDC_CFG_RADIO_NOTIFY_SILENT,
    IDC_CFG_RADIO_NOTIFY_POPUP,

    IDC_CFG_BTN_OK,
    IDC_CFG_BTN_CANCEL
};

struct ConfigDialogState {
    PluginConfig tempConfig;
    bool saved = false;
};

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ConfigDialogState* state = reinterpret_cast<ConfigDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_CREATE: {
        ConfigDialogState* s = reinterpret_cast<ConfigDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        HFONT hFont = CreateFontW(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
        if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        auto CreateCtrl = [&](LPCWSTR className, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) -> HWND {
            HWND hCtrl = CreateWindowExW(0, className, text,
                WS_CHILD | WS_VISIBLE | style,
                x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
            SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            return hCtrl;
        };

        // 1. フレーム数設定グループ (1.5倍)
        CreateCtrl(L"BUTTON", L"フレーム数の起算点 (表示基準)", WS_GROUP | BS_GROUPBOX, 22, 15, 595, 115, IDC_CFG_GRP_FRAME);
        HWND r1 = CreateCtrl(L"BUTTON", L"1 始まり (UI表示基準: 1, 2, 3...) 【推奨】", BS_AUTORADIOBUTTON | WS_GROUP, 45, 48, 550, 30, IDC_CFG_RADIO_FRAME_1BASED);
        HWND r0 = CreateCtrl(L"BUTTON", L"0 始まり (プログラム/内部基準: 0, 1, 2...)", BS_AUTORADIOBUTTON, 45, 86, 550, 30, IDC_CFG_RADIO_FRAME_0BASED);
        Button_SetCheck(s->tempConfig.frame_origin == 1 ? r1 : r0, BST_CHECKED);

        // 2. 秒数設定グループ (1.5倍)
        CreateCtrl(L"BUTTON", L"秒数の表示形式", WS_GROUP | BS_GROUPBOX, 22, 145, 595, 130, IDC_CFG_GRP_SEC);
        CreateCtrl(L"STATIC", L"小数点桁数:", SS_LEFT, 45, 182, 140, 30, IDC_CFG_STATIC_PRECISION);
        HWND cbPrec = CreateCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 195, 178, 190, 180, IDC_CFG_COMBO_PRECISION);
        ComboBox_AddString(cbPrec, L"2桁 (例: 1.23)");
        ComboBox_AddString(cbPrec, L"3桁 (ミリ秒: 1.234)");
        ComboBox_AddString(cbPrec, L"4桁 (例: 1.2345)");
        int precIndex = (s->tempConfig.sec_precision >= 2 && s->tempConfig.sec_precision <= 4) ? (s->tempConfig.sec_precision - 2) : 1;
        ComboBox_SetCurSel(cbPrec, precIndex);

        HWND chkUnit = CreateCtrl(L"BUTTON", L"秒数の末尾に単位 \"s\" を付与する (例: 1.234s)", BS_AUTOCHECKBOX, 45, 228, 550, 30, IDC_CFG_CHECK_SEC_UNIT);
        Button_SetCheck(chkUnit, s->tempConfig.sec_with_unit ? BST_CHECKED : BST_UNCHECKED);

        // 3. コピー時通知グループ (1.5倍)
        CreateCtrl(L"BUTTON", L"コピー時の通知設定", WS_GROUP | BS_GROUPBOX, 22, 290, 595, 115, IDC_CFG_GRP_NOTIFY);
        HWND rNotif0 = CreateCtrl(L"BUTTON", L"通知なし (クリップボードに静かにコピー) 【快適】", BS_AUTORADIOBUTTON | WS_GROUP, 45, 323, 550, 30, IDC_CFG_RADIO_NOTIFY_SILENT);
        HWND rNotif1 = CreateCtrl(L"BUTTON", L"メッセージボックスでコピー結果を表示する", BS_AUTORADIOBUTTON, 45, 361, 550, 30, IDC_CFG_RADIO_NOTIFY_POPUP);
        Button_SetCheck(s->tempConfig.notify_mode == 1 ? rNotif1 : rNotif0, BST_CHECKED);

        // OK / キャンセル ボタン (1.5倍)
        CreateCtrl(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 335, 425, 135, 40, IDC_CFG_BTN_OK);
        CreateCtrl(L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP, 482, 425, 135, 40, IDC_CFG_BTN_CANCEL);

        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_CFG_BTN_OK) {
            if (state) {
                HWND r1 = GetDlgItem(hwnd, IDC_CFG_RADIO_FRAME_1BASED);
                state->tempConfig.frame_origin = (Button_GetCheck(r1) == BST_CHECKED) ? 1 : 0;

                HWND cbPrec = GetDlgItem(hwnd, IDC_CFG_COMBO_PRECISION);
                int selPrec = ComboBox_GetCurSel(cbPrec);
                state->tempConfig.sec_precision = (selPrec >= 0) ? (selPrec + 2) : 3;

                HWND chkUnit = GetDlgItem(hwnd, IDC_CFG_CHECK_SEC_UNIT);
                state->tempConfig.sec_with_unit = (Button_GetCheck(chkUnit) == BST_CHECKED);

                HWND rNotif1 = GetDlgItem(hwnd, IDC_CFG_RADIO_NOTIFY_POPUP);
                state->tempConfig.notify_mode = (Button_GetCheck(rNotif1) == BST_CHECKED) ? 1 : 0;

                state->saved = true;
            }
            DestroyWindow(hwnd);
            return 0;
        } else if (id == IDC_CFG_BTN_CANCEL || id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 重要: サブダイアログでは絶対に PostQuitMessage(0) を呼んではならない（呼ぶとAviUtl2本体が終了する）
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 設定ダイアログの表示
static void ShowConfigDialog(HWND parentHwnd) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc   = ConfigWndProc;
        wc.hInstance     = g_hinst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = CONFIG_CLASS_NAME;
        RegisterClassExW(&wc);
        registered = true;
    }

    ConfigDialogState state;
    state.tempConfig = g_config;
    state.saved = false;

    int dlgW = 645;
    int dlgH = 525;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;

    if (parentHwnd && IsWindow(parentHwnd)) {
        RECT rcParent;
        GetWindowRect(parentHwnd, &rcParent);
        x = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
        y = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;
    } else {
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        x = (scrW - dlgW) / 2;
        y = (scrH - dlgH) / 2;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CONFIG_CLASS_NAME,
        L"TimeAcquisition 設定",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        parentHwnd, nullptr, g_hinst, &state);

    if (!hwnd) return;

    if (parentHwnd && IsWindow(parentHwnd)) {
        EnableWindow(parentHwnd, FALSE);
    }

    // 安全なモーダルループ (PostQuitMessageに頼らず、IsWindowでウィンドウ破棄を監視)
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, IDC_CFG_BTN_CANCEL, 0);
                continue;
            } else if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, IDC_CFG_BTN_OK, 0);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parentHwnd && IsWindow(parentHwnd)) {
        EnableWindow(parentHwnd, TRUE);
        SetForegroundWindow(parentHwnd);
    }

    if (state.saved) {
        g_config = state.tempConfig;
        SaveConfig();
        MessageBoxW(parentHwnd, L"設定を保存しました。", L"TimeAcquisition", MB_OK | MB_ICONINFORMATION);
    }
}

// 設定メニュー選択時コールバック (AviUtl2のシステムメニュー「設定」用)
static void OnConfigMenu(HWND hwnd, HINSTANCE /*dll_hinst*/) {
    ShowConfigDialog(hwnd);
}

// メニュー（非編集セクション）から設定ダイアログを開くコールバック
static void OnOpenConfigNoEditSection(void* /*param*/) {
    HWND hMain = GetHostAppWindowSafe();
    ShowConfigDialog(hMain);
}

//---------------------------------------------------------------------
// プラグイン登録関数 (RegisterPlugin)
//---------------------------------------------------------------------
#define EXPORT extern "C" __declspec(dllexport)

EXPORT COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &g_common_plugin_table;
}

EXPORT void RegisterPlugin(HOST_APP_TABLE* host) {
    g_host = host;
    LoadConfig();

    if (!host) return;

    if (host->create_edit_handle) {
        g_edit_handle = host->create_edit_handle();
    }

    if (host->set_plugin_information) {
        host->set_plugin_information(PLUGIN_INFO);
    }

    // 1. オブジェクト選択時の右クリックメニュー (register_object_menu)
    if (host->register_object_menu) {
        host->register_object_menu(L"位置・時間コピー\\タイムライン全体: フレーム数", OnCopyTimelineFrame);
        host->register_object_menu(L"位置・時間コピー\\タイムライン全体: 秒数", OnCopyTimelineSeconds);
        host->register_object_menu(L"位置・時間コピー\\タイムライン全体: タイムコード", OnCopyTimelineTimeCode);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト基準: 相対フレーム数", OnCopyObjectRelativeFrame);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト基準: 相対秒数", OnCopyObjectRelativeSeconds);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト: 長さ(フレーム数)", OnCopyObjectLengthFrame);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト: 長さ(秒数)", OnCopyObjectLengthSeconds);
    }
    // 設定メニューは非編集セクションで登録 (排他ロック状態でダイアログを開かないようにするため)
    if (host->register_object_menu_param) {
        host->register_object_menu_param(L"位置・時間コピー\\設定...", nullptr, OnOpenConfigNoEditSection);
    }

    // 2. レイヤー空白部（オブジェクト未選択時）の右クリックメニュー (register_layer_menu)
    if (host->register_layer_menu) {
        host->register_layer_menu(L"位置・時間コピー\\タイムライン全体: フレーム数", OnCopyTimelineFrame);
        host->register_layer_menu(L"位置・時間コピー\\タイムライン全体: 秒数", OnCopyTimelineSeconds);
        host->register_layer_menu(L"位置・時間コピー\\タイムライン全体: タイムコード", OnCopyTimelineTimeCode);
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 相対フレーム数", OnCopyObjectRelativeFrame);
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 相対秒数", OnCopyObjectRelativeSeconds);
    }
    if (host->register_layer_menu_param) {
        host->register_layer_menu_param(L"位置・時間コピー\\設定...", nullptr, OnOpenConfigNoEditSection);
    }

    // 3. 編集メニュー (register_edit_menu) にも登録
    if (host->register_edit_menu) {
        host->register_edit_menu(L"位置・時間コピー\\タイムライン全体: フレーム数", OnCopyTimelineFrame);
        host->register_edit_menu(L"位置・時間コピー\\タイムライン全体: 秒数", OnCopyTimelineSeconds);
        host->register_edit_menu(L"位置・時間コピー\\タイムライン全体: タイムコード", OnCopyTimelineTimeCode);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト基準: 相対フレーム数", OnCopyObjectRelativeFrame);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト基準: 相対秒数", OnCopyObjectRelativeSeconds);
    }
    if (host->register_edit_menu_param) {
        host->register_edit_menu_param(L"位置・時間コピー\\設定...", nullptr, OnOpenConfigNoEditSection);
    }

    // 4. システム設定メニュー (register_config_menu)
    if (host->register_config_menu) {
        host->register_config_menu(L"TimeAcquisition の設定", OnConfigMenu);
    }
}

EXPORT bool InitializePlugin(DWORD /*version*/) {
    return true;
}

EXPORT void UninitializePlugin() {
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hinst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
