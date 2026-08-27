#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QIcon>
#include <QTimer>
#include "FfmpegRunner.h"
#include "VideoView.h"
#include "RangeSlider.h"
#include "Encoder.h"
#include "SeekPreview.h"
#include "ThumbnailExtractor.h"
#include "SilenceTone.h"

class QDragEnterEvent;
class QDropEvent;
class QProcess;
class QAction;
class QContextMenuEvent;

// アプリケーションのメインウィンドウ
// ファイル選択・シーク・開始/終了設定・変換実行を担う
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // initialPath にパスを渡すと起動完了後にそのファイルを読み込む
    // （Windows の D&D 起動・「送る」・「プログラムを指定して開く」想定）
    explicit MainWindow(const QString& initialPath = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

    // IPC で他インスタンスから受信したファイルパスを取り込む
    // 受信時はウィンドウを前面化して可視性を確保する。空文字なら前面化のみ
    void loadFileFromIpc(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    // 右クリックでコンテキストメニューを表示する
    void contextMenuEvent(QContextMenuEvent* event) override;

    // Windows ネイティブメッセージを処理する
    // WM_SIZING でウィンドウドラッグ中の RECT を直接書き換え、リアルタイムにアスペクト比を維持する
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onOpenFile();
    void onSeekSliderChanged(int value);
    void onPlayerPositionChanged(qint64 ms);
    void onSetIn();
    void onSetOut();
    void onStop();
    void onConvertOrCancel();
    void onTrimOrCancel();
    void onCopyFilePath();
    void onEncoderProgress(int pct);
    void onEncoderFinished(bool ok, const QString& outputPath, const QString& err);

    // 同名上書きの退避リネーム直前に path を開いているファイルハンドルを解放する
    // Encoder::releaseFileRequested から direct 接続で同期実行される
    void onEncoderReleaseFile(const QString& path);

    // シークバーのホバー位置を受信して、サムネイル + 時刻のプレビューを表示する
    void onSeekHoverMoved(int x, int sliderValue);

    // マウスがシークバー外に出たときにプレビューを非表示にする
    void onSeekHoverLeft();

private:
    // メディアファイルを実際に読み込む（全ロード経路が合流する唯一の入口）
    // centerOnMonitor=true のときのみモニタ作業領域の中央へウィンドウを移動する
    void loadFile(const QString& path, bool centerOnMonitor = false);

    // ffprobe 完了後の UI 反映処理
    // loadFile の async コールバックから呼び出され、メディア情報のラベル更新・
    // ウィンドウサイズ調整・波形生成キックを担う
    void onProbeFinished(const QString& path, const VideoInfo& info, bool centerOnMonitor);

    // フォルダ内の前後メディアファイルへ切替
    // step は +1（次）/ -1（前）。現在ファイルと同じフォルダを対応拡張子で列挙する。
    // エクスプローラと同じ自然順（数値対応・大文字小文字無視）で並べた隣接ファイルを開く。
    // 端（先頭・末尾）ではラップせず何もしない。未ロード時・現在ファイルが列挙に無い場合も何もしない
    void loadNeighborFile(int step);

    // 拡張子がメディア（動画・音声）として受け付け可能か判定する
    static bool isAcceptedMedia(const QString& path);

    // 拡張子から音声ファイル（mp3/wav/flac/ogg/opus）かを判定する
    // ロード前の初期ウィンドウ構成と、ロード後の isAudioOnly() 判定の両方で使う。
    // 対応音声拡張子は中身に関わらず音声扱いに倒すため、isAudioOnly() の判定第一項として参照する
    static bool isAudioByExtension(const QString& path);

    // 音声のみ扱い（プレビュー領域を省略し変換は libopus のみ）の判定
    // probe 未完了時は false（動画扱い）を返す。VideoInfo を probe 完了前に参照すると
    // 旧情報を流用してフリッカ要因になるため m_info.valid をガードに使う。
    // 対応音声拡張子（mp3/wav/flac/ogg/opus）は ID3v2 APIC 等の埋め込み画像で
    // attached_pic 付き video stream が返るため、中身ではなく拡張子で音声側へ倒す。
    // 動画拡張子（mp4/mkv/mov/avi/webm）は中身が音声のみのコンテナ（例：mkv 内音声のみ）
    // にも追従できるよう ffprobe 結果を見る
    bool isAudioOnly() const {
        return m_info.valid
            && (isAudioByExtension(m_filePath)
                || m_info.codec.isEmpty()
                || m_info.width <= 0);
    }

    // UI 有効/無効を切り替える（変換中は無効化）
    void setUiEnabled(bool enabled);

    // トリムが意味を持つか（実効範囲が動画全長と異なるか）を判定する
    bool isTrimMeaningful() const;

    // ffmpeg 実行ファイルが利用可能か
    // パス未設定と実体消失の双方を都度判定する（起動後の削除・移動にも追従するため状態を持たない）
    bool isFfmpegAvailable() const;

    // ロード失敗をユーザへ通知する
    // 再生をクリアした上でダイアログを出す。表示中のネストイベントループで D&D 等から
    // loadFile が再入するのを防ぐため m_loadInhibited を立てて囲う
    // 本関数自体の再入時は旧値を退避・復元し、内側の復帰で外側の抑止が解けないようにする
    void showLoadError(const QString& message);

    // 実行中の操作種別。None ならアイドル
    enum class Operation { None, Convert, Trim };

    // 変換・トリム共通の起動/中止ハンドラ
    void startOrCancel(EncodeMode mode);

    // 実行状態に応じて UI をまとめて切り替える
    void setRunning(Operation op);

    // 区間マーカーをスライダーに反映する
    void updateRangeMarkers();

    // スライダー値を秒数に変換する
    double sliderToSec(int value) const;

    // 秒数を HH:MM:SS 形式の文字列に変換する
    static QString formatSec(double sec);

    // ffmpeg パスの妥当性を検査し、不正なら警告ダイアログを 1 回出す
    void validateFfmpegPath();

    // 音声波形 PNG の生成を非同期で起動する
    // キャッシュヒット時は ffmpeg を起動せず即時シークバーへ反映する
    void startWaveformGeneration(const QString& inputPath);

    // 入力ファイルパス + mtime をキーにした波形 PNG キャッシュパスを返す
    QString waveformCachePath(const QString& inputPath) const;

    // 実行中の波形生成プロセスを停止し m_waveformProc を解放する
    // disconnect → kill → 短時間 waitForFinished → setParent(nullptr) + deleteLater の順で
    // 解放することで、~QProcess() の waitForFinished(30000) ブロックを回避しつつ
    // ハンドルが解放されるのを待つ。kill した中途生成 PNG は QFile::remove で削除し
    // 次回起動時に破損キャッシュをヒットさせない
    void stopWaveformProcess();

    // カーソルキーによる相対シーク（delta > 0 で早送り、< 0 で巻き戻し）
    void seekRelative(int deltaMs);

    // 再生速度を相対変更してステータス表示を更新する（delta は 0.05 単位想定）
    void changePlaybackRate(qreal delta);

    // 再生速度ラベルの表示を現在値で更新する
    void updateSpeedDisplay();

    // 音量を相対変更してラベル表示と VideoView へ反映する（delta は 0.05 単位想定）
    void changeVolume(qreal delta);

    // ホイール入力を修飾子に応じてシーク／音量／再生速度に振り分ける
    // VideoView と RangeSlider の両 wheelScrolled 経路で共通使用する。
    // Ctrl 優先 → Shift → 修飾子なしシークの順。変換・トリム実行中はすべて抑止
    void handleWheelInput(bool forward, bool shift, bool ctrl);

    // 音量ラベルの表示を現在値で更新する
    void updateVolumeDisplay();

    // 「開く...」ダイアログの初期ディレクトリを返す
    // 動画読込済なら同フォルダ、未読込なら %USERPROFILE%
    QString openDialogStartDir() const;

    // アプリケーション全体のキー入力を捕捉してシーク・再生制御に変換する
    bool eventFilter(QObject* watched, QEvent* event) override;

    // メニューから操作する設定の即時反映用ハンドラ
    void onToggleTopmost(bool checked);
    void onToggleSingleInstance(bool checked);
    void onTogglePriority(bool checked);

    // 音声強調の ON/OFF をトグルする
    // C キー押下から呼ばれる。AudioWorker への反映とラベル更新をまとめて行う。
    // 永続化はしない（起動時は常に OFF。インスタンス生存中はファイル切替をまたいで保持）
    void toggleSpeechEnhance();

    // 音声強調ラベルの表示を現在の状態に応じて更新する
    // 常時表示で「Clarity:ON/OFF」を表示する
    void updateSpeechEnhanceDisplay();

    // g キー押下時のトグル動作
    // 1 回目で再生速度/音量/音声強調を全て「中立値」へ揃え、
    // 2 回目で速度・音量を起動時に読み込んだ TOML 値へ復元する（音声強調は両回とも OFF）
    void toggleGReset();

    // 再生速度・音量を一括適用し、音声強調は常に OFF へ倒す
    // toggleGReset 専用の内部ヘルパで、m_gResetActive フラグは操作しない（呼び出し側で管理）。
    // 音声強調は永続化しない仕様のため、中立値復元・起動時値復元のいずれでも OFF が正となる
    void applyPlaybackState(qreal rate, qreal vol);

    // 再生状態に応じてウィンドウの最前面表示を切り替える
    // Settings::topmostWhilePlaying が true かつ playing なら topmost、それ以外は解除
    void applyTopmostState();

    // メニューアクションの enabled 状態を現在の文脈に合わせて更新する
    void updateMenuActionEnabled();

    // 指定した画面座標にコンテキストメニューを表示する
    // contextMenuEvent と VideoView 経由のシグナルから共通で呼び出す
    void showContextMenuAt(const QPoint& globalPos);

    // 現在のシークスライダー位置から SeekPreview の表示位置を更新する
    void updateSeekPreviewPosition(int x);

    // 現在の m_hoverPendingSec を対象にサムネイル抽出を要求する
    // 完了 callback 内で最新ホバー位置が遷移していたら自分自身を再呼び出しして連鎖追従する
    void requestHoverThumbnail();

    // 動画情報
    QString   m_filePath;
    VideoInfo m_info;
    double    m_inSec  = 0.0;
    double    m_outSec = 0.0;
    bool      m_inSet  = false;
    bool      m_outSet = false;

    // 設定
    QString m_ffmpegPath;
    int     m_seekLeftMs       = 5000;
    int     m_seekRightMs      = 5000;
    int     m_seekWheelForwardMs = 5000;
    int     m_seekWheelBackMs    = 5000;

    // 動画読込時の初期ウィンドウサイズ上限のモニタ比率（avply.toml の [window].initial_screen_ratio）
    double m_initialScreenRatio = 0.7;

    // 現在の再生速度（1.0 = 等速）
    qreal m_playbackRate = 1.0;

    // 現在の再生音量（0.0〜1.0）
    qreal m_volume = 1.0;

    // 現在の音声強調 ON/OFF
    // 永続化しないためインスタンス生存中のみ保持する（起動時は常に OFF）
    bool m_speechEnhanceEnabled = false;

    // g キーで参照する起動時デフォルト値のスナップショット
    // TOML から初回読込した値をコンストラクタで保存する
    qreal m_initialPlaybackRate   = 1.0;
    qreal m_initialVolume         = 1.0;

    // g キーによる「全リセット状態」フラグ
    // true の間に手動で速度/音量/音声強調のいずれかが変わると自動で false に戻り、
    // 次の g キー押下は再び「全リセット」として動作する
    bool  m_gResetActive = false;

    // ウィンドウのアスペクト比連動用状態
    // m_videoAspect は WM_SIZING 中に参照する動画の基準比率（動画未読込時は 16:9）
    // m_lowerUiH は下部 UI（seekRow + statusBar + 余白）の自然高合計
    double m_videoAspect            = 16.0 / 9.0;
    int    m_lowerUiH               = 0;

    // ウィジェット
    VideoView*    m_videoView;
    QPushButton*  m_playPauseBtn;
    QPushButton*  m_stopBtn;

    // 再生状態切替で頻繁に差し替えるアイコンはコンストラクタで一度だけ生成して保持する
    QIcon m_iconPlay;
    QIcon m_iconPause;
    QLabel*       m_posLabel;
    QLabel*       m_speedLabel;
    QLabel*       m_volumeLabel;
    QLabel*       m_speechEnhanceLabel;
    RangeSlider*  m_seekSlider;
    QPushButton*  m_setInBtn;
    QPushButton*  m_setOutBtn;
    QPushButton*  m_trimBtn;
    QLabel*       m_videoInfoLabel;
    QLabel*       m_outputLabel;

    // コンテキストメニュー（右クリック）の各項目
    // 「変換」は実行中に「中止」表記へ切り替え、「トリム」はメインの m_trimBtn と同期する
    QAction*      m_actOpen          = nullptr;
    QAction*      m_actCopyPath      = nullptr;
    QAction*      m_actConvert       = nullptr;
    QAction*      m_actTrim          = nullptr;
    QAction*      m_actTopmost       = nullptr;
    QAction*      m_actSingleInst    = nullptr;
    QAction*      m_actPriority      = nullptr;

    // 現在の再生状態（applyTopmostState で参照）
    bool m_isPlaying = false;

    // シークバードラッグ開始時点の再生状態
    // ドラッグ終了時の再生再開可否判定に使う（開始前に停止していたら再開しない）
    bool m_wasPlayingBeforeDrag = false;

    Encoder* m_encoder = nullptr;

    // 同名上書きのためにプレイヤー等のファイルハンドルを解放済みかのフラグ
    // onEncoderReleaseFile で立て、onEncoderFinished で落とす。
    // 退避リネーム失敗等で置換に至らなかったとき、解放したファイルを開き直す判定に使う
    bool m_fileReleasedForOverwrite = false;

    // 実行中の波形生成プロセス。新規ファイル読込時に kill して入れ替える
    QProcess* m_waveformProc = nullptr;

    // 実行中の ffprobe プロセス。新規ファイル読込時に kill して旧結果を破棄する
    QProcess* m_probeProc = nullptr;

    // loadFile の世代番号（呼び出しごとに加算）
    // 旧 probe 破棄の waitForFinished(1000) はイベントループを再入させるため、
    // その間に D&D / IPC 経由の loadFile が割り込み得る。待機明けに世代が進んでいたら
    // 自分は追い越された古いロードなので、以降の処理を放棄して新しいロードへ全面的に譲る
    quint64 m_loadGeneration = 0;

    // 現在生成中プロセスの出力先 PNG パス。kill 時に部分書き込みファイルを削除するため保持する
    QString m_waveformProcOutPath;

    // シークバーホバープレビュー
    SeekPreview*        m_seekPreview    = nullptr;
    ThumbnailExtractor* m_thumbExtractor = nullptr;

    // 最新の量子化済みホバー秒数（-1 = 未設定。シークバー外・ファイル未読込の初期状態）
    // -1 との比較により初回ホバーは必ず ffmpeg を起動する。
    // クロージャ内で「現在のホバー対象が同じ秒か」をチェックして古い結果の表示を防ぐ
    int    m_hoverPendingSec = -1;
    int    m_hoverLastX      = 0;

    // 実行中の操作種別。None ならアイドル
    Operation m_runningOp = Operation::None;

    // probe 失敗ダイアログ表示中の loadFile 再入抑止フラグ
    // QMessageBox::critical のネストイベントループ中に D&D 等で loadFile が呼ばれても無視する
    bool m_loadInhibited = false;

    // シーク要求のスロットル（連続 valueChanged を間引く）
    QTimer  m_seekTimer;
    qint64  m_pendingSeekMs = -1;

    // BT ヘッドセットのアイドル復帰時プチノイズ抑制用の常時不可聴トーン出力
    // QMediaPlayer とは独立した QAudioSink で 1kHz / 約 -80dBFS を流し続け、
    // 出力デバイスが省電力状態に落ちて再エンゲージするときの音切れを防ぐ
    SilenceTone* m_silenceTone = nullptr;
};
