#include "VideoView.h"
#include "AudioWorker.h"
#include "StartupTrace.h"
#include <QPalette>
#include <QVBoxLayout>
#include <QQuickView>
#include <QQuickItem>
#include <QVideoSink>
#include <QMediaPlayer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QThread>
#include <QMetaObject>
#include <QEvent>
#include <QWheelEvent>
#include <QUrl>
#include <QDebug>
#include <algorithm>

namespace {

// QAudioBufferOutput / QAudioSink で共通使用する固定オーディオフォーマット
// 48000Hz / ステレオ / Float に固定することでサンプル処理（DSP）が単純化される
QAudioFormat makeAudioFormat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    return fmt;
}

} // namespace

VideoView::VideoView(QWidget* parent)
    : QWidget(parent)
    , m_quickView(new QQuickView)
    , m_player(new QMediaPlayer(this))
{
    // VideoView 本体の背景を即時に黒系に設定する。
    // QQuickView のネイティブサーフェス確立前にコンテナが白く見えるのを防ぐ
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x1a, 0x1a, 0x1a));
    setPalette(pal);
    setAutoFillBackground(true);

    // QML をロードして VideoOutput のシンクを QMediaPlayer に接続する。
    // threaded render loop が有効な場合、render thread が Win32 modal loop 中も
    // IDXGISwapChain::Present を独立して呼べるため、ドラッグ中も再生が継続する
    m_quickView->setColor(QColor(0x1a, 0x1a, 0x1a));
    m_quickView->setResizeMode(QQuickView::SizeRootObjectToView);

    // QML の Ready 時に VideoOutput の videoSink を QMediaPlayer に接続する。
    // setSource() は同期的に statusChanged を emit するため、必ず setSource() より前に connect する
    connect(m_quickView, &QQuickView::statusChanged,
            this, [this](QQuickView::Status status) {
        if (status != QQuickView::Ready) return;
        QObject* root = m_quickView->rootObject();
        if (!root) return;
        auto* sink = root->property("videoSink").value<QVideoSink*>();
        if (sink) {
            m_player->setVideoSink(sink);
            // 起動計測用：最初の映像フレーム到達を記録する（2 回目以降は mark 側で無視）
            connect(sink, &QVideoSink::videoFrameChanged,
                    this, []() { StartupTrace::mark("first_video_frame"); });
        }
        // QML シグナルを C++ スロットに接続する。
        // QML 側のシグネチャと不一致になっていれば connect が失敗するため警告ログで通知する
        const std::pair<const char*, const char*> qmlConns[] = {
            { SIGNAL(clicked()),
              SLOT(onQmlClicked()) },
            { SIGNAL(contextMenuRequested(qreal,qreal)),
              SLOT(onQmlContextMenuRequested(qreal,qreal)) },
            { SIGNAL(wheelScrolled(bool,bool,bool)),
              SLOT(onQmlWheelScrolled(bool,bool,bool)) },
            { SIGNAL(fileDropped(QString)),
              SLOT(onQmlFileDropped(QString)) },
        };
        for (const auto& [sig, slot] : qmlConns) {
            if (!connect(root, sig, this, slot)) {
                qWarning() << "VideoView: QML signal connect failed:" << sig;
            }
        }
    });

    m_quickView->setSource(QUrl("qrc:/VideoOutput.qml"));

    // QQuickView を QWidget として埋め込む。
    // D&D は QML 側 DropArea で受ける（createWindowContainer の埋め込み HWND が
    // 親 QWidget の dragEnter/drop へイベントを伝搬しないため）
    m_videoContainer = QWidget::createWindowContainer(m_quickView, this);
    m_videoContainer->setMinimumSize(1, 1);
    m_videoContainer->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_videoContainer);

    // 音声経路を QAudioBufferOutput + 専用スレッド AudioWorker + QAudioSink で構成する。
    // decoder thread → audio thread の QueuedConnection 経路により、
    // GUI thread が modal loop でブロックされても音声経路が独立稼働する。
    // 音声強調は永続化しない仕様のため初期 OFF で生成する
    const QAudioFormat audioFmt = makeAudioFormat();
    m_audioBuf    = new QAudioBufferOutput(audioFmt, this);
    m_audioThread = new QThread(this);
    m_audioWorker = new AudioWorker(audioFmt);
    m_audioWorker->moveToThread(m_audioThread);

    connect(m_audioThread, &QThread::started, m_audioWorker, &AudioWorker::start);
    connect(m_audioBuf, &QAudioBufferOutput::audioBufferReceived,
            m_audioWorker, &AudioWorker::onAudioBuffer,
            Qt::QueuedConnection);

    m_player->setAudioBufferOutput(m_audioBuf);
    // audio thread を HighPriority で起動する
    // 高速再生時の DSP 負荷で QAudioSink::write が間に合わず underrun が出ていたため、
    // GUI/decoder thread に対し audio sink 書き込みを優先させる。
    // TimeCriticalPriority は OS スケジューラ独占リスクがあるため避ける
    m_audioThread->start(QThread::HighPriority);

    // OS のデフォルト出力デバイス切替（BT ヘッドフォン接続、USB DAC 抜き挿し等）への追従。
    // QAudioSink は生成時点のデフォルト出力デバイスを掴んだままのため、切替を検知して
    // audio thread 上で sink を作り直さないと旧デバイスへ出力し続ける。
    // 100ms debounce は BT 接続シーケンス中の複数通知を 1 回へ集約する（SilenceTone と同じ理由）。
    // 本クラスは通知を転送するだけで、デバイス増減か切替かの判定は行わない。
    // 実束縛先を知る AudioWorker 側が現デフォルトと比較して再生成の要否を決める
    m_mediaDevices = new QMediaDevices(this);
    m_deviceChangeDebounce.setSingleShot(true);
    m_deviceChangeDebounce.setInterval(100);
    connect(&m_deviceChangeDebounce, &QTimer::timeout, this, [this]() {
        // isRunning ガードは audio thread 停止後（破棄シーケンス中）の通知への防御
        if (!m_audioWorker || !m_audioThread || !m_audioThread->isRunning()) return;
        AudioWorker* w = m_audioWorker;
        QMetaObject::invokeMethod(w, [w]() { w->switchToDefaultDevice(); },
                                  Qt::QueuedConnection);
    });
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, [this]() { m_deviceChangeDebounce.start(); });

    // Qt 6.10 の QAudioBufferOutput パスでは setPitchCompensation(true) は無視され、
    // 実際のピッチ補正は AudioWorker 内の SoundTouch が担う。将来 Qt が本パスへ補正を
    // 実装すると SoundTouch と二重補正になるため、availability ログでその時点を検知し
    // Qt 更新時に SoundTouch 側の無効化を判断する
    qDebug() << "pitchCompensationAvailability:"
             << static_cast<int>(m_player->pitchCompensationAvailability());
    m_player->setPitchCompensation(true);

    // 再生位置の伝搬。
    // 末尾到達時の pause は mediaStatusChanged の EndOfMedia ハンドラ側で行う。
    // positionChanged ベースで先取り pause する旧実装は、ffmpeg backend で mp3 等の
    // 発火粒度が数百 ms 〜 1 秒程度になるケースで「閾値到達したが末尾は踏み越えていない」
    // 区間が未再生のまま残る不具合があった
    connect(m_player, &QMediaPlayer::positionChanged,
            this, [this](qint64 pos) {
        // 新ソース読み込み中は旧ソースの遅延 positionChanged を破棄する
        if (m_primeFirstFrame) return;
        emit positionChanged(pos);
    });

    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, [this](QMediaPlayer::PlaybackState state) {
        // PausedState 以外への遷移時に末尾自動 pause フラグを解除する
        // Playing / Stopped どちらでも解除する（末尾 pause 状態の維持は Paused のみ）
        if (state != QMediaPlayer::PausedState) m_pausingAtEnd = false;
        emit playbackStateChanged(state == QMediaPlayer::PlayingState);
    });

    // メディア状態遷移ハンドラ。
    // 初回ロード完了時の自動再生と、末尾到達時の pause を扱う
    connect(m_player, &QMediaPlayer::mediaStatusChanged,
            this, [this](QMediaPlayer::MediaStatus s) {
        // 初回ロード完了時に再生を開始する
        if (m_primeFirstFrame &&
            (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia)) {
            m_primeFirstFrame = false;
            StartupTrace::mark("loaded_media");
            // hasVideo が true のときのみ QQuickView コンテナを表示する。
            // 音声のみのソースで表示すると VideoOutput に何も描画されず黒矩形が残るため、
            // 描画対象が確定したフレームでだけコンテナを可視化する。
            // 動画→音声切替時は clear(keepVisible=true) 経路でコンテナが表示されたまま
            // 残るため、明示的に隠して旧ソースの最終フレーム残留を解消する
            if (m_player->hasVideo()) {
                m_videoContainer->show();
                StartupTrace::mark("video_container_shown");
            }
            else {
                m_videoContainer->hide();
            }
            // forceReset で立てた suspend ゲートを play() 直前に解除する。
            // Blocking で audio thread と同期し、解除前に配送済みの旧ソースバッファが
            // すべて破棄済みであることを確定させる。新ソースのバッファは play() 後に
            // しか届かないため取りこぼしは生じない。
            // isRunning ガードは audio thread 停止中の Blocking 永久待ちへの防御
            if (m_audioWorker && m_audioThread && m_audioThread->isRunning()) {
                AudioWorker* w = m_audioWorker;
                QMetaObject::invokeMethod(w, [w]() { w->resumeBuffers(); },
                                          Qt::BlockingQueuedConnection);
            }
            StartupTrace::mark("audio_resumed");
            m_player->play();
            StartupTrace::mark("play_called");
            return;
        }
        // ロード失敗時はフラグを落として通知する
        // m_primeFirstFrame が立ったまま残ると以後の positionChanged が
        // 永続的に破棄され、シークバーが動かなくなる
        if (m_primeFirstFrame && s == QMediaPlayer::InvalidMedia) {
            m_primeFirstFrame = false;
            qWarning() << "VideoView: メディアのロードに失敗しました:"
                       << m_player->errorString();
            emit loadFailed(m_player->errorString());
            return;
        }
        // 末尾到達時の pause。Qt が自動で StoppedState へ遷移して位置 0 にリセットするのを
        // 抑止する。EndOfMedia は確実な末尾検出のため、ffmpeg backend の
        // positionChanged 発火粒度に依存せず未再生区間を残さない。
        // pause 直後に position を duration へ固定し、シークバー表示を末尾に張り付ける。
        // backend によっては EndOfMedia 発火時点の内部 position が dur 手前で止まっており、
        // そのまま pause すると最後の数百 ms 分シークバーが届かない。
        // 内部 m_player->setPosition は VideoView::setPosition を経由せず m_pausingAtEnd を
        // 維持するため、再 EndOfMedia 発火でも本ブランチは再入しない
        if (s == QMediaPlayer::EndOfMedia && !m_pausingAtEnd) {
            m_pausingAtEnd = true;
            m_player->pause();
            const qint64 dur = m_player->duration();
            if (dur > 0) {
                m_player->setPosition(dur);
            }
        }
    });

    // 再生エラーを avply.log へ記録する
    // ロード失敗（InvalidMedia）以外の再生中エラーは UI 通知せずログのみ残す
    connect(m_player, &QMediaPlayer::errorOccurred,
            this, [](QMediaPlayer::Error error, const QString& errorString) {
        qWarning() << "VideoView: QMediaPlayer error:"
                   << static_cast<int>(error) << errorString;
    });
}

VideoView::~VideoView()
{
    // QMediaPlayer が破棄前に videoSink へフレームを書き込まないよう先に切断する。
    // QQuickView 側の VideoOutput より QMediaPlayer のほうが寿命が長いケースに備え、
    // 明示的に nullptr を設定してフレーム転送経路を断つ
    if (m_player) m_player->setVideoSink(nullptr);

    // pending なデバイス切替通知を握り潰し、audio thread 停止後の sink 操作要求を防ぐ。
    // デストラクタ本体はイベントループを回さないため現状の発火窓は無いが、
    // 明示停止は SilenceTone::stop と同じ防御であり、将来の破棄手順変更にも耐える
    m_deviceChangeDebounce.stop();

    // audio thread の停止と worker の破棄。
    // QAudioSink は audio thread で生成しているため、quit() より前に teardown を
    // BlockingQueuedConnection で呼んで audio thread 上で sink を解放する。
    // GUI thread で sink を破棄すると thread affinity 違反になるため、この順序が必須
    if (m_audioThread) {
        // teardown 待ち中に decoder thread から audioBufferReceived が発火すると
        // event queue に積まれた onAudioBuffer が teardown 後（sink=null）に走り
        // 早期 return で済むが、teardown 前の隙間で走れば sink->bytesFree() 等の
        // 競合経路に乗る。明示的に audioBuf → audioWorker の connect を切ることで
        // 以後の新規 audioBufferReceived 発火は audioWorker へ届かなくなる。
        // ただし disconnect 時点で既に audio thread のイベントキューに積まれている
        // onAudioBuffer 1 件は実行される可能性が残るため、teardown は
        // BlockingQueuedConnection で audio thread を一旦排他することで競合を防ぐ
        if (m_audioBuf && m_audioWorker) {
            disconnect(m_audioBuf, nullptr, m_audioWorker, nullptr);
        }
        if (m_audioWorker) {
            // functor 型 invokeMethod でスロット名を文字列解決せずコンパイル時に検知する（setSource と同じ理由）
            AudioWorker* w = m_audioWorker;
            QMetaObject::invokeMethod(w, [w]() { w->teardown(); }, Qt::BlockingQueuedConnection);
        }
        m_audioThread->quit();
        m_audioThread->wait();
    }
    if (m_audioWorker) {
        delete m_audioWorker;
        m_audioWorker = nullptr;
    }
}

void VideoView::setSource(const QString& filePath)
{
    m_player->stop();
    m_primeFirstFrame = true;
    m_pausingAtEnd = false;
    // ソース切替時に sink の積み残しサンプルと SpeechEnhancer 状態を強制リセットする。
    // forceReset は throttle 適用外で必ず sink reset()→start() を実行するため、
    // 前ソースのサンプルが WASAPI バッファに残留することを防ぐ。
    // functor 型 invokeMethod でスロット名を文字列解決せずコンパイル時に検知する。
    // BlockingQueuedConnection は GUI thread を audio thread の DSP リセット完了まで
    // 数 ms 程度ブロックする。直後の m_player->setSource() で新ソースのデコードが
    // 開始される前に audio worker 側の状態リセットを完了させ、decoder thread から
    // audio thread への audioBufferReceived（QueuedConnection）が
    // forceReset 完了より先に処理される順序 race を排除する
    if (m_audioWorker) {
        AudioWorker* w = m_audioWorker;
        QMetaObject::invokeMethod(w, [w]() { w->forceReset(); }, Qt::BlockingQueuedConnection);
    }
    StartupTrace::mark("forcereset_done");
    // 同一 URL 再投入時の強制再ロード
    // QMediaPlayer::setSource は同一 URL を渡すと再ロードを省略し、
    // mediaStatusChanged(LoadedMedia) が再発火しない。
    // 自動再生フローはこの遷移を起点にしているため、
    // 一度 QUrl() で NoMedia へ落としてから新 URL を設定し直すことで、
    // 別ファイル投入時と同じ LoadingMedia → LoadedMedia の遷移を必ず発火させる
    m_player->setSource(QUrl());
    m_player->setSource(QUrl::fromLocalFile(filePath));
}

void VideoView::clear(bool keepVisible)
{
    m_primeFirstFrame = false;
    m_pausingAtEnd = false;
    m_player->stop();
    // クリアもソース切替と同等の扱いとし、WASAPI バッファに前ソースの残響を残さない。
    // BlockingQueuedConnection で audio thread のリセット完了を待つ（setSource と同じ理由）
    if (m_audioWorker) {
        AudioWorker* w = m_audioWorker;
        QMetaObject::invokeMethod(w, [w]() { w->forceReset(); }, Qt::BlockingQueuedConnection);
    }
    m_player->setSource(QUrl());
    // コンテナの hide は音声のみソースで黒矩形が残る対策。
    // 直後に映像を開き直す経路（keepVisible=true）では表示を維持してチラつきを抑える
    if (!keepVisible) m_videoContainer->hide();
}

qint64 VideoView::position() const
{
    return m_player->position();
}

void VideoView::setPosition(qint64 ms)
{
    // 手動シークで末尾自動 pause フラグを解除し、sink の積み残しと SpeechEnhancer 状態をリセットする
    m_pausingAtEnd = false;
    if (m_audioWorker) {
        AudioWorker* w = m_audioWorker;
        QMetaObject::invokeMethod(w, [w]() { w->reset(); }, Qt::QueuedConnection);
    }
    m_player->setPosition(ms);
}

void VideoView::setPlaybackRate(qreal rate)
{
    // AudioWorker 側の SoundTouch tempo を先に更新する。
    // QAudioBufferOutput が pitchCompensation を無視するため、AudioWorker 内 SoundTouch で
    // 同じ rate ぶんの時間圧縮 / 伸長を行わないと sink 流入と消費がミスマッチを起こす。
    // QMediaPlayer の rate 変更で decoder の流入レートが変わる前に tempo を合わせることで、
    // 旧 tempo 前提の入力が SoundTouch 内に滞留して sink underrun を引き起こすのを防ぐ。
    // setPlaybackRate 側は atomic 経由で受け取るため GUI thread から直接呼んでもスレッド安全
    // （従来の DirectConnection 版 invokeMethod と同義で、直接呼び出しはコンパイル時に検知される）
    if (m_audioWorker) {
        m_audioWorker->setPlaybackRate(static_cast<double>(rate));
    }
    m_player->setPlaybackRate(rate);
}

void VideoView::setVolume(double volume)
{
    if (m_audioWorker) {
        // functor 型 invokeMethod でスロット名を文字列解決せずコンパイル時に検知する（setSource と同じ理由）
        AudioWorker* w = m_audioWorker;
        const double clamped = qBound(0.0, volume, 1.0);
        QMetaObject::invokeMethod(w, [w, clamped]() { w->setVolume(clamped); }, Qt::QueuedConnection);
    }
}

void VideoView::setSpeechEnhanceEnabled(bool enabled)
{
    if (m_audioWorker) {
        // functor 型 invokeMethod でスロット名を文字列解決せずコンパイル時に検知する（setSource と同じ理由）
        AudioWorker* w = m_audioWorker;
        QMetaObject::invokeMethod(w, [w, enabled]() { w->setSpeechEnhanceEnabled(enabled); }, Qt::QueuedConnection);
    }
}

void VideoView::togglePlay()
{
    if (isPlaying()) {
        m_player->pause();
    }
    else {
        play();
    }
}

void VideoView::pause()
{
    if (isPlaying()) m_player->pause();
}

void VideoView::play()
{
    if (isPlaying()) return;
    // 末尾到達 pause 状態からの再生要求は先頭から再生する。
    // setPosition() 経由で AudioWorker::reset を発出し、前回再生末尾の
    // partial-write 残量（SoundTouch 内部バッファと m_pendingTail）が
    // 先頭区間に貼り付くプチノイズを防ぐ。
    // reset（QueuedConnection）と play() 直後の decoder バッファ送出の処理順は
    // 保証されないが、setPosition 内の race と同根の既知許容（実害は感知しづらい）
    if (m_pausingAtEnd) {
        setPosition(0);
    }
    m_player->play();
}

void VideoView::setInteractive(bool enabled)
{
    m_interactive = enabled;
}

bool VideoView::isPlaying() const
{
    return m_player->playbackState() == QMediaPlayer::PlayingState;
}

QSize VideoView::sizeHint() const
{
    return QSize(800, 450);
}

QSize VideoView::minimumSizeHint() const
{
    return QSize(320, 180);
}

void VideoView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    const auto mods = event->modifiers();
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool ctrl  = mods.testFlag(Qt::ControlModifier);
    if (delta != 0) emit wheelScrolled(delta > 0, shift, ctrl);
    event->accept();
}

void VideoView::onQmlClicked()
{
    if (m_interactive && !m_player->source().isEmpty()) {
        togglePlay();
    }
}

void VideoView::onQmlContextMenuRequested(qreal x, qreal y)
{
    // QML の real 座標を受けたまま QPointF で mapToGlobal する。
    // 高 DPI 環境では MouseArea の mouse.x/y が小数を含むため、int で受けると切り捨てで
    // メニュー表示位置がクリック位置と数ピクセルずれる
    const QPoint globalPos = m_quickView->mapToGlobal(QPointF(x, y)).toPoint();
    emit contextMenuRequested(globalPos);
}

void VideoView::onQmlWheelScrolled(bool forward, bool shift, bool ctrl)
{
    emit wheelScrolled(forward, shift, ctrl);
}

void VideoView::onQmlFileDropped(const QString& url)
{
    const QUrl parsed(url);
    if (parsed.isLocalFile()) {
        emit fileDropped(parsed.toLocalFile());
    }
}
