#include "Encoder.h"
#include <QDebug>
#include <QStringList>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>

namespace {

// 出力映像の幅上限（px）
// QWXGA = 2048 を上限とし、これを超える入力はアスペクト比維持でダウンスケールする
constexpr int kMaxOutputWidth = 2048;

// 同名上書き時の既存ファイル退避リトライ回数と間隔（ms）
// releaseFileRequested 後も QMediaPlayer バックエンドの OS ハンドル解放は非同期の
// 可能性があるため、短い間隔でリネームを再試行する。最悪 1 秒 GUI thread をブロックするが、
// 通常はハンドル解放済みで初回に成功する
constexpr int kReplaceRetryCount = 10;
constexpr int kReplaceRetryIntervalMs = 100;

} // namespace

Encoder::Encoder(const QString& ffmpegPath, QObject* parent)
    : QObject(parent)
    , m_ffmpegPath(ffmpegPath)
{}

Encoder::~Encoder()
{
    // MainWindow デストラクタの waitForFinished 後でも tempfile が残るケースに備える。
    // QProcess は親子破棄で kill+wait されるが、QFile::remove は明示的に呼ぶ必要がある
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    if (!m_tempPath.isEmpty() && QFile::exists(m_tempPath)) {
        // dtor 経路での temp 削除失敗も警告ログを残す
        // 中止 / MainWindow 終了経由で AV ソフトに temp ハンドルを掴まれていた場合の
        // %TEMP% リークを可視化する（onProcessFinished 側と対称）
        if (!QFile::remove(m_tempPath)) {
            qWarning("Encoder: 一時ファイル %s の削除に失敗しました（%%TEMP%% に残存します）",
                     qUtf8Printable(m_tempPath));
        }
    }
}

bool Encoder::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void Encoder::encode(const EncodeParams& params)
{
    // 既存ジョブが走行中なら新規要求を拒否しつつ呼び出し側へ通知する
    // silent return だと UI が「変換中 0%」のまま無限待機に陥るため、
    // 必ず finished(false, ...) を emit して、UI の進捗表示をリセットする経路を確保する
    if (isRunning()) {
        qWarning() << "Encoder: 既存の変換処理が完了していないため新規要求を拒否しました";
        // 受理しなかった要求の outputPath を渡すと、呼び出し側で「失敗したが出力ファイルあり」と
        // 誤解されかねないため空文字で発火する（受理した要求の進捗・後始末とは独立した経路）
        emit finished(false, QString(),
                      QStringLiteral("前の変換処理が完了していないため、新規要求を受け付けられません"));
        return;
    }

    m_cancelled = false;
    m_params = params;
    m_outputTail.clear();
    m_totalDuration = params.outSec - params.inSec;
    if (m_totalDuration <= 0.0) {
        emit finished(false, params.outputPath, "IN/OUT の範囲が無効です");
        return;
    }

    // %TEMP% に一時出力ファイルパスを生成する
    // 完了後に本来の出力パスへ移動する設計
    // 一時ファイルの拡張子は出力パスと一致させる（ffmpeg のコンテナ判定が拡張子依存のため）
    // 複数 avply プロセス同時実行時の衝突を避けるため UUID を使う
    const QString outExt = QFileInfo(params.outputPath).suffix().toLower();
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_tempPath = QString("%1/avply_%2.%3")
        .arg(tempDir,
             QUuid::createUuid().toString(QUuid::WithoutBraces),
             outExt);

    QStringList args;
    args << "-y"
         << "-hide_banner";

    // 映像入力かつ再エンコード時のみ NVENC のための CUDA HW デコードを使う
    if (params.mode == EncodeMode::Reencode && params.hasVideo) {
        args << "-hwaccel" << "cuda";
    }

    args << "-ss" << QString::number(params.inSec, 'f', 3)
         << "-i" << params.inputPath
         << "-t" << QString::number(m_totalDuration, 'f', 3);

    if (params.mode == EncodeMode::Reencode) {
        if (params.hasVideo) {
            // 幅上限超の場合はアスペクト比を維持してスケールダウン
            if (params.inputWidth > kMaxOutputWidth) {
                args << "-vf" << QString("scale=%1:-2").arg(kMaxOutputWidth);
            }

            args << "-c:v" << "av1_nvenc"
                 << "-rc" << "vbr"
                 << "-cq" << "28"
                 << "-preset" << "p6"
                 // スライド切替後の品質回復を最大 4 秒以内（30fps 想定）に抑える
                 << "-g" << "120"
                 // フラットな背景・テキストのビット配分を改善する
                 << "-spatial_aq" << "1";
        }
        else {
            // 音声のみ入力：映像ストリームを除外する（一部コンテナの非映像ストリーム混入も防ぐ）
            args << "-vn";
        }

        args << "-c:a" << "libopus"
             << "-b:a" << "96k";
    }
    else {
        // ストリームコピー：再エンコードせずキーフレーム単位で切り出す
        args << "-c" << "copy";
    }

    // +faststart は ISOBMFF 系コンテナ専用。それ以外（opus/ogg/mp3/wav 等）に付けると ffmpeg が警告を出す
    if (outExt == "mp4" || outExt == "m4a" || outExt == "mov") {
        args << "-movflags" << "+faststart";
    }

    args << m_tempPath;

    m_process = new QProcess(this);
    // ffmpeg は進捗行（time=...）を stderr に出力するため、readyReadStandardOutput でまとめて受け取れるよう統合する
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &Encoder::onReadyReadOutput);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Encoder::onProcessFinished);

    // FailedToStart のとき finished は発火しないため errorOccurred で捕捉する。
    // ここで通知しないと UI が「変換中 0%」のまま無限待機となる
    connect(m_process, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        if (!m_process) return;
        disconnect(m_process, nullptr, this, nullptr);
        m_process->deleteLater();
        m_process = nullptr;
        emit finished(false, m_params.outputPath, "ffmpeg の起動に失敗しました");
    });

    m_process->start(m_ffmpegPath, args);
}

void Encoder::cancel()
{
    if (m_process) {
        m_cancelled = true;
        m_process->kill();
    }
}

bool Encoder::waitForFinished(int timeoutMs)
{
    if (!m_process) return true;
    return m_process->waitForFinished(timeoutMs);
}

void Encoder::onReadyReadOutput()
{
    if (!m_process || m_totalDuration <= 0.0) return;

    // ffmpeg は Windows でロケール（典型的に CP932）の文字を含み得るため fromLocal8Bit を使う。
    // UTF-8 暗黙変換だとエラーメッセージが化けてユーザに有用な情報が伝わらない
    const QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput());

    // 失敗時のエラー説明用に末尾を一定量だけ保持する。
    // ffmpeg のエラー要旨は最終フレーム数行に集約されるため、末尾の固定窓だけで実用十分
    constexpr int kOutputTailLimit = 2048;
    m_outputTail.append(text);
    if (m_outputTail.size() > kOutputTailLimit) {
        m_outputTail = m_outputTail.right(kOutputTailLimit);
    }

    // ffmpeg 進捗行の time=HH:MM:SS.nn から経過時間を抽出
    // 先頭の `-` は \d+ に一致しないため、ffmpeg がシーク直後に出力する負値（time=-00:00:01.500 等）は
    // そのまま skip される。NaN / 異常値の侵入経路を構造的に断つ
    static const QRegularExpression re(R"(time=(\d+):(\d{2}):(\d{2}\.\d+))");
    auto it = re.globalMatch(text);
    double latestSec = -1.0;
    while (it.hasNext()) {
        const auto m = it.next();
        const double sec = m.captured(1).toDouble() * 3600.0
                         + m.captured(2).toDouble() * 60.0
                         + m.captured(3).toDouble();
        latestSec = sec;
    }

    // 進捗の上限ガード
    // 通常 latestSec は 0〜totalDuration 内に収まるが、ffmpeg のバグ・破損ストリーム解析で
    // 異常な大きい値が混入し progressChanged が暴れるのを防ぐ。
    // 短い区間（例：100ms 切り出し）では m_totalDuration*2.0 が極小になり、ffmpeg 初期の
    // 「00:00:00.50」等の進捗が上限を超えて以降の emit が全部抑制されるため、最低 10 秒の下限フロアを設ける。
    // 短区間で latestSec が m_totalDuration を超えうるが、下流の qBound(0, pct, 99) が
    // 100% 化を吸収するため UI への悪影響はない（ffmpeg 初期異常値の遮断と短区間進捗の両立を優先）。
    // 0 未満は捕捉漏れ（初期値の -1.0）として下流に流さない
    const double upperBound = std::max(m_totalDuration * 2.0, 10.0);
    if (latestSec >= 0.0 && latestSec <= upperBound) {
        // 100% は onProcessFinished で出力ファイル確定後に emit するため、進捗段階は 99% で頭打ち
        const int pct = static_cast<int>(latestSec / m_totalDuration * 100.0);
        emit progressChanged(qBound(0, pct, 99));
    }
}

void Encoder::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    m_process->deleteLater();
    m_process = nullptr;

    // 失敗・中止時は一時ファイルを削除する
    auto cleanupTemp = [this]() {
        if (!m_tempPath.isEmpty() && QFile::exists(m_tempPath)) {
            QFile::remove(m_tempPath);
        }
    };

    // ユーザ要求による中止：err を空文字で発火し呼び出し側でダイアログを抑制する
    if (m_cancelled) {
        cleanupTemp();
        emit finished(false, m_params.outputPath, {});
        return;
    }
    if (status == QProcess::CrashExit) {
        cleanupTemp();
        emit finished(false, m_params.outputPath, "変換処理が中断されました");
        return;
    }
    if (exitCode != 0) {
        cleanupTemp();
        // ffmpeg 出力末尾を付加してデバッグ可能性を確保する
        QString detail = QString("変換に失敗しました（終了コード：%1）").arg(exitCode);
        const QString tail = m_outputTail.trimmed();
        if (!tail.isEmpty()) {
            detail.append(QStringLiteral("\n----- ffmpeg 出力末尾 -----\n"));
            detail.append(tail);
        }
        emit finished(false, m_params.outputPath, detail);
        return;
    }

    // 空出力の検証
    // ffmpeg は選択区間に実データが 1 フレームも乗らない場合、「Output file is empty」と
    // 警告しつつヘッダのみのコンテナを書いて exit 0 で終わることがある。メタデータ duration が
    // 実ストリーム長より長い破損録画のトリム等で実機再現済みだ。そのまま置換すると
    // _mod 上書き経路で元データが再生不能な空コンテナへ不可逆に置き換わるため、置換前に弾く
    if (QFileInfo(m_tempPath).size() == 0
        || m_outputTail.contains(QLatin1String("Output file is empty"))) {
        cleanupTemp();
        emit finished(false, m_params.outputPath,
                      "出力にデータが含まれていません（指定区間に有効なストリームがない可能性があります）");
        return;
    }

    // 一時ファイルを本来の出力パスへ移動する
    // 出力先に既存ファイルがある場合の扱いは allowOverwrite で分岐する。
    //   - 不許可：OutputNamer のユニーク名生成から本処理までの TOCTOU で他プロセス・
    //     他ユーザ操作の成果物を上書きしてしまうため、明示的にエラー終了させる
    //   - 許可（入力名が _mod 形式の同名上書き）：既存ファイルを退避してから置換する
    QString backupPath;
    if (QFile::exists(m_params.outputPath)) {
        if (!m_params.allowOverwrite) {
            cleanupTemp();
            emit finished(false, m_params.outputPath,
                          "出力先に同名ファイルが存在します。別の avply プロセスや外部操作の可能性があります。競合ファイルを移動または削除してから再実行してください。");
            return;
        }

        // 既存ファイルの退避（バックアップへリネーム）
        // 出力先 = 再生中の入力ファイルのケースがあるため、先に releaseFileRequested で
        // 呼び出し側のファイルハンドル解放を依頼してからリネームをリトライする。
        // 削除でなく退避にするのは、後段の移動が失敗したとき元ファイルを復元するため
        emit releaseFileRequested(m_params.outputPath);
        backupPath = m_params.outputPath + ".avply.bak";
        QFile::remove(backupPath); // 前回異常終了の残骸を除去する
        bool backedUp = false;
        for (int i = 0; i < kReplaceRetryCount; ++i) {
            if (QFile::rename(m_params.outputPath, backupPath)) {
                backedUp = true;
                break;
            }
            QThread::msleep(kReplaceRetryIntervalMs);
        }
        if (!backedUp) {
            cleanupTemp();
            emit finished(false, m_params.outputPath,
                          "既存ファイルが他のプロセスに使用されているため上書きできませんでした");
            return;
        }
    }
    // %TEMP% が出力先と別ボリュームだと QFile::rename が失敗するため copy + remove にフォールバックする
    if (!QFile::rename(m_tempPath, m_params.outputPath)) {
        if (!QFile::copy(m_tempPath, m_params.outputPath)) {
            // 部分書き込みで生じた壊れた出力ファイルを除去してから失敗を通知する
            QFile::remove(m_params.outputPath);
            // 同名上書き時は退避しておいた元ファイルを復元する
            if (!backupPath.isEmpty() && !QFile::rename(backupPath, m_params.outputPath)) {
                qWarning("Encoder: 退避ファイル %s の復元に失敗しました",
                         qUtf8Printable(backupPath));
            }
            cleanupTemp();
            emit finished(false, m_params.outputPath,
                          "一時ファイルから出力先への移動に失敗しました");
            return;
        }
        // copy 後の temp 削除失敗時は警告ログを残す
        // AV ソフトが temp ハンドルを掴んだままだと remove が失敗し、%TEMP% に大容量ファイルがリークする
        if (!QFile::remove(m_tempPath)) {
            qWarning("Encoder: 一時ファイル %s の削除に失敗しました（%%TEMP%% に残存します）",
                     qUtf8Printable(m_tempPath));
        }
    }

    // 置換完了後に退避ファイルを削除する
    if (!backupPath.isEmpty() && !QFile::remove(backupPath)) {
        qWarning("Encoder: 退避ファイル %s の削除に失敗しました（出力フォルダに残存します）",
                 qUtf8Printable(backupPath));
    }

    emit progressChanged(100);
    emit finished(true, m_params.outputPath, {});
}
