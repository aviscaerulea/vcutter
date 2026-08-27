#include "AudioWorker.h"
#include "AudioSinkHealth.h"
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QIODevice>
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <SoundTouch.h>
#include <algorithm>
#include <cmath>
// SSE 制御レジスタ操作（denormal flush 設定用）
#include <xmmintrin.h>
#include <pmmintrin.h>

AudioWorker::AudioWorker(const QAudioFormat& format,
                         QObject* parent)
    : QObject(parent)
    , m_format(format)
{
    // SpeechEnhancer（APM）の生成は start()（audio thread）で行う。
    // APM は生成・設定・処理を同一スレッドで完結させる前提のためここでは生成しない
}

AudioWorker::~AudioWorker() = default;

void AudioWorker::start()
{
    // audio thread の SSE 制御レジスタで denormal flush を有効化する。
    // APM 内部の IIR / フィルタは無音区間で内部状態が指数的に減衰し、
    // 数秒で denormal float（1.18e-38 未満）に到達する。x86 では denormal 演算が
    // 通常の 50〜100 倍遅く、audio thread 上で発生すると sink underrun の引き金になる。
    // FTZ/DAZ は MXCSR のスレッドローカルフラグのため、必ず audio thread 側で設定する
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    // SpeechEnhancer（APM）を所属スレッド（audio thread）で生成する。
    // ApplyConfig / ProcessStream / Initialize の呼び出しスレッドを一貫させるため、
    // コンストラクタ側では生成せず必ず本スロット経由で生成する
    m_enhancer = std::make_unique<SpeechEnhancer>(
        m_format.sampleRate(), m_format.channelCount());

    // 所属スレッド（audio thread）で QAudioSink を生成して start する。
    // QAudioSink / QIODevice の thread affinity を所属スレッドで一貫させるため、
    // コンストラクタ側では生成せず必ず本スロット経由で生成する
    createAndStartSink();

    // SoundTouch を所属スレッドで生成する。
    // QAudioBufferOutput が pitchCompensation を無視するため、AudioWorker 側で
    // playback rate に応じた時間圧縮 / 伸長を行う。setTempo は onAudioBuffer 冒頭で
    // m_pendingRate を読み取って適用する（GUI thread から DirectConnection で呼ばれても安全にするため）
    m_stretch = std::make_unique<soundtouch::SoundTouch>();
    m_stretch->setSampleRate(static_cast<uint>(m_format.sampleRate()));
    m_stretch->setChannels(static_cast<uint>(m_format.channelCount()));

    // WSOLA パラメータを音声（speech）向けに固定する
    // SoundTouch の既定は音楽向けの自動設定で、会議音声の 1.2 倍程度ではシーケンスの
    // つなぎ目に微小なサンプル不連続・局所ピークが出る。後段 APM の AGC ゲインが
    // これを増幅し、可聴なプチノイズ化していた。公式 README（TDStretch.h）が speech 用途に
    // 推奨する固定値（SEQUENCE 40 / SEEKWINDOW 15 / OVERLAP 8ms）に切り替えてつなぎ目の
    // アーティファクト自体を抑える。AA フィルタは既定（有効）のまま品質を優先する
    m_stretch->setSetting(SETTING_SEQUENCE_MS,   40);
    m_stretch->setSetting(SETTING_SEEKWINDOW_MS, 15);
    m_stretch->setSetting(SETTING_OVERLAP_MS,     8);

    const double initialRate = m_pendingRate.load(std::memory_order_relaxed);
    m_stretch->setTempo(initialRate);
    m_appliedRate = initialRate;
}

void AudioWorker::createAndStartSink()
{
    // 束縛先デバイスを明示指定して生成し、その id を記録する。
    // デバイス未指定生成でも QAudioSink は内部で同じデフォルトを引くが、
    // 引いた瞬間と記録する瞬間の間にデフォルトが変わると記録値と実束縛先が食い違う。
    // 明示指定にすることで switchToDefaultDevice の id 比較が実束縛先を必ず指す
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    m_sinkDeviceId = dev.id();
    m_sink = new QAudioSink(dev, m_format, this);

    // 内部バッファを 200ms 相当に拡張する（48kHz stereo Float の場合 76800 バイト）。
    // デフォルトの WASAPI バッファは数 ms と極小で、1.5x 等の高速再生時にデコーダから
    // バーストで届くサンプルを吸収しきれず write が partial になり音が欠ける。
    // setBufferSize は start() より前に呼ぶ必要がある
    const qsizetype bytesPerSec =
        static_cast<qsizetype>(m_format.bytesForDuration(1000 * 1000));
    m_sink->setBufferSize(bytesPerSec / 5);

    m_sinkDev = m_sink->start();
    if (!m_sinkDev) {
        qWarning() << "AudioWorker: QAudioSink::start() failed:" << m_sink->error();
    }
    // 起動時情報は qDebug 経由とする。HighPriority audio thread からの周期 qInfo は
    // OutputDebugString 同期 I/O で数 ms ブロックし sink underrun の引き金になり得るため、
    // audio thread からのログは原則 Debug 出力に抑える
    qDebug() << "AudioWorker: QAudioSink bufferSize="
             << m_sink->bufferSize()
             << "bytes (requested" << (bytesPerSec / 5) << ")";
}

void AudioWorker::recoverSink()
{
    // 自己回復（不健全 sink の作り直し）とデバイス切替追従の共通経路。
    // 旧セッション時代に DSP 段へ蓄積したサンプルは現行再生位置に対して遅延しているため、
    // reset() と同様に破棄して現行デコード位置から鳴らし直す
    if (m_enhancer) m_enhancer->reset();
    if (m_stretch) m_stretch->clear();
    m_pendingTail.clear();
    m_firstBufferReported = false;

    // 旧 sink を破棄して作り直す。再生成時点のデフォルト出力デバイスへ束縛し直すため、
    // デバイス切替にも追従する。
    // delete 前の stop() は teardown / SilenceTone::closeSink と同じ規約だ。
    // stop() は同期完了を保証し、内部 audio thread を join してから解放へ進める。
    // デバイス切替経路からは稼働中の健全な sink も破棄するため、この明示停止が要る
    if (m_sink) m_sink->stop();
    delete m_sink;
    m_sink    = nullptr;
    m_sinkDev = nullptr;
    createAndStartSink();

    // 直後の reset() が 50ms 以内に sink restart を重ねないようスロットリング時刻を更新する
    m_lastSinkRestartMs = QDateTime::currentMSecsSinceEpoch();
}

void AudioWorker::onAudioBuffer(const QAudioBuffer& buf)
{
    if (!m_sink || !m_stretch || !m_enhancer) return;

    // ソース切替中（forceReset 後〜resumeBuffers 前）は旧ソースの pending バッファを破棄する
    if (m_suspended) return;

    // sink 死活チェック（外部要因で無効化された WASAPI セッションからの自己回復）
    // 画面録画ソフト等がシステム音声キャプチャ開始時にオーディオエンドポイントを再構成すると、
    // 既存セッションが AUDCLNT_E_DEVICE_INVALIDATED で無効化され sink は停止状態へ落ちる。
    // 放置すると bytesFree() が恒久 0 となり、overflow guard の 2 秒毎破棄だけが続いて
    // 次のシーク（reset）まで無音が継続する（Aiseesoft Screen Recorder の録画開始で実機再現）。
    // SilenceTone の m_healthCheck タイマと共通の isSinkUnhealthy で不健全を検知し、sink を再生成して自動復帰する。
    // 能動停止は teardown のみ（直後に m_sink=nullptr）のため、ここでの StoppedState は異常確定。
    // デバイス完全消失時に毎バッファ再生成が空振りし続けるのを避けるため再試行は 1 秒間隔に絞る
    const bool unhealthy = !m_sinkDev || isSinkUnhealthy(m_sink);
    if (unhealthy) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastSinkRecoverMs < 1000) return;
        m_lastSinkRecoverMs = nowMs;
        qWarning() << "AudioWorker: sink unhealthy (state=" << m_sink->state()
                   << "error=" << m_sink->error() << ") — recreating sink";
        recoverSink();
        if (!m_sinkDev) return;
    }

    // GUI thread から要求された再生速度を audio thread 上で SoundTouch に反映する。
    // setTempo はスレッド安全ではないため、必ずこの経路（onAudioBuffer = audio thread）でのみ呼ぶ。
    // decoder の rate 変更が反映されたバッファが届く前に tempo を合わせることで、
    // 旧 tempo 前提で受け取った入力が SoundTouch 内に滞留し partial write の引き金になるのを防ぐ
    const double pendingRate = m_pendingRate.load(std::memory_order_relaxed);
    if (std::abs(pendingRate - m_appliedRate) > 1e-9) {
        m_stretch->setTempo(pendingRate);
        m_appliedRate = pendingRate;
    }

    // 等速再生（rate≒1.0）は SoundTouch をバイパスする。
    // tempo 1.0 でも SoundTouch は WSOLA のオーバーラップ加算でつなぎ目に微小な不連続を残し、
    // 後段 APM の AGC ゲインがそれを可聴なプチノイズへ増幅するため、等速では経路ごと外す。
    // バイパスの ON/OFF が切り替わる瞬間に SoundTouch 内の旧 tempo 残量を破棄する。
    // bypass へ入る場合は以後 putSamples しないため残量が宙に浮き、bypass から出る場合も
    // 残量は旧経路のもので不要。速度変更の瞬間に数十 ms 欠落するが体感されない
    const bool bypass = std::abs(pendingRate - 1.0) < 1e-6;
    if (bypass != m_bypassActive) {
        m_stretch->clear();
        m_bypassActive = bypass;
    }

    // 初回バッファのフォーマット記録
    // format mismatch 検出用の診断ログ。m_firstBufferReported は reset() で false に戻すため、
    // ファイル切替・シーク後の最初のバッファでも再出力される
    if (!m_firstBufferReported) {
        m_firstBufferReported = true;
        const auto& f = buf.format();
        qDebug() << "AudioWorker: first buffer format:"
                 << "rate=" << f.sampleRate()
                 << "ch=" << f.channelCount()
                 << "fmt=" << static_cast<int>(f.sampleFormat())
                 << "frames=" << buf.frameCount()
                 << "bytes=" << buf.byteCount();
    }

    if (buf.format().sampleFormat() != QAudioFormat::Float) return;

    const int channels = m_format.channelCount();
    const qsizetype inFrames = buf.frameCount();
    if (inFrames <= 0) return;

    // フェールセーフ：連続 underrun 時の蓄積暴走を抑止する
    // sink への書き戻し失敗が続くと SoundTouch 内出力キュー (numSamples)、SpeechEnhancer の
    // 出力 FIFO、m_pendingTail が入力レート分だけ際限なく膨らみ、再生レイテンシが秒単位で増大する。
    // 約 2 秒相当を超えた時点ですべて破棄して即時回復させる。聴感上は一瞬の音飛びになるが
    // 持続的なレイテンシ膨張に比べ復帰コストが圧倒的に低い。
    // チェックは putSamples の前に行う。後置きにすると当該バッファ投入分が即捨てされ、
    // overflow 復旧後の最初の出力が空となって音切れが連鎖する
    constexpr qint64 kOverflowSeconds = 2;
    const qint64 sampleRate    = m_format.sampleRate();
    const qint64 bytesPerSec   = static_cast<qint64>(m_format.bytesForDuration(1000 * 1000));
    const qint64 maxStretchFrames = sampleRate  * kOverflowSeconds;
    const qint64 maxTailBytes     = bytesPerSec * kOverflowSeconds;
    const qint64 stretchFrames    = static_cast<qint64>(m_stretch->numSamples());
    const qint64 enhancerFrames   = static_cast<qint64>(m_enhancer->availableFrames());
    if (stretchFrames > maxStretchFrames || enhancerFrames > maxStretchFrames
        || m_pendingTail.size() > maxTailBytes) {
        qWarning() << "AudioWorker: overflow guard triggered."
                   << "stretchFrames=" << stretchFrames
                   << "enhancerFrames=" << enhancerFrames
                   << "pendingTail=" << m_pendingTail.size()
                   << "— clearing all to recover";
        m_stretch->clear();
        m_enhancer->reset();
        m_pendingTail.clear();
        // 統計に underrun として 1 件計上して 1 秒ログから可視化する
        ++m_statsUnderruns;
        // 異常時はフォーマット診断ログを再出力する。
        // overflow が連発する局面こそ「直前まで何を受けていたか」の記録が重要なため、
        // 次バッファで first-buffer 経路の qDebug を再走させる
        m_firstBufferReported = false;
    }

    // 統計集計用ローカル変数
    qint64 totalInBytes  = 0;
    qint64 totalOutBytes = 0;
    int    underruns     = 0;
    int    writes        = 0;

    // ステージ 1：raw サンプル（interleaved float）を SpeechEnhancer へ投入する。
    // バイパス時は decoder 出力をそのまま、非バイパス時は SoundTouch で時間圧縮した出力を投入する。
    // APM は内部で 10ms フレーム単位に蓄積して処理し、出力 FIFO へ積む
    if (bypass) {
        m_enhancer->pushInterleaved(buf.constData<float>(), inFrames);
    }
    else {
        // SoundTouch に入力サンプルを投入し、time-stretched 出力を全量取り出して enhancer へ渡す。
        // 出力は tempo に応じて入力 frame 数 / tempo 程度のフレーム数になる。
        // 取り出しは received == 0 まで繰り返す。sink の空きとは独立に全量を enhancer へ移し、
        // sink への書き込みはステージ 3 の enhancer 出力ドレインに一本化する
        m_stretch->putSamples(buf.constData<float>(), static_cast<uint>(inFrames));

        // receiveSamples 1 回あたりの取り出し上限フレーム数
        // 4096 ≒ 85ms@48kHz。大きすぎると 1 ループが長くなり粒度が落ちる
        constexpr uint kRecvBatchFrames = 4096;
        const qsizetype batchBytes =
            static_cast<qsizetype>(kRecvBatchFrames) * channels * static_cast<qsizetype>(sizeof(float));
        if (m_workBuf.size() < batchBytes) {
            m_workBuf.resize(batchBytes);
        }
        float* recv = reinterpret_cast<float*>(m_workBuf.data());
        for (;;) {
            const uint received = m_stretch->receiveSamples(recv, kRecvBatchFrames);
            if (received == 0) break;
            m_enhancer->pushInterleaved(recv, static_cast<qsizetype>(received));
        }
    }

    // 音量適用のラムダ
    // m_pendingTail / enhancer 出力のいずれも pre-volume（post-enhancer）として保持し、
    // sink への書き込み直前にここで最新 m_volume を適用する。退避から書き戻しの間に
    // ユーザが音量を変更しても旧音量で出力されない
    auto applyVolume = [this](const char* src, qint64 bytes) -> const char* {
        if (m_volumeWork.size() < bytes) {
            m_volumeWork.resize(bytes);
        }
        const float     vol     = static_cast<float>(m_volume);
        const float*    in      = reinterpret_cast<const float*>(src);
        float*          out     = reinterpret_cast<float*>(m_volumeWork.data());
        const qsizetype samples = bytes / static_cast<qsizetype>(sizeof(float));
        for (qsizetype i = 0; i < samples; ++i) {
            out[i] = in[i] * vol;
        }
        return m_volumeWork.constData();
    };

    // sink の partial write 残量を最優先で flush する。
    // 前回 onAudioBuffer で bytesFree() 不足により書ききれなかった末尾サンプルを保持しており、
    // ここで書き戻すことでサンプル不連続点（プチノイズ）の発生を防ぐ
    if (!m_pendingTail.isEmpty()) {
        const qint64 free = m_sink->bytesFree();
        if (free <= 0) {
            // sink が満タンなのでドレインせず次回まで待つ（enhancer 出力 FIFO 側で滞留させる）
            return;
        }
        // bytesFree() は 4 の非倍数を返すことがある。applyVolume は bytes/sizeof(float) でサンプル数を
        // 切り捨てるため、非倍数のまま渡すと末尾バイトが m_volumeWork の旧データで汚染される。
        // float 境界（4 バイト）止まりだと書き残し断片がフレーム中間で始まり、異常系で断片が
        // sink 再起動なしに破棄された後 L/R 入替＋1 サンプルずれが残留するため、
        // フレーム境界（channels × float）へ切り下げる
        const qint64 frameBytes  = static_cast<qint64>(channels) * static_cast<qint64>(sizeof(float));
        const qint64 alignedFree = (free / frameBytes) * frameBytes;
        const qint64 toWrite = std::min<qint64>(alignedFree, m_pendingTail.size());
        const char*  outPtr  = applyVolume(m_pendingTail.constData(), toWrite);
        const qint64 written = m_sinkDev->write(outPtr, toWrite);
        totalInBytes  += toWrite;
        totalOutBytes += (written > 0 ? written : 0);
        ++writes;
        if (written < 0) {
            qWarning() << "AudioWorker: pendingTail write failed, discarding" << toWrite << "bytes";
            m_pendingTail.clear();
            return;
        }
        if (written == 0 && toWrite > 0) {
            ++underruns;
            return;
        }
        const qint64 consumed = written;
        if (consumed < m_pendingTail.size()) {
            // 一部しか書けなかった分を先頭から除去して保持し、enhancer ドレインは次回まで持ち越す
            // pendingTail は pre-volume のまま remove するので、次回も最新音量が適用される
            m_pendingTail.remove(0, consumed);
            ++underruns;
            return;
        }
        m_pendingTail.clear();
    }

    // ステージ 3：SpeechEnhancer の処理済み出力を取り出して音量・sink への書き込みを行う。
    // sink の空きを見ながら pullInterleaved でバッチ取り出しし、sink フル時と partial write 時の
    // 未送出分を pendingTail へ退避する。残りは enhancer 出力 FIFO に滞留し次回消化されるため欠落しない
    constexpr qsizetype kPullBatchFrames = 4096;
    const qsizetype pullBatchBytes =
        kPullBatchFrames * channels * static_cast<qsizetype>(sizeof(float));
    if (m_workBuf.size() < pullBatchBytes) {
        m_workBuf.resize(pullBatchBytes);
    }
    float* out = reinterpret_cast<float*>(m_workBuf.data());
    for (;;) {
        const qint64 free = m_sink->bytesFree();
        if (free <= 0) break;

        const qsizetype pulled = m_enhancer->pullInterleaved(out, kPullBatchFrames);
        if (pulled <= 0) break;

        const qsizetype outBytes = pulled * channels * static_cast<qsizetype>(sizeof(float));

        // sink の空きに収まる分のみ即時書き込み、残量は pre-volume のまま退避する。
        // m_workBuf は pre-volume（post-enhancer）状態のままで、音量は applyVolume 内でコピー適用する
        // pendingTail と同様にフレーム境界へ切り下げる
        const qint64 frameBytes  = static_cast<qint64>(channels) * static_cast<qint64>(sizeof(float));
        const qint64 alignedFree = (free / frameBytes) * frameBytes;
        const qint64 toWrite = std::min<qint64>(alignedFree, outBytes);
        const char*  outPtr  = applyVolume(m_workBuf.constData(), toWrite);
        const qint64 written = m_sinkDev->write(outPtr, toWrite);
        totalInBytes  += toWrite;
        totalOutBytes += (written > 0 ? written : 0);
        ++writes;
        if (written < 0) {
            qWarning() << "AudioWorker: Stage 3 write failed, skipping";
            break;
        }
        const qint64 consumed = written;

        // 未送出分は実際に書けた consumed を起点に pre-volume のまま pendingTail へ退避する。
        // 未送出分は「sink の空き不足で切り詰めた [toWrite, outBytes)」と
        // 「partial write で書けなかった [consumed, toWrite)」の 2 種があり、両者は同時に起き得る。
        // 起点を toWrite に置くと後者が sink にも pendingTail にも残らず破棄されるため、
        // consumed を唯一の起点として [consumed, outBytes) をまとめて退避する
        // （pendingTail フラッシュ側の consumed 起点処理と同じ扱いに揃える）
        if (consumed < outBytes) {
            m_pendingTail.append(m_workBuf.constData() + consumed,
                                 static_cast<qsizetype>(outBytes - consumed));
            ++underruns;
            break;
        }
    }

    // 診断ログ：1 秒ごとに集計（毎呼び出し underrun を出すと音飛びの体感悪化に繋がるため）。
    // 周期 qInfo は OutputDebugString 同期 I/O で audio thread を数 ms ブロックするため
    // qDebug に格下げし、リリースビルドでは QT_NO_DEBUG_OUTPUT で完全抑止できる位置付けにする。
    // 集計変数はメンバ化し reset() でリセットすることで、シーク直後の集計区間が不正な長さに
    // ならないようにする
    m_statsInBytes   += totalInBytes;
    m_statsOutBytes  += totalOutBytes;
    m_statsWrites    += writes;
    m_statsUnderruns += underruns;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_statsWinStart == 0) m_statsWinStart = now;
    if (now - m_statsWinStart >= 1000) {
        qDebug() << "AudioWorker: 1s stats"
                 << "in=" << (m_statsInBytes / 1024) << "KB"
                 << "out=" << (m_statsOutBytes / 1024) << "KB"
                 << "writes=" << m_statsWrites
                 << "underruns=" << m_statsUnderruns
                 << "pendingTail=" << m_pendingTail.size()
                 << "bytesFree=" << m_sink->bytesFree()
                 << "tempo=" << m_stretch->getInputOutputSampleRatio();
        m_statsWinStart  = now;
        m_statsInBytes   = 0;
        m_statsOutBytes  = 0;
        m_statsWrites    = 0;
        m_statsUnderruns = 0;
    }
}

void AudioWorker::reset()
{
    // シーク時の sink 積み残し破棄と DSP 状態リセット。
    // 50ms 以内の連打ではスロットリング側に流し、sink reset()→start() を間引く
    if (m_enhancer) m_enhancer->reset();
    if (m_stretch) m_stretch->clear();
    // sink を reset→start で再起動するため partial write 残量も破棄する（古いサンプルを再開後に書き出さない）。
    // m_workBuf / m_volumeWork はシーク連打中の audio thread malloc/free スパイクを避けるため
    // ここでは解放せず保持する（解放は forceReset でのみ行う）
    m_pendingTail.clear();
    // 1 秒集計ウィンドウもリセットする。リセット直後の集計区間が 1 秒超 / 未満で出ないようにする
    m_statsWinStart  = 0;
    m_statsInBytes   = 0;
    m_statsOutBytes  = 0;
    m_statsWrites    = 0;
    m_statsUnderruns = 0;
    m_firstBufferReported = false;
    if (!m_sink) return;
    // シーク連打スロットリング
    // 直近 50ms 以内の再 reset では sink reset()→start() をスキップする。WASAPI 完全再起動の
    // 約 30ms ブロックが audio thread に蓄積するのを防ぐ。スロットリング窓は「最後に実際に
    // restart した時刻」から 50ms とし、throttle 側 return では m_lastSinkRestartMs を
    // 更新しない。これにより 49ms 間隔の連打中でも 50ms ごとに一度は sink restart が走り、
    // WASAPI バッファに残った旧サンプルが定期的に flush される
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastSinkRestartMs < 50) {
        return;
    }
    m_lastSinkRestartMs = now;
    m_sink->reset();
    m_sinkDev = m_sink->start();
    if (!m_sinkDev) {
        qWarning() << "AudioWorker: QAudioSink::start() failed (after reset):" << m_sink->error();
    }
}

void AudioWorker::forceReset()
{
    // ソース切替時の強制リセット。throttle を無視して必ず sink を reset→start し、
    // 前ソースのサンプルが WASAPI バッファに残留することを防ぐ
    if (m_enhancer) m_enhancer->reset();
    if (m_stretch) m_stretch->clear();
    m_workBuf.clear();
    m_volumeWork.clear();
    m_pendingTail.clear();
    m_statsWinStart  = 0;
    m_statsInBytes   = 0;
    m_statsOutBytes  = 0;
    m_statsWrites    = 0;
    m_statsUnderruns = 0;
    m_firstBufferReported = false;
    // 旧ソースの pending バッファ混入を防ぐ破棄ゲートを立てる。
    // 解除は新ソースの play() 直前に resumeBuffers() で行う。
    // 新ソースのバッファは play() 開始後にしか届かないため取りこぼしは生じない
    m_suspended = true;
    if (!m_sink) return;
    m_lastSinkRestartMs = QDateTime::currentMSecsSinceEpoch();
    m_sink->reset();
    m_sinkDev = m_sink->start();
    if (!m_sinkDev) {
        qWarning() << "AudioWorker: QAudioSink::start() failed (after forceReset):" << m_sink->error();
    }
}

void AudioWorker::resumeBuffers()
{
    m_suspended = false;
}

void AudioWorker::setVolume(double volume)
{
    m_volume = volume;
}

void AudioWorker::setSpeechEnhanceEnabled(bool enabled)
{
    if (!m_enhancer) return;
    m_enhancer->setEnabled(enabled);
}

void AudioWorker::setPlaybackRate(double rate)
{
    // 再生速度を atomic に受け取り、実際の SoundTouch::setTempo は
    // onAudioBuffer 冒頭（audio thread 上）で適用する。
    // この slot は VideoView から DirectConnection で GUI thread から直接呼ばれる場合があり、
    // SoundTouch がスレッド安全でないため、ここでは触らない。
    // tempo=1.5 → 入力 1.5 秒分を出力 1 秒分に時間圧縮（ピッチ保持）
    // 範囲外 / 不正値は無視する（0 や負値は SoundTouch の前提を破る）
    if (rate <= 0.0) return;
    m_pendingRate.store(rate, std::memory_order_relaxed);
}

void AudioWorker::switchToDefaultDevice()
{
    // start() 前 / teardown 後は sink 未所持のため何もしない。
    // start() 時点のデフォルト出力デバイスで sink を生成するため取りこぼしは生じない
    if (!m_sink) return;

    // 束縛中のデバイスが現デフォルトと同じなら再生成しない。
    // GUI 側は audioOutputsChanged をそのまま転送するため、デフォルト以外のデバイス増減でも
    // 本スロットへ届く。自己回復（onAudioBuffer の死活チェック）が先に新デフォルトへ
    // 束縛し直した直後にも届く。比較を束縛側で行うことで、いずれの重複再生成も抑える
    const QByteArray currentId = QMediaDevices::defaultAudioOutput().id();
    // デフォルト出力デバイス不在（全消失、または列挙の過渡状態）では現 sink を維持する。
    // 空 id は「移る先が無い」状態であり、稼働中の sink を壊しても得るものが無い。
    // またこの状態で再生成すると Qt 側が選ぶ代替デバイスと空 id の記録が食い違い、
    // 以後の通知が毎回「差分あり」となって再生成を繰り返す
    if (currentId.isEmpty()) return;
    if (currentId == m_sinkDeviceId) return;

    // avply.log は Warning 以上のみ記録するため、追従の事実を残すには qWarning が要る。
    // 単発イベントのため audio thread のログ抑制方針（周期ログは Debug）とは競合しない
    qWarning() << "AudioWorker: default output device changed — recreating sink";

    recoverSink();
}

void AudioWorker::teardown()
{
    // audio thread 上で QAudioSink・SoundTouch を安全に破棄する。
    // QAudioSink は audio thread で生成しているため GUI thread からの delete は thread affinity 違反になる。
    // null 化だけで delete を残すと実破棄が ~VideoView の親子連鎖（GUI thread）に乗ってしまうため、
    // ここで delete まで完結させる
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
        m_sinkDev = nullptr;
    }
    m_stretch.reset();
    // SpeechEnhancer（APM）も生成スレッドと同じ audio thread で破棄して affinity を一貫させる
    m_enhancer.reset();
}
