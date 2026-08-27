#include "MainWindow.h"
#include "Config.h"
#include "OutputNamer.h"
#include "Settings.h"
#include <QApplication>
#include <QClipboard>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QSignalBlocker>
#include <QTimer>
#include <QStatusBar>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QStyle>
#include <QIcon>
#include <QPixmap>
#include <QFont>
#include <QProcess>
#include <QStandardPaths>
#include <QDateTime>
#include <QCryptographicHash>
#include <QAction>
#include <QMenu>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QWindow>
#include <QThreadPool>
#include <QCollator>
#include <algorithm>
#include <cmath>

// WM_SIZING / WMSZ_* 定数のため Windows API ヘッダを取り込む
// NOMINMAX を先に定義しないと windows.h の min / max マクロが std::min / std::max と衝突する
#define NOMINMAX
#include <windows.h>

// シークスライダーの分解能
// 0〜10000 の固定分解能だ（duration の 0.01% 刻み）。
// 短尺ではフレーム未満の精度、長尺（30fps で約 5.5 分超）では 1 目盛が複数フレームに相当する。
// トリム開始位置はキーフレーム丸めが支配的なため、この精度で実用上問題ない
static constexpr int kSliderMax = 10000;

// 再生速度の増減刻み（`.` / `,` キー、Ctrl + ホイール）
static constexpr qreal kPlaybackRateStep = 0.05;
// 音量の増減刻み（↑ / ↓ キー、Shift + ホイール）
static constexpr qreal kVolumeStep = 0.05;
// 再生速度の下限（これ以下は音声の時間伸張が破綻するため許可しない）
static constexpr qreal kPlaybackRateMin = 0.05;
// 再生速度の上限（これ以上は音声が判別できず実用に耐えないため許可しない）
// toml の [playback].speed も Config.cpp の clampConfig が同じ 4.0 で丸める
static constexpr qreal kPlaybackRateMax = 4.0;

// キー入力の修飾子判定に用いるマスク集合
// Shift / Ctrl / Alt / Meta だけを見る。KeypadModifier はテンキー押下で付与される
// 意味的に中立な修飾子のため除外し、テンキーの ↑ / ↓ もメインキーと同じ動作で扱う
static constexpr Qt::KeyboardModifiers kModifierMask =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

// 起動時の初期ウィンドウサイズ（最小サイズも兼ねる）
// 動画ロード後に動画サイズへリサイズするまでの暫定表示用
static constexpr int kInitialWindowW = 500;
static constexpr int kInitialWindowH = 375;

// 音声波形 PNG の生成サイズ
// シークバー幅は最大でも数百 px だが、QPainter 側のスケール描画品質を保つため幅 2048px の余裕を持たせる。
// 高さ 48px はトラック高 28px への縮小描画でも詳細が潰れない解像度
static constexpr int kWaveformW = 2048;
static constexpr int kWaveformH = 48;

namespace {

// ステータスバーラベル先頭の絵文字プレフィックス
// 🎬 = 再生速度、🔊 = 音量。MSVC の文字リテラル経路を避けるため UTF-8 バイト列で直書きする
const QString kSpeedPrefix     = QString::fromUtf8("  \xf0\x9f\x8e\xac ");
const QString kVolumePrefix    = QString::fromUtf8("  \xf0\x9f\x94\x8a ");
// ステータスバー常時表示ラベルのプレフィックス
// 後ろに "ON" / "OFF" を連結して表示する
const QString kSpeechEnhancePrefix = "  Clarity:";

// メニューの角丸抑制スタイル
// Windows 11 ネイティブ装飾の強い角丸を抑え、ほぼ角張った見た目にする。
// コンテキストメニューとその設定サブメニューの両方へ適用する
const QString kMenuStyle = QStringLiteral("QMenu { border-radius: 2px; }");

// 受け入れ可能なメディア拡張子（小文字、ドットなし）
// QFileDialog のフィルタ生成・D&D 判定・音声/動画振り分けで共通使用する
const QStringList kVideoExts = { "mp4", "mkv", "mov", "avi", "webm" };
const QStringList kAudioExts = { "mp3", "wav", "flac", "ogg", "opus" };

// QFileDialog のフィルタ文字列を生成する
// "*.mp4 *.mkv ..." 形式のスペース区切り
QString dialogFilterFromExts()
{
    QStringList globs;
    for (const QString& e : kVideoExts) globs << ("*." + e);
    for (const QString& e : kAudioExts) globs << ("*." + e);
    return globs.join(' ');
}

// 古い波形キャッシュ PNG を削除する
// mtime キーで命名済みのため、ユーザがソースを更新すると古い PNG が残り続ける。
// 60 日 atime/mtime しきい値で削除する（再生頻度の低いファイル分のみ自動清掃）。
// 起動時に QThreadPool 経由でワーカースレッドから呼び出す（UI スレッドの I/O ブロックを避けるため）
void purgeOldWaveformCache()
{
    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(tmpDir);
    if (!dir.exists()) return;
    const QDateTime threshold = QDateTime::currentDateTime().addDays(-60);
    const QFileInfoList entries = dir.entryInfoList(
        { "avply_wave_*.png" }, QDir::Files);
    for (const QFileInfo& fi : entries) {
        // 最後アクセス時刻が取れる環境では atime、取れなければ mtime を見る
        const QDateTime ref = fi.lastRead().isValid() && !fi.lastRead().isNull()
                              ? fi.lastRead() : fi.lastModified();
        if (ref < threshold) {
            QFile::remove(fi.absoluteFilePath());
        }
    }
}

} // namespace

MainWindow::MainWindow(const QString& initialPath, QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("avply");
    setAcceptDrops(true);

    // リサイズで露出した領域の未描画ギャップを軽減する
    // 新たに露出した領域がパレット既定色で即時クリアされ、「外枠だけ新サイズ・内側未描画」の
    // 描画追従ラグによる隙間が見えにくくなる
    setAutoFillBackground(true);

    // --- 動画プレビュー（クリックで再生/停止トグル、D&D でファイル読み込み） ---
    m_videoView = new VideoView;
    connect(m_videoView, &VideoView::positionChanged,
            this, &MainWindow::onPlayerPositionChanged);
    connect(m_videoView, &VideoView::fileDropped,
            this, [this](const QString& path) {
        if (m_runningOp == Operation::None && isAcceptedMedia(path)) loadFile(path);
    });
    connect(m_videoView, &VideoView::wheelScrolled,
            this, &MainWindow::handleWheelInput);
    // QMediaPlayer のロード失敗（InvalidMedia）をユーザへ通知する
    // ffprobe は成功するが Qt backend がデコードできないケース（コーデック非対応等）で発火する
    connect(m_videoView, &VideoView::loadFailed,
            this, [this](const QString& error) {
        showLoadError("メディアを再生できませんでした：\n" + error);
    });
    // QQuickView はネイティブ子ウィンドウのため右クリックが MainWindow へ伝搬しない。
    // VideoView から転送されたシグナルでメニューを表示する
    connect(m_videoView, &VideoView::contextMenuRequested,
            this, &MainWindow::showContextMenuAt);

    // --- 再生位置ラベル（ステータスバー右端に配置） ---
    // 先頭 2 半角スペースは項目間の区切りとして機能する
    m_posLabel = new QLabel("  --:--:-- / --:--:--");

    // --- 再生速度ラベル（ステータスバー右端、再生位置の右に配置） ---
    // 先頭の 🎬（カチンコ）はラベル種別の視覚的区別のため付与する
    m_speedLabel = new QLabel(kSpeedPrefix + "x1.00");

    // --- 音量ラベル（再生速度の右に配置） ---
    // 初期値は avply.toml の [audio].volume から取得し、カーソルキー（上下）と Shift+ホイールで動的に変更する
    // 先頭の 🔊 はラベル種別の視覚的区別のため付与する
    m_volumeLabel = new QLabel(kVolumePrefix + "100%");

    // --- 音声強調ラベル（常時表示。ON/OFF） ---
    m_speechEnhanceLabel = new QLabel(kSpeechEnhancePrefix + "OFF");

    // --- シークスライダー ---
    m_seekSlider = new RangeSlider(Qt::Horizontal);
    m_seekSlider->setRange(0, kSliderMax);
    m_seekSlider->setEnabled(false);
    // valueChanged を使うことでクリックジャンプの位置も拾える
    connect(m_seekSlider, &QSlider::valueChanged,
            this, &MainWindow::onSeekSliderChanged);
    connect(m_seekSlider, &RangeSlider::wheelScrolled,
            this, &MainWindow::handleWheelInput);

    // シークバードラッグ中は再生を一時停止し、離したら元の再生状態へ復帰する
    // ドラッグ開始前から一時停止していた場合は復帰時も一時停止を維持する
    connect(m_seekSlider, &RangeSlider::dragStarted, this, [this]() {
        m_wasPlayingBeforeDrag = m_videoView->isPlaying();
        if (m_wasPlayingBeforeDrag) m_videoView->pause();
    });
    connect(m_seekSlider, &RangeSlider::dragEnded, this, [this]() {
        if (m_wasPlayingBeforeDrag) m_videoView->play();
    });

    // --- シークバーホバープレビュー（MPC-HC 風） ---
    m_seekPreview    = new SeekPreview(this);
    m_thumbExtractor = new ThumbnailExtractor(this);

    connect(m_seekSlider, &RangeSlider::hoverMoved,
            this, &MainWindow::onSeekHoverMoved);
    connect(m_seekSlider, &RangeSlider::hoverLeft,
            this, &MainWindow::onSeekHoverLeft);

    // アイコン式ボタン共通スタイル
    // 外枠と内側パディングを消し、ホバー時のみ薄いグレーで反応を示す
    // padding: 0 を入れないとテキストボタン（【】）でホバー範囲が縦に膨らみ、
    // アイコンボタン（再生・停止）と見た目のサイズが揃わない
    const QString iconBtnStyle =
        "QPushButton { border: none; padding: 0; }"
        "QPushButton:hover { background-color: rgba(255, 255, 255, 30); }";
    // アイコン式ボタンの基準サイズ（再生・停止・トリムの正方形 3 ボタンへ適用し、【】は高さのみ揃える）
    // 【】テキストの見た目に揃うようコンパクトにし、アイコンも一回り小さくする
    const QSize iconBtnSize(28, 28);
    const QSize iconImgSize(18, 18);

    // --- 再生/一時停止ボタン（シークバー左、再生状態の視認も兼ねる） ---
    // PNG アイコンを使用する
    m_iconPlay  = QIcon(":/icons/play.png");
    m_iconPause = QIcon(":/icons/pause.png");
    m_playPauseBtn = new QPushButton;
    m_playPauseBtn->setIcon(m_iconPlay);
    m_playPauseBtn->setIconSize(iconImgSize);
    connect(m_playPauseBtn, &QPushButton::clicked, this, [this]() {
        if (m_info.valid) m_videoView->togglePlay();
    });
    connect(m_videoView, &VideoView::playbackStateChanged,
            this, [this](bool playing) {
        m_playPauseBtn->setIcon(playing ? m_iconPause : m_iconPlay);
        m_isPlaying = playing;
        applyTopmostState();
    });

    // --- 停止ボタン（シーク位置を 0 に戻し、開始/終了マーカーをクリアする） ---
    m_stopBtn = new QPushButton;
    m_stopBtn->setIcon(QIcon(":/icons/stop.png"));
    m_stopBtn->setIconSize(iconImgSize);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    // --- 開始/終了 設定ボタン（再生/停止と同じアイコン式スタイルに揃える） ---
    // [ / ] キーでも操作できるようキーボードショートカットを割り当てる
    m_setInBtn  = new QPushButton("【");
    m_setOutBtn = new QPushButton("】");
    m_setInBtn ->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    m_setOutBtn->setShortcut(QKeySequence(Qt::Key_BracketRight));
    connect(m_setInBtn,  &QPushButton::clicked, this, &MainWindow::onSetIn);
    connect(m_setOutBtn, &QPushButton::clicked, this, &MainWindow::onSetOut);

    // --- トリムボタン（シークバー行の右側、他アイコンボタンと同サイズに揃える） ---
    // 「変換」はコンテキストメニュー側に移行済み
    // Segoe UI Symbol を明示することで Segoe UI Emoji へのフォールバック（カラー絵文字化）を回避し
    // 他の PNG アイコンと同トーンの線画モノクロで表示する
    m_trimBtn = new QPushButton(QString::fromUtf8(u8"✂"));
    QFont trimFont("Segoe UI Symbol");
    trimFont.setPixelSize(16);
    m_trimBtn->setFont(trimFont);
    m_trimBtn->setToolTip("トリム");
    connect(m_trimBtn, &QPushButton::clicked, this, &MainWindow::onTrimOrCancel);

    // 3 つの正方形アイコンボタンに共通スタイルとサイズを一括適用する
    // 28x28 でシークバー上段トラック高（RangeSlider::kTrackH）と縦中心を合わせる
    for (QPushButton* b : { m_playPauseBtn, m_stopBtn, m_trimBtn }) {
        b->setStyleSheet(iconBtnStyle);
        b->setFixedSize(iconBtnSize);
        b->setEnabled(false);
    }
    // 【】は横幅を正方形の半分にしてシークバーへの密着感を出す（縦は揃える）
    const QSize bracketBtnSize(iconBtnSize.width() / 2, iconBtnSize.height());
    for (QPushButton* b : { m_setInBtn, m_setOutBtn }) {
        b->setStyleSheet(iconBtnStyle);
        b->setFixedSize(bracketBtnSize);
        b->setEnabled(false);
    }

    // 左側アイコン群を内側レイアウトでまとめ、ボタン同士をピッタリ隣接させる
    auto* leftIconRow = new QHBoxLayout;
    leftIconRow->setSpacing(0);
    leftIconRow->setContentsMargins(0, 0, 0, 0);
    leftIconRow->addWidget(m_playPauseBtn);
    leftIconRow->addWidget(m_stopBtn);
    leftIconRow->addWidget(m_setInBtn);

    // 行内要素を上端で揃える
    // 左側アイコンボタン高がスライダー上段トラック高（RangeSlider::kTrackH）と一致する前提で、
    // 波形中心とボタン中心が揃う。下段の区間バー（kRangeBarH）はボタン下に張り出して描画される
    auto* seekRow = new QHBoxLayout;
    seekRow->setSpacing(0);
    seekRow->addLayout(leftIconRow);
    seekRow->setAlignment(leftIconRow, Qt::AlignTop);
    seekRow->addWidget(m_seekSlider, 1, Qt::AlignTop);
    seekRow->addWidget(m_setOutBtn,  0, Qt::AlignTop);
    seekRow->addWidget(m_trimBtn,    0, Qt::AlignTop);

    // --- 動画情報ラベル（ステータスバー左端、解像度・動画形式・音声形式） ---
    m_videoInfoLabel = new QLabel;
    m_videoInfoLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // --- 出力ファイルラベル（ステータスバー、動画情報の右） ---
    m_outputLabel = new QLabel;
    m_outputLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // --- メインレイアウト ---
    auto* central = new QWidget;
    auto* main    = new QVBoxLayout(central);
    main->setSpacing(8);
    // bottom はわずかな余白だけ残して開始/終了行とステータスバーの間隔を詰める
    main->setContentsMargins(12, 12, 12, 2);
    // 余剰スペースを全てプレビューに割り当ててウィンドウリサイズに追従させる
    main->addWidget(m_videoView, 1);
    main->addLayout(seekRow);

    setCentralWidget(central);

    // --- ステータスバー：左から動画情報・出力状況、右に再生位置・再生速度・音量・音声強調 ---
    // 項目間の縦罫線を非表示にして、ラベル先頭の半角スペースのみで間隔を作る
    statusBar()->setStyleSheet("QStatusBar::item { border: none; }");
    statusBar()->addWidget(m_videoInfoLabel);
    statusBar()->addWidget(m_outputLabel, 1);
    statusBar()->addPermanentWidget(m_posLabel);
    statusBar()->addPermanentWidget(m_speedLabel);
    statusBar()->addPermanentWidget(m_volumeLabel);
    statusBar()->addPermanentWidget(m_speechEnhanceLabel);

    // シーク要求スロットル：先頭は即時、後続は 40ms 間隔で最新値を反映
    m_seekTimer.setSingleShot(true);
    m_seekTimer.setInterval(40);
    connect(&m_seekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekMs < 0) return;
        m_videoView->setPosition(m_pendingSeekMs);
        m_pendingSeekMs = -1;
        m_seekTimer.start();
    });

    // 設定読込
    const AppConfig cfg = Config::load();
    m_ffmpegPath           = cfg.ffmpegPath;
    m_seekLeftMs           = cfg.seekLeftMs;
    m_seekRightMs          = cfg.seekRightMs;
    m_seekWheelForwardMs   = cfg.wheelForwardMs;
    m_seekWheelBackMs      = cfg.wheelBackMs;
    m_initialScreenRatio   = cfg.initialScreenRatio;
    m_playbackRate         = cfg.playbackSpeed;
    m_volume               = cfg.audioVolume;

    // g キーの「起動時デフォルトへ復元」で参照するスナップショット
    // 以降にユーザ操作で m_playbackRate / m_volume / Settings 値が変わっても、ここの値は維持する
    m_initialPlaybackRate = m_playbackRate;
    m_initialVolume       = m_volume;

    m_videoView->setVolume(m_volume);
    updateSpeedDisplay();
    updateVolumeDisplay();

    // サムネイル抽出時の ffmpeg HW デコード値を反映する
    if (m_thumbExtractor) m_thumbExtractor->setHwaccel(cfg.thumbnailHwaccel);

    // --- コンテキストメニュー用アクションを構築する ---
    // contextMenuEvent ごとにメニューを組み立てる際に使い回せるようメンバとして保持する
    m_actOpen = new QAction("ファイルを開く", this);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::onOpenFile);

    m_actCopyPath = new QAction("ファイルパスをコピー", this);
    connect(m_actCopyPath, &QAction::triggered, this, &MainWindow::onCopyFilePath);

    m_actConvert = new QAction("ファイルを変換する", this);
    connect(m_actConvert, &QAction::triggered, this, &MainWindow::onConvertOrCancel);

    m_actTrim = new QAction("ファイルをトリムする", this);
    connect(m_actTrim, &QAction::triggered, this, &MainWindow::onTrimOrCancel);

    m_actTopmost = new QAction("再生中は常に最前面に表示する", this);
    m_actTopmost->setCheckable(true);
    m_actTopmost->setChecked(Settings::instance().topmostWhilePlaying());
    connect(m_actTopmost, &QAction::toggled, this, &MainWindow::onToggleTopmost);

    m_actSingleInst = new QAction("常にひとつのプレイヤーで再生する", this);
    m_actSingleInst->setCheckable(true);
    m_actSingleInst->setChecked(Settings::instance().singleInstance());
    m_actSingleInst->setToolTip("変更は次回起動から有効");
    connect(m_actSingleInst, &QAction::toggled, this, &MainWindow::onToggleSingleInstance);

    m_actPriority = new QAction("プロセス優先度を通常以上にする", this);
    m_actPriority->setCheckable(true);
    m_actPriority->setChecked(Settings::instance().aboveNormalPriority());
    connect(m_actPriority, &QAction::toggled, this, &MainWindow::onTogglePriority);

    // 音声強調は永続化しない仕様のため起動時は常に OFF（AudioWorker も初期 OFF で生成済み）
    // QAction は持たず C キー押下のみでトグルするため、コンテキストメニュー項目は作らない
    updateSpeechEnhanceDisplay();

    updateMenuActionEnabled();
    // アプリケーション全体のキー入力を捕捉してシーク・再生制御に変換する
    qApp->installEventFilter(this);

    // 下部 UI（seekRow + statusBar + 余白）の自然高を直接合算する。
    // videoView は stretch=1 のためレイアウト後の実高は伸縮配分で揺れる。
    // 引き算ではなく構成要素の sizeHint を積み上げて算出する。
    ensurePolished();
    QLayout* mainLayout = centralWidget()->layout();
    Q_ASSERT(mainLayout);
    const QMargins cm = mainLayout->contentsMargins();
    const int spacing = mainLayout->spacing();
    m_lowerUiH = cm.top() + cm.bottom()
               + RangeSlider::kTotalH + spacing
               + statusBar()->sizeHint().height();

    const bool hasInitialPath = !initialPath.isEmpty()
        && isAcceptedMedia(initialPath) && QFile::exists(initialPath);

    if (hasInitialPath && isAudioByExtension(initialPath)) {
        // 音声ファイルは可視化前にプレビュー領域を非表示にして幅 500 で見せる。
        // 最小高も m_lowerUiH に合わせ、起動直後から最終形に近い見た目で表示する
        m_videoView->hide();
        setMinimumSize(kInitialWindowW, m_lowerUiH);
        resize(kInitialWindowW, m_lowerUiH);
    }
    else {
        // 動画／ファイル指定なし共通：500x375 を初期形とする
        setMinimumSize(kInitialWindowW, kInitialWindowH);
        resize(kInitialWindowW, kInitialWindowH);
    }

    // 初期ファイルのロードはイベントループに戻った直後に行い、show() を最速で先行させる
    // これにより、ユーザにはまずデフォルトサイズのウィンドウが表示され、続いて動画サイズへリサイズされる
    if (hasInitialPath) {
        QTimer::singleShot(0, this, [this, initialPath]() { loadFile(initialPath, true); });
    }

    // ウィンドウ表示後に検証する（show 前のダイアログ表示を避ける）
    QTimer::singleShot(0, this, &MainWindow::validateFfmpegPath);

    QThreadPool::globalInstance()->start([]() { purgeOldWaveformCache(); });

    // BT 機器のアイドル復帰時プチノイズ抑制用に、不可聴トーンを常時出力する
    // BT コーデックが無音区間でアイドル状態に入り、次の音声再開時にプチ音が乗る現象を防ぐ。
    // [audio].silence_tone_enabled=false で完全にスキップ可能（OS への常時音声出力を行わない）
    if (cfg.silenceToneEnabled) {
        m_silenceTone = new SilenceTone(this);
        m_silenceTone->setFrequency(cfg.silenceToneFreqHz);
        m_silenceTone->setAmplitude(cfg.silenceToneAmp);
        m_silenceTone->start();
    }
}

MainWindow::~MainWindow()
{
    // 子プロセスとシグナル経路の安全終了
    // デストラクタ実行中にコールバックが発火すると this が破棄済みとなり未定義動作になるため、
    // waitForFinished を挟んで確実に終わらせる。
    // Encoder は親子破棄でも QProcess の dtor が kill+wait するが、
    // av1_nvenc が長く残るケースを考慮して先制 cancel を入れて終了応答性を確保する。

    // Encoder 以外の子 QObject を先に静止させる
    // m_encoder->waitForFinished はネストイベントループ相当の挙動となり、
    // その間に他の子 QObject から queued/auto signal が MainWindow へ届くと
    // 「メンバ破棄進行中の this にスロット呼び出し」という半壊状態が生じる。
    // これを防ぐため、Encoder 待機より先にそれらの disconnect / cancel を済ませる
    stopWaveformProcess();
    if (m_thumbExtractor) m_thumbExtractor->cancelInflight(true);
    if (m_probeProc) {
        disconnect(m_probeProc, nullptr, this, nullptr);
        m_probeProc->kill();
        m_probeProc->waitForFinished(1000);
        // 親子破棄経路に乗せると ~QProcess() の waitForFinished(30000) が
        // ここで上乗せされ最長 31 秒ブロックする。setParent(nullptr) + deleteLater で切り離す
        m_probeProc->setParent(nullptr);
        m_probeProc->deleteLater();
        m_probeProc = nullptr;
    }

    // Encoder の cancel+wait
    // disconnect で finished / progress 等のシグナルがデストラクタ進行中に発火して
    // 半壊状態の this を触らないようにしてから cancel+wait する
    if (m_encoder) {
        disconnect(m_encoder, nullptr, this, nullptr);
        if (m_encoder->isRunning()) {
            m_encoder->cancel();
            if (!m_encoder->waitForFinished(3000)) {
                qWarning("MainWindow: Encoder の終了待ちが 3 秒でタイムアウトしました（プロセス終了で OS が回収します）");
                // タイムアウト時は QObject 親子破棄経路から外して破棄自体を諦める
                // 親子破棄ルートだと QProcess::~QProcess() の kill+waitForFinished(30000) が
                // ここに上乗せされ MainWindow デストラクタが最長 33 秒ブロックする。
                // setParent(nullptr) で親子破棄を切り離せばウィンドウ閉鎖は即時化できる。
                // ~MainWindow() は app.exec() リターン後に走るため deleteLater は発火せず Encoder は
                // 孤立リークするが、QApplication 終了直後にプロセスが exit するため OS が回収する。
                // 走行中の ffmpeg 子プロセスも親 Windows プロセス終了で OS により kill される
                m_encoder->setParent(nullptr);
                m_encoder->deleteLater();
                m_encoder = nullptr;
            }
        }
    }
}

// ---- ドラッグ＆ドロップ ----

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isAcceptedMedia(url.toLocalFile())) {
            // 常にコピー扱いで受理する
            // 提案アクションのまま受理すると Shift ドラッグの MoveAction を返してしまい、
            // Explorer が「移動完了」と解釈して元ファイルを削除する
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (isAcceptedMedia(path)) {
            loadFile(path);
            // dragEnterEvent と同じ理由でコピー扱いに固定する
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
}

// ---- スロット実装 ----

void MainWindow::onOpenFile()
{
    const QString filter = QString("メディアファイル (%1);;すべてのファイル (*)")
        .arg(dialogFilterFromExts());
    const QString path = QFileDialog::getOpenFileName(
        this, "メディアファイルを開く", openDialogStartDir(), filter);
    if (path.isEmpty()) return;
    loadFile(path);
}

void MainWindow::onCopyFilePath()
{
    if (m_filePath.isEmpty()) return;
    QApplication::clipboard()->setText(QDir::toNativeSeparators(m_filePath));
}

void MainWindow::onSeekSliderChanged(int value)
{
    if (m_info.duration <= 0.0) return;
    const qint64 ms = static_cast<qint64>(sliderToSec(value) * 1000.0);
    // 先頭の要求は即時反映し、後続はタイマーで 40ms ごとに最新値だけ反映する
    m_pendingSeekMs = ms;
    if (!m_seekTimer.isActive()) {
        m_videoView->setPosition(ms);
        m_pendingSeekMs = -1;
        m_seekTimer.start();
    }
}

void MainWindow::onPlayerPositionChanged(qint64 ms)
{
    const double sec = ms / 1000.0;
    m_posLabel->setText("  " + formatSec(sec) + " / " + formatSec(m_info.duration));

    if (m_info.duration <= 0.0) return;
    // ffprobe の duration と QMediaPlayer の duration がわずかにずれて末尾で kSliderMax 超になることがあるため明示クランプする
    const int value = std::clamp(static_cast<int>(sec / m_info.duration * kSliderMax), 0, kSliderMax);
    QSignalBlocker block(m_seekSlider);
    m_seekSlider->setValue(value);
}

void MainWindow::onSetIn()
{
    m_inSec = sliderToSec(m_seekSlider->value());
    m_inSet = true;
    updateRangeMarkers();
}

void MainWindow::onSetOut()
{
    m_outSec = sliderToSec(m_seekSlider->value());
    m_outSet = true;
    updateRangeMarkers();
}

void MainWindow::onStop()
{
    if (!m_info.valid) return;

    m_videoView->pause();
    m_videoView->setPosition(0);

    m_inSet  = false;
    m_outSet = false;
    m_inSec  = 0.0;
    m_outSec = m_info.duration;
    updateRangeMarkers();
}

void MainWindow::onConvertOrCancel()
{
    startOrCancel(EncodeMode::Reencode);
}

void MainWindow::onTrimOrCancel()
{
    startOrCancel(EncodeMode::StreamCopy);
}

void MainWindow::startOrCancel(EncodeMode mode)
{
    // 同モード実行中なら中止する（異モード実行中はボタン非活性で到達しない想定）
    if (m_encoder && m_encoder->isRunning()) {
        m_encoder->cancel();
        return;
    }

    // --- バリデーション ---
    if (!isFfmpegAvailable()) {
        QMessageBox::warning(this, "設定エラー",
            "ffmpeg.exe のパスが正しく設定されていません。\n"
            "avply.toml を確認してください。");
        return;
    }
    if (m_filePath.isEmpty()) {
        QMessageBox::warning(this, "入力エラー", "メディアファイルを選択してください。");
        return;
    }
    if (m_info.duration <= 0.0) {
        QMessageBox::warning(this, "入力エラー", "メディアの長さを取得できませんでした。");
        return;
    }

    // IN/OUT 未指定なら全長を自動指定する
    // 中断時に赤バーを残すことで実際に処理対象だった範囲をユーザに示す
    if (!m_inSet) {
        m_inSec = 0.0;
        m_inSet = true;
    }
    if (!m_outSet) {
        m_outSec = m_info.duration;
        m_outSet = true;
    }
    updateRangeMarkers();

    const double effectiveIn  = m_inSec;
    const double effectiveOut = m_outSec;
    if (effectiveIn >= effectiveOut) {
        QMessageBox::warning(this, "範囲エラー", "開始は終了より前に設定してください。");
        return;
    }

    // 動画の再エンコードは NVENC を使うため対応確認を行う。音声のみは libopus のみで NVENC 不要
    if (mode == EncodeMode::Reencode && !isAudioOnly()) {
        if (!Ffmpeg::checkAv1Nvenc(m_ffmpegPath)) {
            QMessageBox::critical(this, "GPU エラー",
                "av1_nvenc エンコーダが利用できません。\n"
                "NVIDIA GPU と最新ドライバを確認してください。");
            return;
        }
    }

    // 出力拡張子を決定する：
    //   変換 + 動画 → mp4、変換 + 音声 → opus、トリム → 入力拡張子を維持
    QString outExt;
    if (mode == EncodeMode::Reencode) {
        outExt = isAudioOnly() ? "opus" : "mp4";
    }
    else {
        outExt = QFileInfo(m_filePath).suffix().toLower();
    }
    const QString outputPath = OutputNamer::generate(m_filePath, outExt);

    EncodeParams params;
    params.mode         = mode;
    params.inputPath    = m_filePath;
    params.outputPath   = outputPath;
    params.inSec        = effectiveIn;
    params.outSec       = effectiveOut;
    params.inputWidth   = m_info.width;
    params.hasVideo     = !isAudioOnly();
    // 入力名が既に _mod 形式なら OutputNamer は同名パスを返すため、置換上書きを許可する
    params.allowOverwrite = OutputNamer::isModName(m_filePath);

    // 旧 Encoder を破棄してから新規生成する
    if (m_encoder) {
        disconnect(m_encoder, nullptr, this, nullptr);
        m_encoder->deleteLater();
        m_encoder = nullptr;
    }
    m_encoder = new Encoder(m_ffmpegPath, this);
    connect(m_encoder, &Encoder::progressChanged, this, &MainWindow::onEncoderProgress);
    connect(m_encoder, &Encoder::finished,        this, &MainWindow::onEncoderFinished);
    connect(m_encoder, &Encoder::releaseFileRequested, this, &MainWindow::onEncoderReleaseFile);

    const Operation op = (mode == EncodeMode::StreamCopy) ? Operation::Trim : Operation::Convert;
    const QString label = (op == Operation::Trim) ? "トリム中" : "変換中";
    m_outputLabel->setText(QString("  %1：0%").arg(label));
    m_seekSlider->setProgress(0);
    setRunning(op);

    m_encoder->encode(params);
}

void MainWindow::onEncoderProgress(int pct)
{
    m_seekSlider->setProgress(pct);
    const QString label = (m_runningOp == Operation::Trim) ? "トリム中" : "変換中";
    m_outputLabel->setText(QString("  %1：%2%").arg(label).arg(pct));
}

void MainWindow::onEncoderFinished(bool ok, const QString& outputPath, const QString& err)
{
    // 失敗表示に使う種別は setRunning(None) で m_runningOp が消える前に退避する
    const QString label = (m_runningOp == Operation::Trim) ? "トリム失敗" : "変換失敗";
    // ダイアログタイトルは label と語彙が異なるため別に退避する
    const QString title = (m_runningOp == Operation::Trim) ? "トリムエラー" : "変換エラー";
    setRunning(Operation::None);

    if (ok) {
        m_fileReleasedForOverwrite = false;
        // 完了直後に出力ファイルを開き直す
        // loadFile→onProbeFinished が区間マーカー・進捗・各ラベルをリセットするため、
        // 進捗 100% や完了ラベルの設定は不要（直後に上書きされる）
        loadFile(outputPath, false);
        return;
    }

    // 中止・失敗時は進捗オーバーレイを除去して区間表示を元に戻す
    m_seekSlider->clearProgress();

    // 同名上書きのために解放したファイルを開き直す
    // 退避リネーム失敗等で置換に至らなかった場合、プレイヤーが空のまま残るため。
    // 復元失敗の異常系では元ファイルが存在しないことがあり、そのときは開き直さない
    if (m_fileReleasedForOverwrite) {
        m_fileReleasedForOverwrite = false;
        if (QFile::exists(m_filePath)) loadFile(m_filePath, false);
    }

    // ユーザ中止：err 空文字 → ダイアログ抑制、ステータス表示もクリアのみ
    // 進捗オーバーレイの消失で中止は十分認識可能
    if (err.isEmpty()) {
        m_outputLabel->clear();
        return;
    }

    // 詳細（ffmpeg 出力末尾等）は複数行でステータスバーを崩すためダイアログのみに出す。
    // ステータスバーは種別の短文表示に留める
    m_outputLabel->setText(QString("  %1").arg(label));
    QMessageBox::critical(this, title, err);
}

void MainWindow::onEncoderReleaseFile(const QString& path)
{
    // 解放対象が現在開いているファイルでなければ何もしない
    // （変換でコンテナが変わると出力先が別パスの既存ファイルになるケースがある）
    // Windows のパスは大文字小文字を区別しないため CaseInsensitive で比較する
    if (QString::compare(QFileInfo(path).absoluteFilePath(),
                         QFileInfo(m_filePath).absoluteFilePath(),
                         Qt::CaseInsensitive) != 0) {
        return;
    }

    // 波形生成・サムネイル抽出の ffmpeg 子プロセスも入力ファイルを開いている
    // 可能性があるため、プレイヤー解放と合わせて同期停止する。
    // 直後の onEncoderFinished → loadFile で同パスを開き直すため、クリアは一瞬で済む
    stopWaveformProcess();
    if (m_thumbExtractor) m_thumbExtractor->cancelInflight(true);
    m_videoView->clear(/*keepVisible=*/true);
    m_fileReleasedForOverwrite = true;
}

// ---- 内部ユーティリティ ----

void MainWindow::showLoadError(const QString& message)
{
    m_videoView->clear();
    // 抑止フラグは直接 false 代入せず旧値へ戻す。
    // 破損ファイル 1 個に対して VideoView::loadFailed 経由と ffprobe コールバック経由が
    // 相前後して発火し得るため、外側ダイアログのネストイベントループ中に本関数が再入する。
    // 直接 false にすると内側ダイアログを閉じた時点で外側の抑止まで解け、
    // モーダル入力ブロックの対象外である IPC 経由のロードが通ってしまう
    const bool prevInhibited = m_loadInhibited;
    m_loadInhibited = true;
    QMessageBox::critical(this, "エラー", message);
    m_loadInhibited = prevInhibited;
}

void MainWindow::loadFile(const QString& rawPath, bool centerOnMonitor)
{
    if (m_loadInhibited) return;

    // 空パス・空白のみパスの早期 return
    // IPC 経由で空ペイロードが渡ると QFileInfo("").absoluteFilePath() が cwd を返してしまい、
    // 下流の setSource / ffprobe に意味のないパスが渡って追跡困難なエラーになる。
    // 半角スペース・タブのみのゴミ入力も isEmpty() では false になるため trimmed() で同時に弾く
    if (rawPath.trimmed().isEmpty()) return;

    // 入口で絶対パスに正規化する
    // CLI 引数経由でハイフン始まりの相対パス（"-bad.mp4" 等）が渡ると、
    // 下流の ffmpeg/ffprobe で `-i` 直後のトークンがオプションとして誤解釈される。
    // Windows では絶対パスは必ずドライブレター（"C:\..."）または UNC（"\\..."）で始まるため、
    // 正規化するだけでハイフン誤解釈リスクを構造的に排除できる
    const QString path = QFileInfo(rawPath).absoluteFilePath();

    // probe 完了までファイル依存 UI を一旦無効化する。
    // setSource() より前に invalidate しておくことで、setSource 経由で発火し得る
    // mediaStatusChanged 等の同期コールバックでも旧 m_info / m_filePath が参照されない
    // 同じ理由で世代番号の加算も setSource() より前に置く。setSource が同期的に InvalidMedia を
    // 出すと showLoadError の QMessageBox がネストイベントループを回し、その間に旧 probe の
    // コールバックが発火し得るため、加算が後だと旧世代がガードを通過して旧パスの結果を反映してしまう
    // 保留中のシーク要求も同時に破棄する。40ms のスロットル窓内でファイルが切り替わると、
    // 旧ファイル基準の位置が新ソースへ適用され、新ファイルが先頭から始まらないためだ
    m_seekTimer.stop();
    m_pendingSeekMs = -1;
    m_info = VideoInfo();
    m_filePath.clear();
    setWindowTitle(QStringLiteral("avply"));
    setUiEnabled(false);

    // 再入検出用の世代番号を進める（m_loadGeneration のヘッダコメント参照）
    const quint64 gen = ++m_loadGeneration;

    // QMediaPlayer の非同期ロードを ffprobe 実行と並行させて先頭フレーム表示を早める
    m_videoView->setSource(path);

    // ソース設定直後に現在の再生速度を確定させる
    // probe 完了を待つと、LoadedMedia 到達による自動再生が先行して冒頭が等速で鳴る。
    // loadFile は全ロード経路が合流する唯一の入口のため、
    // この位置に置けば rate 適用を一様に前倒しできる
    m_videoView->setPlaybackRate(m_playbackRate);

    // 旧 probe を破棄してから新規発行する。
    // 連続 D&D などで前ファイルの probe 結果が遅れて到着し、新ファイルの状態を上書きするのを防ぐ。
    // メンバは破棄処理の前にローカルへ切り離す。waitForFinished 中のイベントループ再入で
    // ネストした loadFile が m_probeProc を新プロセスへ差し替えても、
    // 後続の破棄処理が旧プロセスだけを対象とし、稼働中の新 probe を巻き込まないため
    if (m_probeProc) {
        QProcess* oldProbe = m_probeProc;
        m_probeProc = nullptr;
        disconnect(oldProbe, nullptr, this, nullptr);
        oldProbe->kill();
        // kill 後の終了を待ってから削除予約する。Running のまま遅延削除に乗ると
        // ~QProcess() の waitForFinished(30000) が GUI thread で同期実行されるため、
        // デストラクタ側と同じ waitForFinished + setParent(nullptr) で防御する
        oldProbe->waitForFinished(1000);
        oldProbe->setParent(nullptr);
        oldProbe->deleteLater();
    }

    // 待機中の再入で新しいロードに追い越されていたら以降を放棄する。
    // ここで probe を発行すると、ネストしたロードが発行済みの probe と二重になり、
    // 後着の結果が先着の状態を上書きして表示と加工対象が食い違う
    if (gen != m_loadGeneration) return;

    const QString ffprobePath = Ffmpeg::ffprobePath(m_ffmpegPath);
    m_probeProc = Ffmpeg::probeAsync(ffprobePath, path, this,
        [this, path, centerOnMonitor, gen](const VideoInfo& info, const FfmpegResult& result) {
        // 新しいロードに追い越されていたら何もしない。
        // 起動失敗経路（FfmpegRunner の errorOccurred）は QueuedConnection で遅延発火し、
        // コンテキストが proc 側のため loadFile の旧 probe 破棄では止まらない。
        // このガードは m_probeProc のクリアより前に置く。世代が古い場合の m_probeProc は
        // 既に新 probe を指しており、クリアすると稼働中のハンドルを失う
        if (gen != m_loadGeneration) return;

        // callback 返却後に FfmpegRunner 側が m_probeProc を deleteLater するため、
        // 解放済みポインタへの再アクセス（次回 loadFile 時の kill/waitForFinished 等）を
        // 避けるためポインタのみ先にクリアする
        m_probeProc = nullptr;

        if (!result.ok) {
            showLoadError("動画情報を取得できませんでした：\n" + result.err);
            return;
        }
        if (!info.valid || info.duration <= 0.0) {
            showLoadError("有効なメディアファイルではありません。");
            return;
        }
        onProbeFinished(path, info, centerOnMonitor);
    });
}

// フォルダ内の前後メディアファイルへ切替
// 列挙はキー押下ごとに行いキャッシュしない（フォルダ内容の変化へ常に追従し、実装も単純なため）。
// ソートは QCollator の numeric mode で、エクスプローラの並び（file2 < file10）と一致させる
void MainWindow::loadNeighborFile(int step)
{
    if (m_filePath.isEmpty()) return;

    const QFileInfo cur(m_filePath);
    QStringList nameFilters;
    for (const QString& ext : kVideoExts + kAudioExts) {
        nameFilters << ("*." + ext);
    }
    QFileInfoList entries = cur.absoluteDir().entryInfoList(nameFilters, QDir::Files);

    // Windows のファイル名は大文字小文字を区別しないため無視で比較する
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(),
              [&collator](const QFileInfo& a, const QFileInfo& b) {
                  return collator.compare(a.fileName(), b.fileName()) < 0;
              });

    int idx = -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].absoluteFilePath().compare(cur.absoluteFilePath(), Qt::CaseInsensitive) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return; // 現在ファイルが削除済み等で列挙に無い

    const int next = idx + step;
    if (next < 0 || next >= entries.size()) return; // 端ではラップしない

    loadFile(entries[next].absoluteFilePath(), false);
}

void MainWindow::onProbeFinished(const QString& path, const VideoInfo& info, bool centerOnMonitor)
{
    m_filePath = path;
    m_info     = info;
    m_inSet    = false;
    m_outSet   = false;
    m_inSec    = 0.0;
    m_outSec   = info.duration;

    setWindowTitle(QString("avply - %1").arg(QFileInfo(path).fileName()));
    {
        QSignalBlocker block(m_seekSlider);
        m_seekSlider->setValue(0);
    }
    m_seekSlider->clearRangeMarkers();
    m_seekSlider->clearProgress();
    m_outputLabel->clear();
    m_posLabel->setText("  00:00:00 / " + formatSec(info.duration));

    // メディア情報をステータスバー左端に表示する
    // 動画形式：解像度  fps  映像コーデック ビットレート  音声コーデック ビットレート サンプリング ch
    // 音声のみ：先頭の解像度・fps・映像コーデック表示は省略する
    QString videoInfo;
    if (!isAudioOnly()) {
        videoInfo = QString("  %1x%2").arg(info.width).arg(info.height);
        if (info.frameRate > 0.0) {
            videoInfo += "  " + QString::number(info.frameRate, 'g', 4) + "fps";
        }
        if (!info.codec.isEmpty()) {
            videoInfo += "  " + info.codec;
            if (info.videoBitrate > 0.0) {
                videoInfo += " " + QString::number(info.videoBitrate / 1.0e6, 'f', 1) + "Mbps";
            }
        }
    }
    if (!info.audioCodec.isEmpty()) {
        videoInfo += "  " + info.audioCodec;
        if (info.audioBitrate > 0.0) {
            videoInfo += " " + QString::number(static_cast<int>(info.audioBitrate / 1000.0)) + "kbps";
        }
        if (info.audioSampleRate > 0) {
            videoInfo += " " + QString::number(info.audioSampleRate / 1000.0, 'g', 3) + "kHz";
        }
        if (info.audioChannels > 0) {
            videoInfo += " " + QString::number(info.audioChannels) + "ch";
        }
    }
    m_videoInfoLabel->setText(videoInfo);

    // 読込完了に応じて動画プレビュー領域の表示／非表示を切り替える
    // 音声のみ：プレビュー領域を完全に消し、下部 UI のみのコンパクト表示にする
    // 動画あり：QQuickView コンテナの遅延表示は VideoView::mediaStatusChanged ハンドラに委ねる
    if (isAudioOnly()) {
        m_videoView->hide();
    }
    else {
        m_videoView->show();
    }

    // 読込が完了したのでファイル依存ボタンをまとめて活性化する
    setUiEnabled(true);

    // 音声波形を非同期生成する。音声ストリームが無いファイルは中央基線で代替する
    m_seekSlider->clearWaveform();
    if (!info.hasAudio()) {
        m_seekSlider->setBaseline(true);
    }
    else {
        startWaveformGeneration(path);
    }

    // ホバープレビューのソース更新
    // 音声のみは QSize() を渡して抽出を抑止する。動画は scale フィルタが
    // force_original_aspect_ratio で縦横比を保つため、固定サイズを渡せば
    // 実際の出力 PNG は元動画比に合わせて縮小される
    if (m_seekPreview) m_seekPreview->hide();
    if (m_thumbExtractor) {
        const QSize thumbSize = isAudioOnly() ? QSize() : QSize(240, 135);
        m_thumbExtractor->setSource(m_ffmpegPath, path, thumbSize);
        // probe で得た framerate を渡して preSeek を可変化する
        // 0 を渡しても ThumbnailExtractor 側で固定値フォールバックに切り替わる
        m_thumbExtractor->setFramerate(info.frameRate);
    }
    m_hoverPendingSec = -1;

    // probe 完了時点でも現在の再生速度を再適用する
    // 主たる適用は loadFile のソース設定直後だ。ここは backend がソース確定の過程で
    // rate を落とした場合の保険で、値が同じなら再適用しても副作用はない。
    // 再生速度はインスタンス起動中ずっと保持するためファイル間でリセットしない
    m_videoView->setPlaybackRate(m_playbackRate);

    // ウィンドウサイズを決定する：動画はアスペクト比連動、音声は下部 UI 高にあわせる
    // primaryScreen() も null を返しうる（QGuiApplication 初期化失敗時など）。
    // スクリーン取得不能ならサイズ調整・センタリングをスキップして安全側で抜ける
    const QScreen* sc = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!sc) return;
    const QRect geom = sc->availableGeometry();

    if (isAudioOnly()) {
        // 音声専用：プレビュー領域がないため縦サイズを下部 UI 高に固定する
        // setFixedHeight は最小・最大の双方を同値に設定するため、Qt が WM_GETMINMAXINFO 経由で
        // OS にこの制約を伝え、Windows 自身がウィンドウの縦ドラッグを禁止する
        setMinimumWidth(kInitialWindowW);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setFixedHeight(m_lowerUiH);

        // 動画→音声切替時にも最小幅へ縮める（音声 UI は最小幅で十分）
        resize(kInitialWindowW, m_lowerUiH);
    }
    else {
        // 動画のアスペクト比をウィンドウ連動の基準として更新する
        m_videoAspect = (info.height > 0)
            ? static_cast<double>(info.width) / info.height
            : 16.0 / 9.0;

        // 音声モードからの切替で残った setFixedHeight を解除する
        // setMinimumSize だけでは setFixedHeight が設定した最大高さが残るため、
        // 明示的に setMaximumHeight を呼んで縦伸縮を解放する必要がある
        setMaximumHeight(QWIDGETSIZE_MAX);
        setMinimumSize(kInitialWindowW, kInitialWindowH);

        // モニタ作業領域の指定比率を上限としてアスペクト比維持で動画サイズを縮める
        // 比率は avply.toml の [window].initial_screen_ratio で変更可能（デフォルト 0.7）
        const double maxWindowW  = geom.width()  * m_initialScreenRatio;
        const double maxWindowH  = geom.height() * m_initialScreenRatio;
        const double maxPreviewH = maxWindowH - m_lowerUiH;

        // 元動画サイズに対するスケール係数（1.0 を超えない範囲で最も小さい制約を採用）
        double scale = 1.0;
        if (info.width > maxWindowW) {
            scale = std::min(scale, maxWindowW / info.width);
        }
        if (maxPreviewH > 0 && info.height > maxPreviewH) {
            scale = std::min(scale, maxPreviewH / info.height);
        }

        const int previewW = qRound(info.width  * scale);
        const int previewH = qRound(info.height * scale);

        resize(std::max(kInitialWindowW, previewW), previewH + m_lowerUiH);
    }

    // タイトルバーを含むフレーム矩形をモニタ作業領域の中心に合わせる
    // frameGeometry は resize 直後も Windows では即時反映されるため安全
    if (centerOnMonitor) {
        QRect frame = frameGeometry();
        frame.moveCenter(geom.center());
        move(frame.topLeft());
    }
}

bool MainWindow::isAcceptedMedia(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return kVideoExts.contains(ext) || kAudioExts.contains(ext);
}

bool MainWindow::isAudioByExtension(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return kAudioExts.contains(ext);
}

void MainWindow::setUiEnabled(bool enabled)
{
    // ファイル依存ボタンは「enabled かつ動画読込済」のときのみ活性化する
    const bool fileLoaded = enabled && m_info.valid;
    const bool ffmpegOk = isFfmpegAvailable();
    m_seekSlider->setEnabled(fileLoaded);
    m_playPauseBtn->setEnabled(fileLoaded);
    m_stopBtn->setEnabled(fileLoaded);
    m_setInBtn->setEnabled(fileLoaded);
    m_setOutBtn->setEnabled(fileLoaded);
    m_trimBtn   ->setEnabled(fileLoaded && ffmpegOk && isTrimMeaningful());
    updateMenuActionEnabled();
}

void MainWindow::updateMenuActionEnabled()
{
    // 「開く」は実行中以外（m_runningOp==None）なら常に許可
    // 「変換」「トリム」はファイル読込済 + ffmpeg 存在を要求し、トリムはさらに範囲が有効である必要がある
    const bool idle      = (m_runningOp == Operation::None);
    const bool ffmpegOk  = isFfmpegAvailable();
    const bool fileReady = m_info.valid;

    if (m_actOpen)     m_actOpen->setEnabled(idle);
    if (m_actCopyPath) m_actCopyPath->setEnabled(!m_filePath.isEmpty());
    if (m_actConvert) {
        // 実行中（変換のみ）は中止操作のため有効のまま
        const bool running = (m_runningOp == Operation::Convert);
        m_actConvert->setEnabled(running || (idle && fileReady && ffmpegOk));
    }
    if (m_actTrim) {
        const bool running = (m_runningOp == Operation::Trim);
        m_actTrim->setEnabled(running || (idle && fileReady && ffmpegOk && isTrimMeaningful()));
    }
}

bool MainWindow::isTrimMeaningful() const
{
    // 実効範囲（IN/OUT 未指定なら全長）が動画全長と一致する場合、トリムは無意味
    // 浮動小数比較は秒数のため 1ms のしきい値で誤差を吸収して判定する
    if (m_info.duration <= 0.0) return false;
    const double effectiveIn  = m_inSet  ? m_inSec  : 0.0;
    const double effectiveOut = m_outSet ? m_outSec : m_info.duration;
    constexpr double eps = 0.001;
    return effectiveIn > eps || effectiveOut < m_info.duration - eps;
}

bool MainWindow::isFfmpegAvailable() const
{
    return !m_ffmpegPath.isEmpty() && QFile::exists(m_ffmpegPath);
}

void MainWindow::setRunning(Operation op)
{
    m_runningOp = op;
    const bool running = (op != Operation::None);
    if (running) m_videoView->pause();

    setUiEnabled(!running);
    // 実行中はウィンドウ全体への D&D を拒否する
    // プレビュー領域の D&D は QML DropArea 経由のため fileDropped ハンドラ側で抑止する
    setAcceptDrops(!running);
    // 実行中はプレビュー領域のマウスクリックでの再生トグルも封じる
    m_videoView->setInteractive(!running);

    // 操作中のラベル切替
    // 変換はメニュー項目のテキストを、トリムはメインボタンとメニュー項目の双方を切り替える
    if (m_actConvert) {
        m_actConvert->setText(op == Operation::Convert ? "変換を中止する" : "ファイルを変換する");
    }
    if (m_actTrim) {
        m_actTrim->setText(op == Operation::Trim ? "トリムを中止する" : "ファイルをトリムする");
    }
    // 実行中は停止記号「■」へ差し替え、停止可能であることを視覚化する
    m_trimBtn->setText(op == Operation::Trim ? QString::fromUtf8(u8"■") : QString::fromUtf8(u8"✂"));
    m_trimBtn->setToolTip(op == Operation::Trim ? "中止" : "トリム");
    if (op == Operation::Trim) m_trimBtn->setEnabled(true);

    updateMenuActionEnabled();
}

namespace {

// WM_SIZING で使う、ウィンドウフレーム外周→クライアント領域への差分マージン
struct FrameMargins {
    int left;
    int top;
    int right;
    int bottom;
};

// フレーム外周とクライアント領域の差分マージンを求める
FrameMargins computeFrameMargins(const QRect& frameGeom, const QRect& clientGeom)
{
    return {
        clientGeom.left()   - frameGeom.left(),
        clientGeom.top()    - frameGeom.top(),
        frameGeom.right()   - clientGeom.right(),
        frameGeom.bottom()  - clientGeom.bottom(),
    };
}

// ドラッグ辺をアンカー反対側に固定した上で newW/newH に書き換える
void anchorRectByEdge(RECT* r, WPARAM edge, int newW, int newH)
{
    switch (edge) {
    case WMSZ_LEFT:
        r->left   = r->right  - newW;
        r->bottom = r->top    + newH;
        break;
    case WMSZ_RIGHT:
        r->right  = r->left   + newW;
        r->bottom = r->top    + newH;
        break;
    case WMSZ_TOP:
        r->top    = r->bottom - newH;
        r->right  = r->left   + newW;
        break;
    case WMSZ_BOTTOM:
        r->bottom = r->top    + newH;
        r->right  = r->left   + newW;
        break;
    case WMSZ_TOPLEFT:
        r->top    = r->bottom - newH;
        r->left   = r->right  - newW;
        break;
    case WMSZ_TOPRIGHT:
        r->top    = r->bottom - newH;
        r->right  = r->left   + newW;
        break;
    case WMSZ_BOTTOMLEFT:
        r->bottom = r->top    + newH;
        r->left   = r->right  - newW;
        break;
    case WMSZ_BOTTOMRIGHT:
        r->bottom = r->top    + newH;
        r->right  = r->left   + newW;
        break;
    default:
        break;
    }
}

} // namespace

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    // WM_SIZING を捕まえて RECT を直接書き換え、ドラッグ中もアスペクト比を維持する。
    // 事後補正方式と異なりマウスドラッグの毎フレームに反映されるため、
    // リリース時のスナップバック（ドラッグ中サイズと最終サイズの食い違い）が起きない
    if (eventType != "windows_generic_MSG") {
        return QMainWindow::nativeEvent(eventType, message, result);
    }
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message != WM_SIZING) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    // 動画モード以外（音声・未読込）はアスペクト維持の対象外。
    // 音声モードは setFixedHeight により Qt 側で縦が固定されているため別途介在不要
    const bool stateExcluded =
        windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized);
    if (isAudioOnly() || !m_info.valid || m_lowerUiH <= 0
        || m_videoAspect <= 0.0 || stateExcluded) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    RECT* r = reinterpret_cast<RECT*>(msg->lParam);
    const WPARAM edge = msg->wParam;

    const FrameMargins fm = computeFrameMargins(frameGeometry(), geometry());

    int clientW = (r->right - r->left) - fm.left - fm.right;
    int clientH = (r->bottom - r->top) - fm.top - fm.bottom;

    // 上下辺ドラッグは高さマスター、それ以外（左右辺・角）は幅マスターとして扱う
    const bool heightMaster = (edge == WMSZ_TOP || edge == WMSZ_BOTTOM);
    if (heightMaster) {
        const int previewH = clientH - m_lowerUiH;
        if (previewH <= 0) return QMainWindow::nativeEvent(eventType, message, result);
        clientW = qRound(previewH * m_videoAspect);
    }
    else {
        const int previewH = qRound(clientW / m_videoAspect);
        clientH = previewH + m_lowerUiH;
    }

    // 最小サイズへのクランプ。
    // WM_GETMINMAXINFO 経由の Qt 最小サイズ制約は WM_SIZING の RECT 書き換え後にも適用されるが、
    // クランプによりアスペクト比が崩れるためこちら側で先に整合させる
    if (clientW < kInitialWindowW) {
        clientW = kInitialWindowW;
        clientH = qRound(clientW / m_videoAspect) + m_lowerUiH;
    }
    if (clientH < kInitialWindowH) {
        clientH = kInitialWindowH;
        const int previewH = clientH - m_lowerUiH;
        if (previewH > 0) clientW = qRound(previewH * m_videoAspect);
    }

    const int newW = clientW + fm.left + fm.right;
    const int newH = clientH + fm.top + fm.bottom;

    anchorRectByEdge(r, edge, newW, newH);

    *result = TRUE;
    return true;
}

void MainWindow::updateRangeMarkers()
{
    // 区間が変わったら過去の進捗オーバーレイは無効になる
    m_seekSlider->clearProgress();

    // 区間変化に追従してトリムボタンの活性状態を更新する
    // 実行中はこの直後に setRunning() → setUiEnabled(false) で再度無効化される
    const bool ffmpegOk = isFfmpegAvailable();
    m_trimBtn->setEnabled(m_info.valid && ffmpegOk && isTrimMeaningful());

    // 区間更新でメニュー側の「トリム」項目の活性条件も変わる
    updateMenuActionEnabled();

    if ((!m_inSet && !m_outSet) || m_info.duration <= 0.0) {
        m_seekSlider->clearRangeMarkers();
        return;
    }
    const double effectiveIn  = m_inSet  ? m_inSec  : 0.0;
    const double effectiveOut = m_outSet ? m_outSec : m_info.duration;
    const double inRatio  = effectiveIn  / m_info.duration;
    const double outRatio = effectiveOut / m_info.duration;
    m_seekSlider->setRangeMarkers(inRatio, outRatio);
}

double MainWindow::sliderToSec(int value) const
{
    if (m_info.duration <= 0.0) return 0.0;
    return static_cast<double>(value) / kSliderMax * m_info.duration;
}

QString MainWindow::formatSec(double sec)
{
    if (sec < 0.0) sec = 0.0;
    const int total = static_cast<int>(sec);
    return QString("%1:%2:%3")
        .arg(total / 3600,          2, 10, QChar('0'))
        .arg((total % 3600) / 60,   2, 10, QChar('0'))
        .arg(total % 60,            2, 10, QChar('0'));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress) {
        return QMainWindow::eventFilter(watched, event);
    }
    // モーダルダイアログ・ポップアップ表示中は素通しする
    // （ファイル選択や警告ダイアログ、コンテキストメニューを誤動作させない。
    // QMenu はモーダル扱いされないため activePopupWidget の併用が必要）
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) {
        return QMainWindow::eventFilter(watched, event);
    }
    // 実行中（変換またはトリム）はメディア操作キーのみ無効化する。
    // Alt+F4・Tab・Ctrl+C 等のシステムキーやアプリ全体のショートカットは default で素通しし、
    // ウィンドウ閉鎖やフォーカス移動をブロックしない。
    // 「メディア操作キーの集合」は下の switch の case 列挙が唯一の定義であり、
    // 各 case 先頭の running ガード（消費のみして処理しない）で実行中無効化を実現する。
    // キー追加時は case を足せば実行中無効化も同時に効き、抑止リストとの二重管理は生じない
    const auto* ke = static_cast<QKeyEvent*>(event);
    const bool running = (m_runningOp != Operation::None);

    switch (ke->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        // 無修飾はシーク。Alt 単独付きはフォルダ内の前後ファイル切替。
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // Alt 以外の修飾子付き（Ctrl+← 等）は素通しする。冒頭コメントのシステムキー契約を守るためだ。
        // Ctrl+←→ は将来の大スキップ用、Shift+←→ は将来のシーンスキップ用に未割当のまま温存する
        if (running) return true;
        const bool forward = (ke->key() == Qt::Key_Right);
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods == Qt::AltModifier) {
            loadNeighborFile(forward ? +1 : -1);
            return true;
        }
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        // 0 以下の設定はシーク無効（avply.toml [seek] の文書仕様、ホイール側ガードと統一）
        if (forward) {
            if (m_seekRightMs > 0) seekRelative(m_seekRightMs);
        }
        else {
            if (m_seekLeftMs > 0) seekRelative(-m_seekLeftMs);
        }
        return true;
    }
    case Qt::Key_Space:
        if (running) return true;
        if (m_info.duration > 0.0) m_videoView->togglePlay();
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        // 音量 ±0.05
        // 実行中は修飾子の有無に関わらず消費する（従来の抑止リストと同挙動）。
        // 修飾子付き（Shift/Ctrl/Alt/Meta）は OS/IME ショートカットと衝突しうるため素通し。
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        changeVolume((ke->key() == Qt::Key_Up) ? kVolumeStep : -kVolumeStep);
        return true;
    }
    case Qt::Key_Period: {
        // 再生速度 +0.05
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // 修飾子付き（Ctrl+. 等）はシステムショートカットと衝突するため素通しし、
        // 冒頭コメントのシステムキー契約を守る（↑↓ キーと同型のガード）
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        changePlaybackRate(kPlaybackRateStep);
        return true;
    }
    case Qt::Key_Comma: {
        // 再生速度 -0.05
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // 修飾子付き（Ctrl+, 等）はシステムショートカットと衝突するため素通しし、
        // 冒頭コメントのシステムキー契約を守る（↑↓ キーと同型のガード）
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        changePlaybackRate(-kPlaybackRateStep);
        return true;
    }
    case Qt::Key_G: {
        // 再生条件（速度/音量/音声強調）の全リセット ↔ 起動時デフォルト復元のトグル
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // 修飾子付き（Ctrl+G 等）はシステムショートカットと衝突するため素通しし、
        // 冒頭コメントのシステムキー契約を守る（↑↓ キーと同型のガード）
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        toggleGReset();
        return true;
    }
    case Qt::Key_R: {
        // 区間マーカーのみクリア（再生位置・再生状態は維持する）
        // onStop は再生位置を 0 に戻すため別実装
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // 修飾子付き（Ctrl+R 等）はシステムショートカットと衝突するため素通しし、
        // 冒頭コメントのシステムキー契約を守る（↑↓ キーと同型のガード）
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        if (m_info.valid) {
            m_inSet  = false;
            m_outSet = false;
            m_inSec  = 0.0;
            m_outSec = m_info.duration;
            updateRangeMarkers();
        }
        return true;
    }
    case Qt::Key_C: {
        // 音声強調（Clarity）の ON/OFF トグル。C は Clarity の頭文字（旧 N キーから直観性のため変更）
        // 実行中は修飾子の有無に関わらず消費する（↑↓ キーと同挙動）。
        // 修飾子付き（Ctrl+C 等）はシステムショートカットと衝突するため素通しし、
        // 冒頭コメントの「Ctrl+C 等は素通し」契約を守る（↑↓ キーと同型のガード）。
        if (running) return true;
        const auto mods = ke->modifiers() & kModifierMask;
        if (mods != Qt::NoModifier) {
            return QMainWindow::eventFilter(watched, event);
        }
        toggleSpeechEnhance();
        return true;
    }
    default:
        return QMainWindow::eventFilter(watched, event);
    }
}

void MainWindow::seekRelative(int deltaMs)
{
    if (m_info.duration <= 0.0 || deltaMs == 0) return;
    const qint64 durationMs = static_cast<qint64>(m_info.duration * 1000.0);
    const qint64 newPos = qBound(qint64(0), m_videoView->position() + deltaMs, durationMs);
    m_videoView->setPosition(newPos);
}

void MainWindow::changePlaybackRate(qreal delta)
{
    if (m_info.duration <= 0.0) return;
    // 浮動小数点の累積誤差を抑えるため 0.05 単位に丸める
    const qreal next = std::round((m_playbackRate + delta) * 100.0) / 100.0;
    m_playbackRate = qBound(kPlaybackRateMin, next, kPlaybackRateMax);
    m_videoView->setPlaybackRate(m_playbackRate);
    updateSpeedDisplay();
    m_gResetActive = false;
}

void MainWindow::updateSpeedDisplay()
{
    m_speedLabel->setText(kSpeedPrefix + QString::asprintf("x%.2f", m_playbackRate));
}

void MainWindow::changeVolume(qreal delta)
{
    // 浮動小数点の累積誤差を抑えるため 0.05 単位に丸める
    const qreal next = std::round((m_volume + delta) * 100.0) / 100.0;
    m_volume = qBound(qreal(0.0), next, qreal(1.0));
    m_videoView->setVolume(m_volume);
    updateVolumeDisplay();
    m_gResetActive = false;
}

void MainWindow::updateVolumeDisplay()
{
    m_volumeLabel->setText(kVolumePrefix + QString::asprintf("%.0f%%", m_volume * 100.0));
}

void MainWindow::toggleSpeechEnhance()
{
    m_speechEnhanceEnabled = !m_speechEnhanceEnabled;
    m_videoView->setSpeechEnhanceEnabled(m_speechEnhanceEnabled);
    updateSpeechEnhanceDisplay();
    m_gResetActive = false;
}

void MainWindow::updateSpeechEnhanceDisplay()
{
    m_speechEnhanceLabel->setText(kSpeechEnhancePrefix + (m_speechEnhanceEnabled ? "ON" : "OFF"));
}

void MainWindow::handleWheelInput(bool forward, bool shift, bool ctrl)
{
    if (m_runningOp != Operation::None) return;
    if (ctrl) {
        changePlaybackRate(forward ? kPlaybackRateStep : -kPlaybackRateStep);
        return;
    }
    if (shift) {
        changeVolume(forward ? kVolumeStep : -kVolumeStep);
        return;
    }
    const int ms = forward ? m_seekWheelForwardMs : m_seekWheelBackMs;
    if (ms > 0) seekRelative(forward ? ms : -ms);
}

void MainWindow::toggleGReset()
{
    if (m_info.duration <= 0.0) return;
    if (!m_gResetActive) {
        // 1 回目：全リセット（速度 1.00、音量 100%、音声強調 OFF）
        applyPlaybackState(1.0, 1.0);
        m_gResetActive = true;
    }
    else {
        // 2 回目：速度・音量を起動時のデフォルト値（TOML 初回読込値）へ復元
        // 音声強調は永続化しない仕様のため起動時デフォルトは常に OFF
        applyPlaybackState(m_initialPlaybackRate, m_initialVolume);
        m_gResetActive = false;
    }
}

void MainWindow::applyPlaybackState(qreal rate, qreal vol)
{
    // 速度・音量を反映し、音声強調は OFF へ倒す
    // 各 setter 経由で AudioWorker への伝搬も同時に行う
    m_playbackRate = qBound(kPlaybackRateMin, rate, kPlaybackRateMax);
    m_videoView->setPlaybackRate(m_playbackRate);
    updateSpeedDisplay();

    m_volume = qBound(qreal(0.0), vol, qreal(1.0));
    m_videoView->setVolume(m_volume);
    updateVolumeDisplay();

    // 音声強調は常に OFF へ倒す（永続化しない仕様のため復元対象にならない）
    m_speechEnhanceEnabled = false;
    m_videoView->setSpeechEnhanceEnabled(false);
    updateSpeechEnhanceDisplay();
}

QString MainWindow::openDialogStartDir() const
{
    if (!m_filePath.isEmpty()) {
        return QFileInfo(m_filePath).absolutePath();
    }
    return QDir::homePath();
}

void MainWindow::validateFfmpegPath()
{
    if (isFfmpegAvailable()) return;

    // 変換ボタンの活性は setUiEnabled が QFile::exists で都度判定するためここでは状態を持たない
    QMessageBox::warning(this, "設定エラー",
        "ffmpeg.exe のパスが見つかりません。\n"
        "実行ファイルと同階層の avply.toml に\n"
        "  [ffmpeg]\n"
        "  path = \"<ffmpeg.exe へのパス>\"\n"
        "を設定してから起動し直してください。");
}

QString MainWindow::waveformCachePath(const QString& inputPath) const
{
    // 入力パスと mtime を組み合わせてハッシュ化する
    // 同一パスでもファイル更新を検出して再生成する仕組み
    const QFileInfo fi(inputPath);
    // フィルタ識別子（"|cbrt"）を含めることでフィルタ仕様変更時に自動的に新キャッシュへ移行する
    // 旧キャッシュは別ハッシュ名のまま %TEMP% に残存、OS の一時掃除に委ねる
    const QString key = fi.absoluteFilePath()
        + "@" + QString::number(fi.lastModified().toMSecsSinceEpoch())
        + "|cbrt";
    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    // QCryptographicHash で決定論的なハッシュ値を得る
    // qHash はプロセスごとにシードがランダム化されるため、再起動で衝突せずキャッシュが機能しなくなる
    const QByteArray digest = QCryptographicHash::hash(
        key.toUtf8(), QCryptographicHash::Md5).toHex();
    return tmpDir + "/avply_wave_" + QString::fromLatin1(digest) + ".png";
}

void MainWindow::startWaveformGeneration(const QString& inputPath)
{
    // 短時間でファイルを切り替えた際に古いプロセスのコールバックが新ファイルへ
    // 誤反映するのを防ぐため、新規生成前に旧プロセスを停止する
    stopWaveformProcess();

    // ffmpeg パスが無効なら波形生成は諦める。シークバーは波形なしのまま
    if (!isFfmpegAvailable()) return;

    const QString cachePath = waveformCachePath(inputPath);

    // キャッシュヒット時は ffmpeg を起動せず即時反映する
    if (QFile::exists(cachePath)) {
        const QPixmap pix(cachePath);
        if (!pix.isNull()) {
            m_seekSlider->setWaveform(pix);
            return;
        }
    }

    m_waveformProcOutPath = cachePath;
    m_waveformProc = Ffmpeg::generateWaveform(
        m_ffmpegPath, inputPath, cachePath,
        QSize(kWaveformW, kWaveformH), this,
        [this, cachePath](bool ok, const QString& /*outputPath*/) {
        m_waveformProc = nullptr;
        m_waveformProcOutPath.clear();
        if (!ok) {
            // 生成失敗は無音動画やデコードエラー等。中央基線にフォールバックする
            m_seekSlider->setBaseline(true);
            return;
        }
        const QPixmap pix(cachePath);
        if (pix.isNull()) {
            m_seekSlider->setBaseline(true);
            return;
        }
        m_seekSlider->setWaveform(pix);
    });
}

void MainWindow::contextMenuEvent(QContextMenuEvent* event)
{
    showContextMenuAt(event->globalPos());
}

void MainWindow::showContextMenuAt(const QPoint& globalPos)
{
    QMenu menu(this);
    menu.setStyleSheet(kMenuStyle);

    // アプリ名・バージョン項目
    // クリックで GitHub のプロジェクトページをブラウザで開く
    QAction* about = menu.addAction(
        QApplication::applicationName() + " v" + QApplication::applicationVersion());
    connect(about, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/aviscaerulea/avply"));
    });
    menu.addSeparator();

    menu.addAction(m_actOpen);
    menu.addAction(m_actCopyPath);
    menu.addSeparator();
    menu.addAction(m_actConvert);
    menu.addAction(m_actTrim);
    menu.addSeparator();

    QMenu* settings = menu.addMenu("設定");
    settings->setStyleSheet(kMenuStyle);
    settings->addAction(m_actTopmost);
    settings->addAction(m_actSingleInst);
    settings->addAction(m_actPriority);

    // tooltip をメニュー項目に表示するため明示有効化する
    settings->setToolTipsVisible(true);

    menu.exec(globalPos);
}

void MainWindow::onToggleTopmost(bool checked)
{
    Settings::instance().setTopmostWhilePlaying(checked);
    applyTopmostState();
}

void MainWindow::onToggleSingleInstance(bool checked)
{
    // 単一インスタンス強制：トグル時の即時反映はせず、レジストリ保存のみ
    // 既存ウィンドウの IPC サーバ起動状態を変えると状態遷移が複雑化するため、次回起動から有効とする
    Settings::instance().setSingleInstance(checked);
}

void MainWindow::onTogglePriority(bool checked)
{
    // プロセス優先度のトグル即時反映
    // SetPriorityClass はジョブオブジェクト制限やポリシーで失敗することがあるため、
    // 戻り値を確認し失敗時は Settings に保存しない（UI チェックは次回起動時にレジストリ値で整合する）。
    // ※ 失敗時の UI 即時巻き戻しは codereview-issue.local.md に「現状維持」判断で持ち越し
    const DWORD priority = checked ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    if (SetPriorityClass(GetCurrentProcess(), priority)) {
        Settings::instance().setAboveNormalPriority(checked);
    }
    else {
        qWarning("MainWindow: SetPriorityClass(%lu) に失敗しました（GetLastError=%lu）",
                 priority, GetLastError());
    }
}

void MainWindow::applyTopmostState()
{
    // ウィンドウの最前面フラグの切り替え
    // QWindow::setFlag は Qt の windowFlags 状態と Win32 の WS_EX_TOPMOST を同時に更新し、
    // QWidget::setWindowFlag のような再 show を伴わない。Qt の内部状態と Win32 拡張スタイル
    // の不一致を防ぐため SetWindowPos を直接呼ぶより安定する
    const bool wantTopmost = Settings::instance().topmostWhilePlaying() && m_isPlaying;

    // QWindow 未作成時のガード
    QWindow* w = windowHandle();
    if (w) {
        w->setFlag(Qt::WindowStaysOnTopHint, wantTopmost);
    }

    // フラグ更新後に Z オーダーを即時反映する
    // setFlag() は WS_EX_TOPMOST の付与・解除のみで、HWND_TOPMOST 順序へのソート
    // （他アプリより上に持ち上げる）は SetWindowPos を併用する必要がある
    HWND hwnd = reinterpret_cast<HWND>(winId());
    SetWindowPos(hwnd,
                 wantTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::loadFileFromIpc(const QString& path)
{
    // 別インスタンスから引数を受け取った時の取り込み口
    // 現在処理中（変換／トリム）なら割り込まずに前面化のみ行う
    if (!path.isEmpty() && m_runningOp == Operation::None && isAcceptedMedia(path) && QFile::exists(path)) {
        loadFile(path);
    }

    // ウィンドウを最前面に持ち上げてユーザに通知する
    // 最小化されている場合は復元してから activate
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
}

void MainWindow::stopWaveformProcess()
{
    if (!m_waveformProc) return;

    // disconnect でコールバック経路を切ってから kill する
    // 同スレッド DirectConnection 想定だが、disconnect により受信側に二度と届かないことを保証する
    disconnect(m_waveformProc, nullptr, this, nullptr);

    // 削除対象パスをローカルに退避して即時クリアする
    // waitForFinished 中にイベントループが回り別経路で startWaveformGeneration が走った場合、
    // m_waveformProcOutPath を新値で上書きされる前に旧値を確保しておく
    const QString stalePath = m_waveformProcOutPath;
    m_waveformProcOutPath.clear();

    m_waveformProc->kill();

    // kill 後にプロセス終端を短時間待つ
    // Windows の DeleteFile は ffmpeg のファイルハンドルが残った状態では失敗するため、
    // QFile::remove より先に確実にプロセスを終了させる必要がある。
    // タイムアウト 1000ms は ffmpeg が SIGKILL 相当を受けて終了するのに十分な実測値
    m_waveformProc->waitForFinished(1000);

    // 親子破棄経路に乗せると ~QProcess() の waitForFinished(30000) が
    // ここで上乗せされ最長 31 秒ブロックする。setParent(nullptr) + deleteLater で切り離す
    // （probeProc・Encoder と同じ終了応答性パターン）
    m_waveformProc->setParent(nullptr);
    m_waveformProc->deleteLater();
    m_waveformProc = nullptr;

    // 中途まで書かれた可能性のある PNG を削除する
    // 次回起動時に QFile::exists ヒット → QPixmap が破損ファイルを読み込む事故を防ぐ
    if (!stalePath.isEmpty()) {
        QFile::remove(stalePath);
    }
}

// ---- シークバーホバープレビュー ----

void MainWindow::onSeekHoverMoved(int x, int sliderValue)
{
    if (!m_info.valid || m_info.duration <= 0.0) return;
    if (!m_seekPreview) return;

    const double sec      = sliderToSec(sliderValue);
    // ThumbnailExtractor::kQuantSec の粒度に合わせて量子化する。
    // 粒度を変えるなら ThumbnailExtractor 側の定数を変えるだけで両者が連動する
    const int    quantSec = static_cast<int>(sec / ThumbnailExtractor::kQuantSec)
                            * ThumbnailExtractor::kQuantSec;

    m_hoverLastX      = x;
    m_hoverPendingSec = quantSec;

    // ポップアップ位置はマウス追従で即時更新する（内部で show() も行う）
    updateSeekPreviewPosition(x);

    const QString timeText = formatSec(sec);

    // 動画のキャッシュヒット時は同期サムネ反映
    QPixmap cached;
    const bool videoHit = !isAudioOnly()
        && m_thumbExtractor
        && m_thumbExtractor->tryGetCached(quantSec, cached);

    if (videoHit) {
        m_seekPreview->setContent(cached, timeText);
        return;
    }

    // サムネ未取得（音声 or キャッシュミス）：時刻のみ即更新する
    m_seekPreview->setTimeOnly(timeText);

    // probe 未完了時（!m_info.valid）は isAudioOnly() が動画扱いの false を返すため、
    // 旧情報のサムネ抽出が走らないよう valid ガードを併記する
    if (!m_info.valid || isAudioOnly() || !m_thumbExtractor) return;

    // 走行中なら request() 内で破棄される（完走優先）。
    // 走行中ジョブの finished で connect された callback がここを再帰呼びして最新位置を取りに行く
    requestHoverThumbnail();
}

void MainWindow::requestHoverThumbnail()
{
    // onSeekHoverMoved と同じ理由で probe 完了前は弾く
    if (m_hoverPendingSec < 0 || !m_info.valid || isAudioOnly()) return;
    if (!m_thumbExtractor || !m_seekPreview) return;
    if (!m_seekPreview->isVisible()) return;

    const int target = m_hoverPendingSec;
    m_thumbExtractor->request(target,
        [this, target](bool ok, const QPixmap& pix) {
        // ok=false は走行中ガードまたは起動失敗。走行中ガード時はそのジョブの完走 callback から
        // 再 request されるため、ここからの追加チェインは行わない
        if (!ok) return;
        if (!m_seekPreview->isVisible()) return;

        // 完走したサムネは常に表示する（MPC-HC 風：時刻は最新ホバー位置、サムネは多少遅れて追従）。
        // target == m_hoverPendingSec の同期チェックは行わない。マウス移動中に不一致が続いて
        // 何も表示されない事態を避ける
        const int displaySec = (m_hoverPendingSec >= 0) ? m_hoverPendingSec : target;
        m_seekPreview->setContent(pix, formatSec(static_cast<double>(displaySec)));
        updateSeekPreviewPosition(m_hoverLastX);

        // 最新位置が遷移していれば次の取得を投げる（連鎖追従）
        if (m_hoverPendingSec != target) {
            requestHoverThumbnail();
        }
    });
}

void MainWindow::onSeekHoverLeft()
{
    m_hoverPendingSec = -1;
    if (m_thumbExtractor) m_thumbExtractor->cancelInflight(false);
    if (m_seekPreview) m_seekPreview->hide();
}

void MainWindow::updateSeekPreviewPosition(int x)
{
    if (!m_seekPreview || !m_seekSlider) return;
    const QPoint cursorGlobal = m_seekSlider->mapToGlobal(QPoint(x, 0));
    const QRect  sliderGlobal(m_seekSlider->mapToGlobal(QPoint(0, 0)),
                              m_seekSlider->size());
    const QScreen* sc = m_seekSlider->screen();
    const QRect avail = sc ? sc->availableGeometry() : QRect();
    m_seekPreview->showAt(cursorGlobal, sliderGlobal, avail);
}
