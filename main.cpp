#include <windows.h>
#include "input.h"
#include <stdio.h>
#include <mutex>
#include <list>
#include <unordered_map>
#include <algorithm>
#include <new>
#include <commctrl.h>    // コモンコントロール (Up-Down Control)
#include <tchar.h>       // _tcsrchr のために必要
#include <stdint.h>      // uintptr_t のために必要
#include <condition_variable> // condition_variable のために必要

// --- リソースIDの定義
#define IDD_CONFIG 1000
#define IDC_CACHE_SIZE_GB 1001
#define IDC_SPIN_CACHE_SIZE 1002
#define IDC_CHECKBOX_1 1003
#define IDC_CHECKBOX_2 1004
#define IDC_READAHEAD_FRAMES 1005 // 先読みフレーム数入力欄
#define IDC_SPIN_READAHEAD 1006 // 先読みフレーム数スピンボタン
#define IDC_CHECKBOX_3 1007
// 【重要】32bit環境で巨大キャッシュを安定させるため、
// リンカ設定で /LARGEADDRESSAWARE フラグを有効にしてください。

// 共有メモリの定義
#define メモリ名前 L"DangerousMemoryCacher_SM"

// --- グローバル変数 (動的サイズ対応) ---
HANDLE メモリ = NULL;
// 現在の書き込みオフセット (64bit)
ULONGLONG 書き込みオフセット = 0;
// キャッシュメタデータの排他制御用Mutex
std::mutex Mtxキャッシュ;

// --- 設定関連グローバル変数 ---
const char* 設定ファイル名 = "DangerousMemoryCacher設定.ini";
// INIファイルから読み込むキャッシュサイズ (GB単位) - 初期値10GB
int キャッシュサイズGB = 10;
int 先読みフレーム数 = 30;
bool デバッグモードオン = false;
bool 毎フレームデバッグモードオン = false;
bool 出力モード = false;
// 実際のキャッシュサイズ (バイト単位) (64bit)
ULONGLONG 合計キャッシュサイズバイト = (ULONGLONG)60 * 1024 * 1024 * 1024;
const int 最低キャッシュサイズ = 1;
const int 最大キャッシュサイズ = 99;

// ターゲット DLL 関連の構造体定義
using インプットプラグインターブル関数取得 = INPUT_PLUGIN_TABLE * (__stdcall*)(void);
HMODULE 流用インプットプラグイン = NULL;
INPUT_PLUGIN_TABLE* 流用プラグインテーブル = nullptr;

// キャッシュのエントリ構造体
struct ビデオキャッシュエントリー
{
    int フレーム番号; 
    int サイズ;         // フレームサイズ
    ULONGLONG メモリオフセット; // 共有メモリ内のオフセット (ULONGLONG - 64bit)
};

// LRUリストのイテレータを値に持つマップ
// Key: フレーム番号, Value: リスト内のイテレータ
using フレームマップ = std::unordered_map<int, std::list<ビデオキャッシュエントリー>::iterator>;

// ※void*をキーとするため、unordered_mapのKeyはuintptr_tを使用
std::unordered_map<INPUT_HANDLE, std::list<ビデオキャッシュエントリー>>* フレームリストマップ = nullptr;

using FrameMapMap = std::unordered_map<uintptr_t, フレームマップ>;
std::unordered_map<INPUT_HANDLE, フレームマップ>* フレームマップマップ = nullptr;

bool エラー出力済 = false;
bool メモリオーバー = false;

bool g_スレッド終了フラグ = true;
HANDLE g_先読みスレッドハンドル = NULL;
std::unordered_map<INPUT_HANDLE, INPUT_INFO> g_動画情報マップ;
std::unordered_map<INPUT_HANDLE, int> g_要求フレームマップ;
std::unordered_map<INPUT_HANDLE, int> g_現在読み込み中フレームマップ;
std::condition_variable Cv先読み要求;
std::mutex Mtx先読み;
//---------------------------------------------------------------------
//		入力プラグイン構造体定義 (SDKに合わせて修正)
//---------------------------------------------------------------------
INPUT_PLUGIN_TABLE インプットプラグインテーブル = {
    INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,	// フラグ
    "DangerousMemoryCacher",								// プラグイン名
    "動画ファイル (*.mp4;*.avi;*.mkv;*.webm;*.flv)\0*.mp4;*.avi;*.mkv;*.webm;*.flv\0すべてのファイル (*.*)\0*.*\0\0",							// 入力ファイルフィルタ（すべてのファイルを対象とする）
    "\nDangerous Memory Cacher\n動画のキャッシュをRAMに保存して、複数の動画再生をスムーズに行えるようにします。(確実に軽くなるわけではありません)\n仕様→キャッシュが設定のメモリサイズを超えると前のキャッシュを古い順から上書きします\n レジスタスより",		 // プラグインの情報
    func_init,											// DLL開始時に呼ばれる関数へのポインタ
    func_exit,											// DLL終了時に呼ばれる関数へのポインタ
    func_open,											// ファイルをオープンする関数へのポインタ
    func_close,											// ファイルをクローズする関数へのポインタ
    func_info_get,										// ファイルの情報を取得する関数へのポインタ
    func_read_video,									// 画像データを読み込む関数へのポインタ
    func_read_audio,									// 音声データを読み込む関数へのポインタ
    func_is_keyframe,									// キーフレームか調べる関数へのポインタ
    func_config,										// 入力設定のダイアログを要求された時に呼ばれる関数へのポインタ
};

EXTERN_C INPUT_PLUGIN_TABLE __declspec(dllexport)* __cdecl GetInputPluginTable(void)
{
    return &インプットプラグインテーブル;
}

void ログ出力(const char* メッセージ, bool デバッグ, bool 毎秒 = false)
{
    if (毎秒) {
        if (毎フレームデバッグモードオン) {
            printf("DMC:");
            printf(メッセージ);
            printf("\n");
            return;
        }
        else {
            return;
        }
    }
    else {
        if (!デバッグ) {
            printf("DMC:");
            printf(メッセージ);
            printf("\n");
            return;
        }
        else if (デバッグモードオン) {
            printf("DMC:");
            printf(メッセージ);
            printf("\n");
            return;
        }

    }
}

// --- 設定ファイル読み書き関数 ---
void 設定読み込み(void)
{   // DLLのパスを取得し、INIファイルのパスを構築
    char モジュールパス[MAX_PATH];
    if (GetModuleFileNameA(NULL, モジュールパス, MAX_PATH) == 0) {
        ログ出力("モジュールファイル名の取得に失敗しました", false);
        return;
    }
    // パスからファイル名を削除し、プラグイン名+iniでINIファイルを指定
    char* p = strrchr(モジュールパス, '\\');
    if (p) {
        *(p + 1) = '\0'; // フォルダパスまでを切り出す
    }
    char 設定ファイルパス[MAX_PATH];
    snprintf(設定ファイルパス, _countof(設定ファイルパス), "%s%s", モジュールパス, 設定ファイル名);
    // INIファイルから設定を読み込む
    キャッシュサイズGB = GetPrivateProfileIntA(
        "Config",
        "キャッシュサイズGB",
        キャッシュサイズGB, // 初期値
        設定ファイルパス
    );
    先読みフレーム数 = GetPrivateProfileIntA(
        "Config",
        "先読みフレーム数",
        30, // 初期値
        設定ファイルパス
    );
    デバッグモードオン = GetPrivateProfileIntA(
        "Config",
        "デバッグログ",
        false, // 初期値
        設定ファイルパス
    );
    毎フレームデバッグモードオン = GetPrivateProfileIntA(
        "Config",
        "フレーム出力モード",
        false, // 初期値
        設定ファイルパス
    );
    出力モード = GetPrivateProfileIntA(
        "Config",
        "出力中",
        false, // 初期値
        設定ファイルパス
    );
    // 範囲チェックと補正
    if (キャッシュサイズGB < 最低キャッシュサイズ) キャッシュサイズGB = 最低キャッシュサイズ;
    if (キャッシュサイズGB > 最大キャッシュサイズ) キャッシュサイズGB = 最大キャッシュサイズ;
    // バイトサイズに変換 (64bit乗算を保証)
    合計キャッシュサイズバイト = (ULONGLONG)キャッシュサイズGB * 1024 * 1024 * 1024;
    char デバッグメッセージ[128];
    snprintf(デバッグメッセージ, sizeof(デバッグメッセージ),
        "設定読み込み - メモリサイズ(GB)=%d, 合計バイト=%llu.", キャッシュサイズGB, 合計キャッシュサイズバイト);
    ログ出力(デバッグメッセージ, true);
}

void 設定保存(void)
{
    // DLLのパスを取得し、INIファイルのパスを構築する
    char モジュールパス[MAX_PATH];
    if (GetModuleFileNameA(NULL, モジュールパス, MAX_PATH) == 0) { // NULLで呼び出し元Exeのパスを取得
        ログ出力("モジュールファイル名の取得に失敗しました", false);
        return;
    }
    char* p = strrchr(モジュールパス, '\\');
    if (p) {
        *(p + 1) = '\0';
    }
    char 設定ファイルパス[MAX_PATH];
    snprintf(設定ファイルパス, _countof(設定ファイルパス), "%s%s", モジュールパス, 設定ファイル名);
    char キャッシュサイズ文字[16];
    snprintf(キャッシュサイズ文字, _countof(キャッシュサイズ文字), "%d", キャッシュサイズGB);
    char 先読みフレーム数文字[16];
    snprintf(先読みフレーム数文字, _countof(先読みフレーム数文字), "%d", 先読みフレーム数);
    char デバッグモード文字[16];
    snprintf(デバッグモード文字, _countof(デバッグモード文字), "%d", デバッグモードオン);
    char 毎フレームデバッグ文字[16];
    snprintf(毎フレームデバッグ文字, _countof(毎フレームデバッグ文字), "%d", 毎フレームデバッグモードオン);
    char 出力中文字[16];
    snprintf(出力中文字, _countof(出力中文字), "%d", 出力モード);
    // INIファイルに設定を書き込む
    WritePrivateProfileStringA(
        "Config",
        "キャッシュサイズGB",
        キャッシュサイズ文字,
        設定ファイルパス
    );
    WritePrivateProfileStringA(
        "Config",
        "先読みフレーム数",
        先読みフレーム数文字,
        設定ファイルパス
	);
    WritePrivateProfileStringA(
        "Config",
        "デバッグログ",
        デバッグモード文字,
        設定ファイルパス
    );
    WritePrivateProfileStringA(
        "Config",
        "フレーム出力モード",
        毎フレームデバッグ文字,
        設定ファイルパス
    );
    WritePrivateProfileStringA(
        "Config",
        "出力中",
        出力中文字,
        設定ファイルパス
    );
}

BOOL func_exit(void)
{   // 1. スレッド終了を通知
    g_スレッド終了フラグ = true;
    Cv先読み要求.notify_all();
    // 2. スレッドが終了するのを待機
    if (g_先読みスレッドハンドル != NULL) {
        DWORD result = WaitForSingleObject(g_先読みスレッドハンドル, 5000); // 5秒待機
        if (result == WAIT_TIMEOUT) {
            ログ出力("警告: 先読みスレッドがタイムアウト内に終了しませんでした。", false, false);
            // 強制終了はデッドロックやリソースリークの原因になるため、基本は避ける
        }
        // 3. ハンドルを解放
        CloseHandle(g_先読みスレッドハンドル);
        g_先読みスレッドハンドル = NULL;
        ログ出力("先読みスレッドを正常に終了・解放しました。", true, false);
    }
    // 4. ターゲットDLLの終了処理 (最初に実行)
    if (流用プラグインテーブル && 流用プラグインテーブル->func_exit) {
        流用プラグインテーブル->func_exit();
    }
    // 5. ターゲットDLLのライブラリ解放
    if (流用インプットプラグイン) {
        FreeLibrary(流用インプットプラグイン);
        流用インプットプラグイン = NULL;
    }
    // 6. 共有メモリハンドルの解放
    if (メモリ != NULL && メモリ != INVALID_HANDLE_VALUE) {
        ログ出力("メモリを閉じます", true);
        CloseHandle(メモリ);
        メモリ = NULL;
    }
    // 7. メタデータの解放 (最後に実行)
    if (フレームリストマップ) {
        delete フレームリストマップ;
        フレームリストマップ = nullptr;
    }
    if (フレームマップマップ) {
        delete フレームマップマップ;
        フレームマップマップ = nullptr;
    }
    return TRUE;
}

INPUT_HANDLE func_open(LPSTR file)
{   // ターゲットDLLのロード処理 (一度のみ実行)
    if (流用インプットプラグイン == NULL) {
        流用インプットプラグイン = ::LoadLibrary(TEXT("MFVideoReaderPlugin.aui"));
        if (流用インプットプラグイン == NULL) {
            流用インプットプラグイン = ::LoadLibrary(TEXT("plugins/MFVideoReaderPlugin.aui"));
        }
        if (流用インプットプラグイン == NULL) {
            ログ出力("MFVideoReaderPlugin.auiの読み込みに失敗。MFVideoReaderPluginを入れてください。", true);
            return NULL;
        }
        インプットプラグインターブル関数取得 MFVインプットプラグインテーブル = (インプットプラグインターブル関数取得)::GetProcAddress(流用インプットプラグイン, "GetInputPluginTable");
        if (MFVインプットプラグインテーブル) {
            流用プラグインテーブル = MFVインプットプラグインテーブル();
        }
        else {
            ::FreeLibrary(流用インプットプラグイン);
            流用インプットプラグイン = NULL;
            ログ出力("MFVideoReaderPlugin.auiの初期化エラー", false);
            return NULL;
        }
        if (流用プラグインテーブル->func_init) {
            流用プラグインテーブル->func_init();
        }
    }
    // ターゲットDLLの func_open を呼び出し、ファイルハンドルを取得
    INPUT_HANDLE インプットハンドル = 流用プラグインテーブル->func_open(file);
    if (インプットハンドル == NULL) {
        return NULL; // ファイルオープン失敗
    }
    if (出力モード) return インプットハンドル;
    // [CRITICAL FIX] ターゲットihはインプットハンドルを再利用
    INPUT_HANDLE ターゲットih = インプットハンドル;
    // 1. キャッシュコンテキストの初期化
    {
        std::lock_guard<std::mutex> lock(Mtxキャッシュ);
        if (フレームリストマップ->find(インプットハンドル) == フレームリストマップ->end()) {
            (*フレームリストマップ)[インプットハンドル] = std::list<ビデオキャッシュエントリー>();
            (*フレームマップマップ)[インプットハンドル] = フレームマップ();
            ログ出力("INPUT_HANDLE の新しいキャッシュコンテキストが作成されました", true);
        }
    }
    INPUT_INFO 動画情報;
    memset(&動画情報, 0, sizeof(動画情報));
    // 2. 動画情報の取得
    if (流用プラグインテーブル->func_info_get) {
        // ターゲットDLLの func_info_get を呼び出す
        流用プラグインテーブル->func_info_get(ターゲットih, &動画情報);
    }
    else {
        ログ出力("警告: ターゲットDLLに func_info_get がありません。", true, false);
    }
    // 3. マルチ動画対応のためのマップ初期化と登録
    const int total_frames = 動画情報.n;
    // Mtx先読みロック: ihごとのメタデータマップの登録を保護
    {
        std::lock_guard<std::mutex> lock_prefetch(Mtx先読み);
        // 動画情報マップの登録
        g_動画情報マップ[ターゲットih] = 動画情報; // INPUT_INFO全体をコピー
        // 先読みスレッドが処理を開始する最初のフレームを決定 (通常は0)
        g_現在読み込み中フレームマップ[ターゲットih] = 0;
    }
    return インプットハンドル;
}

BOOL func_close(INPUT_HANDLE ih)
{   // キャッシュメタデータの解放
    {
        std::lock_guard<std::mutex> lock(Mtxキャッシュ);
        if (フレームリストマップ && フレームリストマップ->count(ih)) {
            フレームリストマップ->erase(ih);
        }
        if (フレームマップマップ && フレームマップマップ->count(ih)) {
            フレームマップマップ->erase(ih);
        }
    }
    // 先読み関連メタデータの解放
    {
        std::lock_guard<std::mutex> lock(Mtx先読み);
        g_動画情報マップ.erase(ih);
        g_要求フレームマップ.erase(ih);
        g_現在読み込み中フレームマップ.erase(ih);
    }
    if (流用プラグインテーブル && 流用プラグインテーブル->func_close) {
        return 流用プラグインテーブル->func_close(ih);
    }
    return FALSE;
}

BOOL func_info_get(INPUT_HANDLE ih, INPUT_INFO* iip)
{
    if (流用プラグインテーブル && 流用プラグインテーブル->func_info_get) {
        return 流用プラグインテーブル->func_info_get(ih, iip);
    }
    return FALSE;
}

int func_read_audio(INPUT_HANDLE ih, int start, int length, void* buf)
{
    if (流用プラグインテーブル && 流用プラグインテーブル->func_read_audio) {
        return 流用プラグインテーブル->func_read_audio(ih, start, length, buf);
    }
    return 0;
}

// ------------------------------------------------------------------------
// キャッシュへの書き込み処理（別スレッド）
// - メタデータ更新と共有メモリへの実データ書き込みを行う
// ------------------------------------------------------------------------
ULONGLONG フレームキャッシュ(INPUT_HANDLE ih, int frame, ULONGLONG read_bytes, const void* buf)
{
    if (出力モード) return 0;
    ULONGLONG 新オフセット = 0;
    ULONGLONG 残りバイト = read_bytes;
    const char* コピー元 = (const char*)buf;
    char デバッグメッセージ[256];
    bool 読み込み成功 = false;
    if (read_bytes == 0) return 0;
    if (メモリ == NULL || メモリ == INVALID_HANDLE_VALUE) {
        ログ出力("重大なエラー - メモリハンドルが無効です。書き込みをスキップします。", false, true);
        return 0;
    }
    // フェーズ3：メタデータ更新
    {
        std::lock_guard<std::mutex> lock(Mtxキャッシュ);
        // ハンドルihのコンテキストが有効かチェック
        if (!フレームリストマップ || フレームリストマップ->find(ih) == フレームリストマップ->end() ||
            !フレームマップマップ || フレームマップマップ->find(ih) == フレームマップマップ->end()) {
            ログ出力("書き込み中に インプットハンドル のキャッシュコンテキストが見つかりません", false, true);
            return 0;
        }
        auto& フレームリスト = フレームリストマップ->at(ih);
        auto& フレームマップ = フレームマップマップ->at(ih);
        // 1. ラップアラウンド処理 (ULONGLONG同士の比較)
        // キャッシュサイズを超える場合、先頭に戻ってキャッシュをクリア
        if (書き込みオフセット + read_bytes > 合計キャッシュサイズバイト) {
            新オフセット = 0;
            書き込みオフセット = 0;
            フレームリスト.clear();
            フレームマップ.clear();
            ログ出力("キャッシュクリア", true, false);
        }
        else {
            新オフセット = 書き込みオフセット;
        }
        // 2. 新しいエントリの作成と登録
        ビデオキャッシュエントリー 新エントリー = {
            frame,
            (int)read_bytes,
            新オフセット
        };
        // NOTE: フレームが既にキャッシュされている場合、既存の要素を削除してから挿入するロジックが必要だが、
        // 既存の要素検索と削除は重いため、ここでは単純に上書き登録（古い要素はリストには残るが、マップは上書き）し、
        // キャッシュが一周した時にまとめてクリアする方式を維持する。
        auto it = フレームリスト.insert(フレームリスト.begin(), 新エントリー);
        フレームマップ[frame] = it;
        // 3. オフセットを進める
        書き込みオフセット += read_bytes;
    }
    // フェーズ3b：共有メモリへの実データ書き込み
    if (新オフセット + read_bytes > 合計キャッシュサイズバイト) {
        ログ出力("致命的なエラー - メタデータ更新後にキャッシュ書き込みが メモリサイズ を超えました。物理書き込みをスキップします。", false, true);
        return 0;
    }
    // MapViewOfFile のためのシステム情報取得
    SYSTEM_INFO システムインフォ;
    GetSystemInfo(&システムインフォ);
    const ULONGLONG allocGran = (ULONGLONG)システムインフォ.dwAllocationGranularity;
    const SIZE_T 最大ビューサイズ = (1u << 30); // 1GB (1GiB)
    ULONGLONG 現在オフセット = 新オフセット;
    DWORD DW残りバイト = (DWORD)read_bytes; // 32bitに収まることを期待 (read_bytesはULONGLONGだが、フレームサイズは数MBと仮定)
    while (DW残りバイト > 0) {
        const ULONGLONG マップスタートオフセット = 現在オフセット & ~(allocGran - 1ULL);
        const size_t ビュー内オフセット = (size_t)(現在オフセット - マップスタートオフセット);
        SIZE_T このビューの最大バイト数 = 最大ビューサイズ > ビュー内オフセット ? (最大ビューサイズ - ビュー内オフセット) : 0;
        if (このビューの最大バイト数 == 0) {
            ログ出力("ビュー内オフセットが大きすぎます。物理書き込みを中止します。", false, true);
            break;
        }
        SIZE_T 要求 = (SIZE_T)DW残りバイト;
        SIZE_T このビューのバイト = 要求 <= このビューの最大バイト数 ? 要求 : このビューの最大バイト数;
        SIZE_T 必要なマップサイズ = ビュー内オフセット + このビューのバイト;
        LPVOID マップビュー = MapViewOfFile(
            メモリ,
            FILE_MAP_WRITE,
            (DWORD)(マップスタートオフセット >> 32),
            (DWORD)(マップスタートオフセット & 0xffffffff),
            必要なマップサイズ
        );
        if (マップビュー == NULL) {
            DWORD エラーコード = GetLastError();
            snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                " MapViewOfFile (書き込み) が失敗。 エラー=%lu。 残りバイト=%lu", エラーコード, DW残りバイト);
            ログ出力(デバッグメッセージ, false, true);
            break; // 書き込み失敗
        }
        SIZE_T コピー可能な量 = 必要なマップサイズ - ビュー内オフセット;
        SIZE_T 実際にコピーする量 = (SIZE_T)DW残りバイト;
        if (実際にコピーする量 > コピー可能な量) 実際にコピーする量 = コピー可能な量;
        // memcpy_sの第2引数にDestAvailable（宛先バッファの最大サイズ）を渡すのが正しい
        memcpy_s((char*)マップビュー + ビュー内オフセット, コピー可能な量, コピー元, 実際にコピーする量);
        UnmapViewOfFile(マップビュー);
        // 進める
        コピー元 += 実際にコピーする量;
        現在オフセット += 実際にコピーする量;
        DW残りバイト -= (DWORD)実際にコピーする量; // DWORDで減算
    }
    if (DW残りバイト == 0) {
        読み込み成功 = true;
        snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
            "先読み:%d", frame);
        ログ出力(デバッグメッセージ, false, true);
    }
    return 読み込み成功 ? read_bytes : 0;
}

// ------------------------------------------------------------------------
// 先読みスレッドのメイン関数
// ------------------------------------------------------------------------
DWORD WINAPI プリフェッチ別スレッド(LPVOID lpParam)
{
    if (出力モード) return 0;
    char デバッグメッセージ[256];
    const size_t 最大バッファサイズ = 4 * 1024 * 1024; // 4MB
    std::vector<char> バッファーベクター(最大バッファサイズ);
    void* バッファ = バッファーベクター.data();
    while (!g_スレッド終了フラグ) {
        INPUT_HANDLE 対象ih = NULL;
        int 要求フレーム = -1;
        int 総フレーム数 = -1;
        // 1. 先読み要求のチェックと待機
        {
            std::unique_lock<std::mutex> lock(Mtx先読み);
            Cv先読み要求.wait(lock, [&] { return g_スレッド終了フラグ || !g_要求フレームマップ.empty(); });
            if (g_スレッド終了フラグ) break; // 終了指示
            // 処理対象の ih を決定
            if (!g_要求フレームマップ.empty()) {
                auto it = g_要求フレームマップ.begin();
                対象ih = it->first;
                要求フレーム = it->second;
                // 処理対象の ih の要求をマップから削除し、他の ih の要求は残す
                g_要求フレームマップ.erase(it);
                // 動画情報と現在の読み込み位置を取得
                if (g_動画情報マップ.count(対象ih)) {
                    総フレーム数 = g_動画情報マップ.at(対象ih).n;
                }
                // 現在読み込み中のフレームを要求フレームに設定（ジャンプに対応）
                g_現在読み込み中フレームマップ[対象ih] = 要求フレーム;
            }
            else {
                continue; // マップが空なら再度待機へ
            }
        } // Mtx先読み lock 解除
        if (対象ih == NULL || 総フレーム数 <= 0) {
            continue; // 無効な状態
        }
        // 2. 先読み処理ループ
        // ターゲットDLLの func_read_video はスレッドセーフではない可能性があるため、
        // 連続で呼び出しすぎると問題が起きる可能性があるが、ここではオリジナルのロジックを維持する。
        const int 最大先読みフレーム数 = 先読みフレーム数;
        int 現在フレーム = g_現在読み込み中フレームマップ[対象ih];
        int 書き込みフレーム位置 = 0;
        // 連続10フレームの先読みを行う
        for (int i = 0; i < 最大先読みフレーム数 && !g_スレッド終了フラグ; ++i) {
            int frame_to_read = 現在フレーム + i;
            // a. 動画の終端チェック
            if (frame_to_read >= 総フレーム数) {
                break;
            }
            // b. キャッシュヒットチェック（競合を防ぐため Mtxキャッシュ を使用）
            {
                std::lock_guard<std::mutex> lock(Mtxキャッシュ);
                if (フレームマップマップ && フレームマップマップ->count(対象ih) &&
                    フレームマップマップ->at(対象ih).count(frame_to_read)) {
                    書き込みフレーム位置++;
                    continue; // 次のフレームへ
                }
            }
            // c. デコードとキャッシュ書き込み（フェーズ2 + フェーズ3）
            ULONGLONG 読み込みバイト = 0;
            if (流用プラグインテーブル && 流用プラグインテーブル->func_read_video) {
                // ターゲット DLL によるデコード（I/Oが発生する重い処理）
                // バッファーベクター のサイズ (4MB) を超えるフレームは処理できない点に注意
                読み込みバイト = 流用プラグインテーブル->func_read_video(対象ih, frame_to_read, バッファ);
                // d. デコード結果をキャッシュに書き込む
                if (読み込みバイト > 0) {
                    フレームキャッシュ(対象ih, frame_to_read, 読み込みバイト, バッファ);
                    書き込みフレーム位置++;
                }
                else {
                    snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                        "フレーム=%d のデコードに失敗しました。先読みを停止。", frame_to_read);
                    ログ出力(デバッグメッセージ, false, true);
                    break; // デコード失敗は先読みを停止
                }
            }
            else {
                ログ出力("ターゲットDLLの func_read_video が無効です。先読みスレッドを停止します。", true, false);
                break;
            }
        } // 先読み for ループ終了
        // 3. 次の先読み開始フレームを更新 (ロックが必要)
        if (書き込みフレーム位置 > 0) {
            std::lock_guard<std::mutex> lock(Mtx先読み);
            g_現在読み込み中フレームマップ[対象ih] = 現在フレーム + 書き込みフレーム位置;
        }
    } 
    return 0;
}

void プリフェッチトリガー(INPUT_HANDLE ih, int base_frame)
{
    int 次のフレーム = base_frame + 1;
    char デバッグメッセージ[256];
    {
        std::lock_guard<std::mutex> lock(Mtx先読み);
        // ターゲット DLL から動画情報が取得できているかチェック
        if (g_動画情報マップ.find(ih) == g_動画情報マップ.end()) {
            ログ出力("警告: 先読みトリガー時に動画情報が見つかりません。", false, true);
            return;
        }
        g_要求フレームマップ[ih] = 次のフレーム;
        // Seek発生時など、現在の読み込み位置より要求フレームが手前の場合、読み込み位置をリセット
        // (g_現在読み込み中フレームマップ[ih]はスレッドが今読み込んでいるフレームを指す)
        if (g_現在読み込み中フレームマップ.count(ih) && g_現在読み込み中フレームマップ[ih] > 次のフレーム) {
            g_現在読み込み中フレームマップ[ih] = 次のフレーム;
        }
        Cv先読み要求.notify_one();
    }
}

int func_read_video(INPUT_HANDLE ih, int frame, void* buf)
{
    ULONGLONG 読み込みバイト = 0;
    ULONGLONG オフセット = 0;
    bool キャッシュヒット = false;
    char デバッグメッセージ[256];
    bool メモリ有 = (メモリ != NULL && メモリ != INVALID_HANDLE_VALUE);
    // メモリハンドルが無効な場合、再オープンを試行
    if (!メモリオーバー && !メモリ有 && !出力モード) {
        // メモリは func_init で作成されるため、ここでは既存のメモリをオープン
        メモリ = OpenFileMappingW(
            FILE_MAP_ALL_ACCESS, // READWRITEでも可
            FALSE,
            メモリ名前
        );
        if (メモリ == NULL) {
            DWORD エラーコード = GetLastError();
            snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                "メモリにアクセスできませんでした。エラーコード=%lu.\nキャッシュからの読み取りをスキップします。", エラーコード);
            ログ出力(デバッグメッセージ, false, true);
        }
        else {
            メモリ有 = true;
        }
    }
    // ------------------------------------------------------------------------
    // 【フェーズ1：キャッシュからの読み出し】
    // ------------------------------------------------------------------------
    if (メモリ有 && !メモリオーバー && !出力モード) {
        {
            std::lock_guard<std::mutex> lock(Mtxキャッシュ);
            if (フレームマップマップ && フレームマップマップ->count(ih)) {
                auto& フレームマップ = フレームマップマップ->at(ih);
                auto& フレームリスト = フレームリストマップ->at(ih);
                if (フレームマップ.count(frame)) {
                    // キャッシュヒット
                    auto it = フレームマップ.at(frame);
                    読み込みバイト = it->サイズ;
                    オフセット = it->メモリオフセット; // ULONGLONG
                    キャッシュヒット = true;
                }
            }
        }
        // ------------------------------------------------------------------------
        // フェーズ1b：キャッシュからのデータコピー
        // ------------------------------------------------------------------------
        if (キャッシュヒット) {
            // MapViewOfFile のためのシステム情報取得
            SYSTEM_INFO システムインフォ;
            GetSystemInfo(&システムインフォ);
            const ULONGLONG allocGran = (ULONGLONG)システムインフォ.dwAllocationGranularity;
            const SIZE_T 最大ビューサイズ = (1u << 30); // 1GB
            ULONGLONG 現在オフセット = オフセット;
            int 残りバイト = (int)読み込みバイト;
            char* コピー先 = (char*)buf;
            bool 読み込み失敗 = false;
            while (残りバイト > 0) {
                // アラインしたマップ開始オフセット
                const ULONGLONG マップスタートオフセット = 現在オフセット & ~(allocGran - 1ULL);
                const size_t ビュー内オフセット = (size_t)(現在オフセット - マップスタートオフセット);
                // マップ要求サイズ
                SIZE_T このビューの最大バイト数 = 最大ビューサイズ > ビュー内オフセット ? (最大ビューサイズ - ビュー内オフセット) : 0;
                if (このビューの最大バイト数 == 0) {
                    snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                        " ビュー内オフセットが大きすぎます (%zu)。 読み取りを中止します。", ビュー内オフセット);
                    ログ出力(デバッグメッセージ, false, true);
                    読み込み失敗 = true;
                    break;
                }
                SIZE_T 要求 = (SIZE_T)残りバイト;
                SIZE_T このビューのバイト = 要求 <= このビューの最大バイト数 ? 要求 : このビューの最大バイト数;
                SIZE_T 必要なマップサイズ = ビュー内オフセット + このビューのバイト;
                LPVOID マップビュー = MapViewOfFile(
                    メモリ,
                    FILE_MAP_READ,
                    (DWORD)(マップスタートオフセット >> 32),
                    (DWORD)(マップスタートオフセット & 0xffffffff),
                    必要なマップサイズ
                );
                if (マップビュー == NULL) {
                    DWORD エラーコード = GetLastError();
                    snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                        " MapViewOfFile が キャッシュヒットで失敗しました。 エラー=%lu。 フォールバックします。", エラーコード);
                    ログ出力(デバッグメッセージ, false, true);
                    // MapViewOfFileが失敗した場合、強制的に読み込み失敗として処理を継続
                    読み込み失敗 = true;
                    キャッシュヒット = false;
                    break;
                }
                //安全にその量だけコピー
                SIZE_T コピーバイト = 必要なマップサイズ - ビュー内オフセット; // Mapped Viewの残りのサイズ
                if ((SIZE_T)残りバイト < コピーバイト) コピーバイト = (SIZE_T)残りバイト; // 実際にコピーする量
                // memcpy_s の第2引数は dest サイズ (バッファ の残りのサイズ: (size_t)残りバイト)
                memcpy_s(コピー先, (size_t)残りバイト, (char*)マップビュー + ビュー内オフセット, コピーバイト);
                UnmapViewOfFile(マップビュー);
                コピー先 += コピーバイト;
                現在オフセット += コピーバイト;
                残りバイト -= (int)コピーバイト;
            } // while
            if (!読み込み失敗) {
                snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                    "キャッシュ有:%d", frame);
                ログ出力(デバッグメッセージ, false, true);
                プリフェッチトリガー(ih, frame);
                return (int)読み込みバイト; // 全て読めたため、ここで終了
            }
            else {
                // 読み込み失敗のため、キャッシュヒット処理を諦めてフェーズ2へフォールバック
                キャッシュヒット = false;
            }
        }
        // キャッシュから読み込めなかった場合のみログ出力
        if (!キャッシュヒット) {
            snprintf(デバッグメッセージ, _countof(デバッグメッセージ),
                " %dフレーム目-キャッシュ無し", frame);
            ログ出力(デバッグメッセージ, false, true);
        }
    }
    // ------------------------------------------------------------------------
    // フェーズ2：ターゲットDLL呼び出
    // - キャッシュから読み込めなかった場合、ターゲット DLL へデコードを要求する
    // ------------------------------------------------------------------------
    if (流用プラグインテーブル && 流用プラグインテーブル->func_read_video) 読み込みバイト = 流用プラグインテーブル->func_read_video(ih, frame, buf);
    else 読み込みバイト = 0;
    // ------------------------------------------------------------------------
    // フェーズ3：キャッシュへの書き込み (WriteFrameToCacheへ委譲)
    // - 読み込み成功後、キャッシュへの書き込みが許可されていれば専用関数に委譲する
    // ------------------------------------------------------------------------
    if (読み込みバイト > 0 && メモリ有 && !メモリオーバー && !出力モード) {
        // フレームキャッシュ() はメタデータ更新と物理データ書き込みの両方を処理
        フレームキャッシュ(ih, frame, 読み込みバイト, buf);
        プリフェッチトリガー(ih, frame);
    }
    return (int)読み込みバイト;
}

int func_is_keyframe(INPUT_HANDLE ih, int frame)
{
    if (流用プラグインテーブル && 流用プラグインテーブル->func_is_keyframe) {
        return 流用プラグインテーブル->func_is_keyframe(ih, frame);
    }
    return 0;
}

BOOL CALLBACK 設定ダイアログプロシージャ(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
    {   // UpDownコントロールの初期化
        HWND hSpin = GetDlgItem(hwnd, IDC_SPIN_CACHE_SIZE);
        SendMessage(hSpin, UDM_SETRANGE, 0, MAKELPARAM(最大キャッシュサイズ, 最低キャッシュサイズ)); // Max, Min
        SendMessage(GetDlgItem(hwnd, IDC_SPIN_READAHEAD), UDM_SETRANGE, 0, MAKELPARAM(99999, 5)); // Max, Min
        // 現在の設定値を表示
        SetDlgItemInt(hwnd, IDC_CACHE_SIZE_GB, キャッシュサイズGB, FALSE);
        SetDlgItemInt(hwnd, IDC_READAHEAD_FRAMES, 先読みフレーム数, FALSE);
        CheckDlgButton(hwnd, IDC_CHECKBOX_1, デバッグモードオン ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_CHECKBOX_2, 毎フレームデバッグモードオン ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_CHECKBOX_3, 出力モード ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDOK:
        {
            BOOL ソース;
            int 新メモリサイズ = GetDlgItemInt(hwnd, IDC_CACHE_SIZE_GB, &ソース, FALSE);
            先読みフレーム数 = GetDlgItemInt(hwnd, IDC_READAHEAD_FRAMES, &ソース, FALSE);
            if (ソース) {
                // 範囲チェック
                if (新メモリサイズ < 最低キャッシュサイズ) 新メモリサイズ = 最低キャッシュサイズ;
                if (新メモリサイズ > 最大キャッシュサイズ) 新メモリサイズ = 最大キャッシュサイズ;
                キャッシュサイズGB = 新メモリサイズ;
                デバッグモードオン = (IsDlgButtonChecked(hwnd, IDC_CHECKBOX_1) == BST_CHECKED);
                毎フレームデバッグモードオン = (IsDlgButtonChecked(hwnd, IDC_CHECKBOX_2) == BST_CHECKED);
                出力モード = (IsDlgButtonChecked(hwnd, IDC_CHECKBOX_3) == BST_CHECKED);
                ログ出力("設定保存", true, false);
                if (出力モード) ログ出力("動画出力処理のためキャッシュ機能は無効になります。", true, false);
                設定保存();
                // 共有メモリのサイズ変更は再起動が必要であることを通知
                MessageBoxW(hwnd,
                    L"設定を保存しました。キャッシュサイズの変更を反映するには、AviUtlを再起動してください。\n動画出力モードは反映されます。",
                    L"DangerousMemoryCacher", MB_OK | MB_ICONINFORMATION);
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int func_config(HWND hwnd, HINSTANCE hinstance)
{   // 設定ダイアログのリソースをロードして表示
    DialogBox(
        hinstance,
        MAKEINTRESOURCE(IDD_CONFIG),
        hwnd,
        設定ダイアログプロシージャ
    );
    return 0;
}

BOOL func_init(void)
{   // 1. 設定の読み込みとサイズ決定
    設定読み込み();
    ログ出力("初期化を開始", true);
    // 2. メタデータマップの初期化
    try {
        if (フレームリストマップ == nullptr) {
            フレームリストマップ = new std::unordered_map<INPUT_HANDLE, std::list<ビデオキャッシュエントリー>>();
        }
        if (フレームマップマップ == nullptr) {
            フレームマップマップ = new std::unordered_map<INPUT_HANDLE, フレームマップ>();
        }
    }
    catch (const std::bad_alloc& e) {
        ログ出力("メモリ割り当てエラー", false);
        return FALSE;
    }
    catch (...) {
        ログ出力("初期化中になんらかの例外(エラー)が発生しました", false);
        return FALSE;
    }
    // 3. 共有メモリの作成またはオープン
    // ULONGLONGを上位・下位32ビットに分割して、CreateFileMappingWに渡す (64bitサイズ対応)
    DWORD 上位32ビット = (DWORD)(合計キャッシュサイズバイト >> 32);
    DWORD 下位32ビット = (DWORD)(合計キャッシュサイズバイト & 0xffffffff);
    char デバッグメッセージ[128];
    snprintf(デバッグメッセージ, sizeof(デバッグメッセージ),
        "メモリ作成 %I64d", 合計キャッシュサイズバイト);
    ログ出力(デバッグメッセージ, true);
    // 既存の共有メモリハンドルをクローズ ( func_init が二重に呼ばれた場合への対策)
    if (メモリ != NULL && メモリ != INVALID_HANDLE_VALUE) {
        CloseHandle(メモリ);
        メモリ = NULL;
    }
    メモリ = CreateFileMappingW(
        INVALID_HANDLE_VALUE, // 物理ファイルなし
        NULL,                 // セキュリティ属性
        PAGE_READWRITE,       // 読み書き可能なページ
        上位32ビット,
        下位32ビット,
        メモリ名前    // オブジェクト名
    );
    if (メモリ == NULL) {
        DWORD DWエラー = GetLastError();
        char デバッグメッセージ[128];
        snprintf(デバッグメッセージ, sizeof(デバッグメッセージ),
            "CreateFileMapping に失敗しました エラー内容=%lu", DWエラー);
        ログ出力(デバッグメッセージ, false);
        MessageBoxW(NULL,
            L"メモリの作成に失敗しました。メモリ割り当てサイズを適切にしてください。\n以降フレームはキャッシュされません。\n※システムの都合上このメッセージは複数回表示されます。",
            L"DangerousMemoryCacher", MB_OK | MB_ICONINFORMATION);
        メモリオーバー = true;
        return FALSE;
    }
    // 既存のメモリを使用する場合、g_current_offset はリセット (キャッシュはクリアと見なす)
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ログ出力("共有メモリは既に存在します。既存のハンドルを使用します", true);
        書き込みオフセット = 0; // 共有メモリの内容が不明なため、オフセットをリセットして上書きを開始
    }
    ログ出力("メモリ作成成功", true);
    // 4. 先読みスレッドの起動
    g_スレッド終了フラグ = false;
    g_先読みスレッドハンドル = CreateThread(NULL, 0, プリフェッチ別スレッド, NULL, 0, NULL);
    if (g_先読みスレッドハンドル == NULL) {
        ログ出力("先読みスレッドの起動に失敗しました。", false, false);
        return FALSE;
    }
    ログ出力("先読みスレッドを正常に起動しました。", true, false);
    return TRUE;
}