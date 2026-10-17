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
//---------------------------------------------------------------------
// 文字列エンコーディング自動判定ユーティリティ (UTF-8 / CP932両対応)
//---------------------------------------------------------------------
static std::wstring ToWideStringAuto(const char* str) {
    if (!str || !*str) return L"";
    // 1. まずUTF-8として厳密に変換を試みる
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, nullptr, 0);
    if (len > 0) {
        std::vector<wchar_t> buf(len);
        if (MultiByteToWideChar(CP_UTF8, 0, str, -1, buf.data(), len) > 0) {
            return buf.data();
        }
    }
    // 2. UTF-8で無効文字があった場合はCP932 (Shift-JIS) として変換
    len = MultiByteToWideChar(932, 0, str, -1, nullptr, 0);
    if (len > 0) {
        std::vector<wchar_t> buf(len);
        if (MultiByteToWideChar(932, 0, str, -1, buf.data(), len) > 0) {
            return buf.data();
        }
    }
    return L"";
}

//---------------------------------------------------------------------
// 元素材（カット前）情報取得ヘルパー
//---------------------------------------------------------------------
struct ObjectSourceInfo {
    bool has_source_pos = false;    // 再生位置が設定されているか
    double play_position = 0.0;     // 再生位置 (秒数、0.000sスタート)
    double play_speed = 100.0;      // 再生速度 (%)
    bool has_media_file = false;    // メディアファイルがあるか
    std::wstring file_path;         // メディアファイルパス
    double total_time = 0.0;        // 元ファイルの総時間 (秒)
    int total_frames = 0;           // 元ファイルの総フレーム数
};

static ObjectSourceInfo ResolveObjectSourceInfo(EDIT_SECTION* edit, OBJECT_HANDLE obj) {
    ObjectSourceInfo info;
    if (!edit || !obj) return info;

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int relFrame0Based = (edit->info) ? (edit->info->frame - lf.start) : 0;
    double currentTimelineFrame = (edit->info) ? static_cast<double>(edit->info->frame) : 0.0;

    static const wchar_t* TRACK_NAMES[] = {
        L"再生位置", L"再生開始", L"開始位置", L"開始フレーム", L"位置", L"start", L"pos", L"position"
    };

    // 1. SDKの get_effect_list + get_effect_track_value による全エフェクト走査
    if (edit->get_effect_list) {
        int effectCount = edit->get_effect_list(obj, nullptr, 0);
        if (effectCount > 0) {
            std::vector<EFFECT_HANDLE> effects(effectCount);
            edit->get_effect_list(obj, effects.data(), effectCount);

            for (EFFECT_HANDLE eff : effects) {
                if (!info.has_source_pos && edit->get_effect_track_value) {
                    for (const wchar_t* trkName : TRACK_NAMES) {
                        double val = 0.0;
                        // frame引数として、0.0、相対フレーム、タイムラインフレームの3通りを順に試す
                        if (edit->get_effect_track_value(eff, trkName, 0.0, &val) ||
                            edit->get_effect_track_value(eff, trkName, static_cast<double>(relFrame0Based), &val) ||
                            edit->get_effect_track_value(eff, trkName, currentTimelineFrame, &val)) {
                            info.play_position = val;
                            info.has_source_pos = true;
                            break;
                        }
                    }
                }

                if (edit->get_effect_track_value) {
                    double spd = 100.0;
                    if (edit->get_effect_track_value(eff, L"再生速度", 0.0, &spd) ||
                        edit->get_effect_track_value(eff, L"再生速度", static_cast<double>(relFrame0Based), &spd)) {
                        info.play_speed = spd;
                    }
                }

                if (edit->get_effect_item_value && !info.has_media_file) {
                    LPCSTR fStr = edit->get_effect_item_value(eff, L"file");
                    if (fStr && strlen(fStr) > 0) {
                        std::wstring wpath = ToWideStringAuto(fStr);
                        if (!wpath.empty()) {
                            info.file_path = wpath;
                            info.has_media_file = true;
                        }
                    }
                }

                if (info.has_source_pos && info.has_media_file) {
                    break;
                }
            }
        }
    }

    // 2. フォールバック: get_object_track_value
    if (!info.has_source_pos && edit->get_object_track_value) {
        static const wchar_t* EFFECTS[] = { L"動画ファイル", L"音声ファイル", L"シーン", L"動画ファイル:0", L"音声ファイル:0", L"動画", L"音声" };
        for (const wchar_t* eff : EFFECTS) {
            for (const wchar_t* trkName : TRACK_NAMES) {
                double val = 0.0;
                if (edit->get_object_track_value(obj, eff, trkName, 0.0, &val) ||
                    edit->get_object_track_value(obj, eff, trkName, static_cast<double>(relFrame0Based), &val)) {
                    info.play_position = val;
                    info.has_source_pos = true;

                    double spd = 100.0;
                    if (edit->get_object_track_value(obj, eff, L"再生速度", 0.0, &spd)) {
                        info.play_speed = spd;
                    }
                    break;
                }
            }
            if (info.has_source_pos) break;
        }
    }

    // 3. フォールバック: エイリアス文字列 (get_object_alias) をワイド文字列化して徹底解析
    if (!info.has_source_pos && edit->get_object_alias) {
        LPCSTR rawAlias = edit->get_object_alias(obj);
        if (rawAlias && strlen(rawAlias) > 0) {
            std::wstring aliasW = ToWideStringAuto(rawAlias);
            if (!aliasW.empty()) {
                std::wistringstream stream(aliasW);
                std::wstring line;
                while (std::getline(stream, line)) {
                    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) {
                        line.pop_back();
                    }
                    if (line.empty() || (line[0] == L'[' && line.back() == L']')) continue;

                    size_t eq = line.find(L'=');
                    if (eq != std::wstring::npos) {
                        std::wstring key = line.substr(0, eq);
                        std::wstring val = line.substr(eq + 1);

                        // キー名の前後の空白を除去
                        while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) key.pop_back();
                        size_t kstart = key.find_first_not_of(L" \t");
                        if (kstart != std::wstring::npos) key = key.substr(kstart);

                        if (key == L"再生位置" || key == L"再生開始" || key == L"開始位置" || key == L"開始フレーム") {
                            try {
                                size_t comma = val.find(L',');
                                if (comma != std::wstring::npos) val = val.substr(0, comma);
                                info.play_position = std::stod(val);
                                info.has_source_pos = true;
                            } catch (...) {}
                        } else if (key == L"再生速度") {
                            try {
                                size_t comma = val.find(L',');
                                if (comma != std::wstring::npos) val = val.substr(0, comma);
                                info.play_speed = std::stod(val);
                            } catch (...) {}
                        } else if (key == L"file" && !val.empty() && !info.has_media_file) {
                            info.file_path = val;
                            info.has_media_file = true;
                        }
                    }
                }
            }
        }
    }

    // 4. メディアファイルがある場合、総時間と総フレーム数を取得
    if (info.has_media_file && !info.file_path.empty() && edit->get_media_info) {
        MEDIA_INFO minfo = { 0 };
        if (edit->get_media_info(info.file_path.c_str(), &minfo, sizeof(MEDIA_INFO))) {
            info.total_time = minfo.total_time;
            if (edit->info && edit->info->scale > 0 && edit->info->rate > 0) {
                double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
                info.total_frames = static_cast<int>(std::round(info.total_time * fps));
            }
        }
    }

    return info;
}

// 8. オブジェクト基準: 絶対フレーム数 (カットが行われていないと仮定した素材基準の現在フレーム数)
// 例: 素材A(512f)の前半310f(約10.333s)をカットした素材B(202f)の15フレーム目の場合 -> 310 + 15 = 325フレーム
static void OnCopyObjectAbsoluteFrame(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int relFrame0Based = edit->info->frame - lf.start;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    ObjectSourceInfo sinfo = ResolveObjectSourceInfo(edit, obj);

    // AviUtl2の再生位置は「秒数」(小数点第3位まで)。秒数にFPSを掛けてカットされた前半フレーム数を算出
    double cutFrames = (sinfo.has_source_pos) ? (sinfo.play_position * fps) : 0.0;
    int cutFramesInt = static_cast<int>(std::round(cutFrames));

    double speed = (sinfo.play_speed != 0.0) ? (sinfo.play_speed / 100.0) : 1.0;
    double progressInB = static_cast<double>(relFrame0Based) * speed;

    // カット後オブジェクト内の現在フレーム (1始まり基準)
    int relFrameDisplay = static_cast<int>(std::round(progressInB)) + g_config.frame_origin;

    // カットを行わなかったと仮定した絶対フレーム数 = (カットされたフレーム数) + (カット後オブジェクト内の現在フレーム)
    int finalAbsFrame = cutFramesInt + relFrameDisplay;

    std::wstring text = std::to_wstring(finalAbsFrame);
    CopyToClipboard(text);

    // 通知メッセージ (内訳付き)
    std::wstring detail = L"【オブジェクト基準:絶対フレーム数】\nコピー結果: " + text +
        L"\n\n[内訳: カットされた前半 + 相対フレーム]\n・カットされた前半のフレーム = " + std::to_wstring(cutFramesInt) + L" (再生位置: " + FormatSecondsValue(sinfo.play_position, 3, false) + L"s)" +
        L"\n・オブジェクト基準(カット後)のフレーム = " + std::to_wstring(relFrameDisplay) +
        L"\n・合計 (絶対フレーム数) = " + text;
    NotifyCopyResult(L"【オブジェクト基準:絶対フレーム数】", (g_config.notify_mode == 1) ? detail : text);
}

// 9. オブジェクト基準: 絶対秒数
// 定義: 「オブジェクト基準: 相対秒数」 + 再生位置の開始の秒数
// AviUtl2の「再生位置」は秒数(小数第3位まで)そのもの
static void OnCopyObjectAbsoluteSeconds(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    int relFrame0Based = edit->info->frame - lf.start;
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    ObjectSourceInfo sinfo = ResolveObjectSourceInfo(edit, obj);

    // 1. オブジェクト基準: 相対秒数 (カット後のオブジェクト先頭からの経過秒数)
    double speed = (sinfo.play_speed != 0.0) ? (sinfo.play_speed / 100.0) : 1.0;
    double progressInB = static_cast<double>(relFrame0Based) * speed;
    double relSec = progressInB / fps;

    // 2. 再生位置の開始の秒数 (再生位置トラックバーに表示されている秒数そのもの)
    double startSec = (sinfo.has_source_pos) ? sinfo.play_position : 0.0;

    // 3. オブジェクト基準: 絶対秒数 = (相対秒数) + (再生位置の開始の秒数)
    double absSec = relSec + startSec;

    std::wstring text = FormatSecondsValue(absSec, g_config.sec_precision, g_config.sec_with_unit);
    CopyToClipboard(text);

    // 通知メッセージ (要望通りの内訳付き)
    std::wstring relSecStr = FormatSecondsValue(relSec, g_config.sec_precision, g_config.sec_with_unit);
    std::wstring startSecStr = FormatSecondsValue(startSec, g_config.sec_precision, g_config.sec_with_unit);
    std::wstring detail = L"【オブジェクト基準:絶対秒数】\nコピー結果: " + text +
        L"\n\n[内訳: 相対秒数 + 再生開始秒数]\n・オブジェクト基準: 相対秒数 = " + relSecStr +
        L"\n・再生位置の開始の秒数 = " + startSecStr +
        L"\n・合計 (相対秒数 + 開始秒数) = " + text;
    NotifyCopyResult(L"【オブジェクト基準:絶対秒数】", (g_config.notify_mode == 1) ? detail : text);
}

// 10. オブジェクト：長さ(絶対フレーム数) (カット前の元素材全体の総フレーム数)
// 例: 素材A(512f)の前半310fカットでB(202f)の場合 -> 元の総フレーム数 512 を取得
static void OnCopyObjectAbsoluteLengthFrame(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    ObjectSourceInfo sinfo = ResolveObjectSourceInfo(edit, obj);

    int totalFrames = 0;
    if (sinfo.total_frames > 0) {
        // メディアファイルのメタデータから直接取得できた場合 (512フレーム)
        totalFrames = sinfo.total_frames;
    } else {
        // メディア情報が取れなくても、カットされた前半(再生位置秒数*FPS) + 残った後半の長さから元素材の長さを復元 (310 + 202 = 512)
        int cutFramesInt = static_cast<int>(std::round(sinfo.play_position * fps));
        int currentLength = lf.end - lf.start + 1;
        totalFrames = cutFramesInt + currentLength;
    }

    std::wstring text = std::to_wstring(totalFrames);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト：長さ(絶対フレーム数)】", text);
}

// 11. オブジェクト：長さ(絶対秒数) (カット前の元素材全体の総秒数)
static void OnCopyObjectAbsoluteLengthSeconds(EDIT_SECTION* edit) {
    if (!edit || !edit->info || edit->info->rate <= 0) return;
    OBJECT_HANDLE obj = ResolveTargetObject(edit);
    if (!obj) {
        HWND parent = GetHostAppWindowSafe();
        MessageBoxW(parent, L"対象のオブジェクトが見つかりませんでした。\nオブジェクトをクリックまたは選択して実行してください。", L"TimeAcquisition", MB_OK | MB_ICONWARNING);
        return;
    }

    OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
    double fps = static_cast<double>(edit->info->rate) / static_cast<double>(edit->info->scale);
    ObjectSourceInfo sinfo = ResolveObjectSourceInfo(edit, obj);

    double totalSec = 0.0;
    if (sinfo.total_time > 0.0) {
        totalSec = sinfo.total_time;
    } else {
        double currentLengthSec = static_cast<double>(lf.end - lf.start + 1) / fps;
        totalSec = sinfo.play_position + currentLengthSec;
    }

    std::wstring text = FormatSecondsValue(totalSec, g_config.sec_precision, g_config.sec_with_unit);
    CopyToClipboard(text);
    NotifyCopyResult(L"【オブジェクト：長さ(絶対秒数)】", text);
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
        host->register_object_menu(L"位置・時間コピー\\オブジェクト基準: 絶対フレーム数", OnCopyObjectAbsoluteFrame);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト基準: 絶対秒数", OnCopyObjectAbsoluteSeconds);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト: 長さ(フレーム数)", OnCopyObjectLengthFrame);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト: 長さ(秒数)", OnCopyObjectLengthSeconds);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト：長さ(絶対フレーム数)", OnCopyObjectAbsoluteLengthFrame);
        host->register_object_menu(L"位置・時間コピー\\オブジェクト：長さ(絶対秒数)", OnCopyObjectAbsoluteLengthSeconds);
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
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 絶対フレーム数", OnCopyObjectAbsoluteFrame);
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 絶対秒数", OnCopyObjectAbsoluteSeconds);
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 長さ(絶対フレーム数)", OnCopyObjectAbsoluteLengthFrame);
        host->register_layer_menu(L"位置・時間コピー\\カーソル下オブジェクト: 長さ(絶対秒数)", OnCopyObjectAbsoluteLengthSeconds);
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
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト基準: 絶対フレーム数", OnCopyObjectAbsoluteFrame);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト基準: 絶対秒数", OnCopyObjectAbsoluteSeconds);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト: 長さ(フレーム数)", OnCopyObjectLengthFrame);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト: 長さ(秒数)", OnCopyObjectLengthSeconds);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト：長さ(絶対フレーム数)", OnCopyObjectAbsoluteLengthFrame);
        host->register_edit_menu(L"位置・時間コピー\\オブジェクト：長さ(絶対秒数)", OnCopyObjectAbsoluteLengthSeconds);
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
