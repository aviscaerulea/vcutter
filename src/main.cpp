// std::min / std::max と windows.h の min / max マクロが衝突しないよう
// 全 include より前に定義する
#define NOMINMAX
#include <windows.h>
#include <DbgHelp.h>
#include <shellapi.h>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageLogContext>
#include <QMutex>
#include <QString>
#include <QThread>
#include "Config.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SingleInstance.h"
#include "StartupTrace.h"

namespace {

// ログハンドラ
// Qt メッセージのうち Warning / Critical / Fatal のみを exe と同フォルダの avply.log に書き出す。
// Debug / Info はログ膨張を避けて除外する。起動ごとにファイルを上書きリセットし、
// QMutex で複数スレッドからの同時書き込みを直列化する。
// OutputDebugString 経路（VS デバッガ表示）は GUI thread からは全レベル、非 GUI thread からは Warning 以上のみ通す。
void avplyMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    // QtMsgType は連番ではない（QtInfoMsg=4 が QtFatalMsg=3 より大きい）ため明示列挙する
    const bool shouldLog = (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);

    if (shouldLog) {
        static QMutex s_mutex;
        static QFile  s_logFile;

        QMutexLocker locker(&s_mutex);

        if (!s_logFile.isOpen()) {
            // ロングパス（MAX_PATH 超）対応と DRY のため Config::exeDirectory を流用する
            const QString dir = Config::exeDirectory();
            s_logFile.setFileName(dir + "/avply.log");
            // 開けなかった場合は isOpen() が false のまま。次の if で書き込みをスキップする
            (void)s_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        }

        if (s_logFile.isOpen()) {
            const char* level = [type]() -> const char* {
                switch (type) {
                case QtWarningMsg:  return "WRN";
                case QtCriticalMsg: return "ERR";
                case QtFatalMsg:    return "FTL";
                default:            return "UNK";
                }
            }();
            const QString line = QString("[%1] [%2] %3:%4 - %5\n")
                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
                .arg(QLatin1String(level))
                .arg(ctx.file ? QLatin1String(ctx.file) : QLatin1String("?"))
                .arg(ctx.line)
                .arg(msg);
            s_logFile.write(line.toUtf8());
            s_logFile.flush();
        }
    }

    // Qt デフォルト経路（OutputDebugString）への出力。
    // GUI thread からの呼び出しは全レベル継続するが、非 GUI thread（特に HighPriority な
    // AudioWorker の audio thread）からの Debug / Info は OutputDebugString の同期 I/O が
    // 数 ms ブロックして sink underrun の引き金になり得るため、Warning 以上のみ通す
    const QCoreApplication* coreApp = QCoreApplication::instance();
    const bool isMainThread = coreApp && (QThread::currentThread() == coreApp->thread());
    const bool isVerbose = (type == QtDebugMsg || type == QtInfoMsg);
    if (isMainThread || !isVerbose) {
        const QString formatted = qFormatLogMessage(type, ctx, msg);
        OutputDebugStringW(reinterpret_cast<const wchar_t*>(formatted.utf16()));
    }
}

// QApplication 構築前にコマンドライン第 1 引数を Unicode 安全に取得する
// argv は MSVCRT が CP932 でナロー化したものなので、CP932 範囲外の文字
// （中国語・絵文字等）が含まれるパスは取得できない。
// GetCommandLineW + CommandLineToArgvW で UTF-16 から直接取得する
QString firstArgumentUnicode()
{
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return QString();
    QString result;
    if (wargc > 1) result = QString::fromWCharArray(wargv[1]);
    LocalFree(wargv);
    return result;
}

// 未処理例外フィルタ
// クラッシュ時に %TEMP% へ minidump（avply_crash.dmp）を書き出す。
// この関数はスタック破壊後に呼ばれ得るため、ヒープ確保（Qt / CRT）を避けて
// Win32 API とスタック上のバッファのみで完結させる。DbgHelp は遅延ロードせず、
// MiniDumpWithIndirectlyReferencedMemory でクラッシュ地点周辺のヒープも含める。
LONG WINAPI avplyCrashHandler(EXCEPTION_POINTERS* ep)
{
    // ダンプ出力先を %TEMP%\avply_crash.dmp に組み立てる（パスもスタック上で完結させる）
    // GetTempPathW は末尾に区切りを付けて返す。失敗時はカレントディレクトリへフォールバックする
    wchar_t path[MAX_PATH] = {0};
    const DWORD tlen = GetTempPathW(MAX_PATH, path);
    if (tlen == 0 || tlen > MAX_PATH) {
        path[0] = L'\0';
    }
    wcsncat_s(path, MAX_PATH, L"avply_crash.dmp", _TRUNCATE);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {0};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        // クラッシュ地点が参照するヒープも含めて原因変数を追えるようにする
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
            MiniDumpWithThreadInfo);

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          file, type, &mei, nullptr, nullptr);
        CloseHandle(file);
    }

    // 通常のクラッシュ処理（WER への通知）も継続させる
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int main(int argc, char* argv[])
{
    // クラッシュ時の minidump 生成を最優先で登録する
    // 以降の初期化で SEH 例外（アクセス違反等）が起きても avply_crash.dmp を残せるよう
    // main 冒頭で設定する。abort / std::terminate 経由は SEH を通らないため捕捉対象外
    SetUnhandledExceptionFilter(avplyCrashHandler);

    // 起動計測（AVPLY_STARTUP_TRACE 設定時のみ有効）。Config::load より前に置き、
    // 見出し行の「プロセス生成〜main」に DLL ロード等の main 前コストだけを載せる
    StartupTrace::init();
    StartupTrace::mark("main");

    // SetWindowsHookEx 経由のサードパーティ DLL 注入を遮断する
    // DisplayFusion 等のウィンドウフック DLL が注入されてクラッシュするケースに対処する
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY epPolicy = {};
    epPolicy.DisableExtensionPoints = TRUE;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &epPolicy, sizeof(epPolicy))) {
        qWarning("DLL 注入遮断ポリシーの設定に失敗しました（error=%lu）。", GetLastError());
    }

    // FFmpeg バックエンドを明示的に固定する
    // 音声経路（QAudioBufferOutput + SoundTouch）と HW デコーダ優先順位指定
    // （QT_FFMPEG_DECODING_HW_DEVICE_TYPES）が FFmpeg バックエンド前提のため
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    // FFmpeg バックエンドの HW デコード優先順位を avply.toml から取得して反映する。
    // QApplication 構築前に qputenv する必要があるため早期ロードする
    // （Config::load() は exe ディレクトリ取得を Win32 API で行うため Qt 初期化非依存）。
    // 空文字なら Qt 自動選択へフォールバックする
    const AppConfig earlyCfg = Config::load();
    StartupTrace::mark("config_loaded");
    if (!earlyCfg.hwDecoderPriority.isEmpty()) {
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
                earlyCfg.hwDecoderPriority.toUtf8());
    }

    // 単一インスタンス強制が ON のとき、自身が 2 個目以降なら引数を既存へ転送して即時終了する
    // QApplication 構築前に判定することで、不要な GUI 初期化を避ける。
    // 2 個目以降の判定は名前付き mutex で行う。パイプ接続成否による判定では、
    // 先発プロセスが listen を開始する前の窓で後発も primary になる競合があった
    const bool singleInstanceEnabled = Settings::instance().singleInstance();
    const QString preliminaryArg = firstArgumentUnicode();

    if (singleInstanceEnabled) {
        if (!SingleInstance::tryBecomePrimary()) {
            // 既存 primary あり。listen 開始待ちを含むリトライ付きで転送する。
            // 相対パス指定の CLI 起動でも primary 側（作業ディレクトリが異なる）で
            // ファイルを解決できるよう、転送前に絶対パスへ正規化する
            const QString forwardArg = preliminaryArg.trimmed().isEmpty()
                ? QString()
                : QFileInfo(preliminaryArg).absoluteFilePath();
            if (SingleInstance::forwardWithRetry(forwardArg)) return 0;
            // 転送失敗（primary 異常終了等）は自身が primary として続行する
        }
    }

    // プロセス優先度設定（レジストリ値が ON のときのみ ABOVE_NORMAL）
    if (Settings::instance().aboveNormalPriority()) {
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    }

    QApplication app(argc, argv);
    StartupTrace::mark("qapplication_ready");

    // Warning 以上の Qt メッセージを avply.log に書き出すハンドラを登録する
    qInstallMessageHandler(avplyMessageHandler);

    // ウィンドウアイコンを設定する
    // QRC 経由のアイコンは実行時のタイトルバー・タスクバー・Alt+Tab 表示用。
    // EXE シェル表示（エクスプローラ等）は src/app.rc で別途埋め込み済み。
    // setWindowIcon は QApplication 構築直後に呼び、起動初期のダイアログにも反映させる。
    QIcon appIcon(":/icons/app.ico");
    if (appIcon.isNull()) {
        qWarning("ウィンドウアイコンのロードに失敗しました：:/icons/app.ico");
    }
    app.setWindowIcon(appIcon);

    app.setApplicationName("avply");
    app.setApplicationVersion(AVPLY_VERSION);
    app.setOrganizationName("avply");

    // コマンドライン第 1 引数があれば初期ファイルとして MainWindow に渡す
    // （Windows の D&D 起動・「送る」・「プログラムを指定して開く」用）。
    // QApplication 構築前に取得した Unicode 引数をそのまま使う
    // （app.arguments() は Qt 6 でも内部的に WinMain 由来の UTF-16 を再構成するが、
    // 起動経路を一本化するため preliminaryArg を流用する）
    const QString initialPath = preliminaryArg;

    // 白フラッシュ抑制（透明化と復帰予約）は MainWindow コンストラクタが行う
    MainWindow win(initialPath);
    StartupTrace::mark("mainwindow_ready");
    win.show();
    StartupTrace::mark("shown");

    // 単一インスタンスが ON のときは primary として IPC サーバを起動し、
    // 後続の起動から送られてくるファイルパスを受信する
    if (singleInstanceEnabled) {
        SingleInstance::startServer(&win, &win);
    }

    return app.exec();
}
