#include "SilenceTone.h"
#include "AudioSinkHealth.h"
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QIODevice>
#include <QtMath>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <memory>

namespace {

// S16 サンプルの最大絶対値（int16 のフルスケール）
constexpr double kInt16FullScale = 32767.0;

// QAudioFormat の固定パラメータ
// 互換性重視で 48kHz / Stereo / S16 を採用する（大半の OS / BT スタックがネイティブ対応）
constexpr int kSinkSampleRate     = 48000;
constexpr int kSinkChannelCount   = 2;
constexpr int kSinkBytesPerSample = 2;

// 出力バッファ長のヒント（ms）
// アンダーフロー対策に余裕を持たせる。実際の値は backend 依存
constexpr int kSinkBufferMs = 100;

// 連続トーン生成 QIODevice
// QAudioSink::start(QIODevice*) のプル方式で要求バイト数だけサンプルを生成する。
// 内部位相 m_phase を保持することでフレーム境界での不連続を回避する
class ToneDevice : public QIODevice {
public:
    ToneDevice(int channels, double phaseStep, double amp)
        : m_channels(channels)
        , m_phaseStep(phaseStep)
        , m_amp(amp)
    {
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        if (maxlen <= 0) return 0;

        // S16LE 想定。1 フレーム = チャンネル数 × S16 1 サンプル分のバイト数
        const qint64 frameBytes = static_cast<qint64>(m_channels) * kSinkBytesPerSample;
        const qint64 frames = maxlen / frameBytes;

        // 端数バイトはゼロ埋めして maxlen 全長を返す。
        // frames=0 や部分書き込みを返すと一部 backend が EOF/Idle と誤認するため
        if (frames == 0) {
            std::memset(data, 0, static_cast<size_t>(maxlen));
            return maxlen;
        }

        for (qint64 i = 0; i < frames; ++i) {
            const double v = std::sin(m_phase) * m_amp;
            const qint16 s = static_cast<qint16>(v * kInt16FullScale);
            qint16* p = reinterpret_cast<qint16*>(data + i * frameBytes);
            for (int c = 0; c < m_channels; ++c) {
                p[c] = s;
            }
            m_phase += m_phaseStep;
            // 連続稼働時の精度劣化を防ぐため while で 2π 周期に正規化する。
            // if 1 回だけだと frames が 2π/phaseStep を跨ぐ要求で残留が累積する
            while (m_phase >= 2.0 * M_PI) m_phase -= 2.0 * M_PI;
        }

        const qint64 framedBytes = frames * frameBytes;
        const qint64 leftover    = maxlen - framedBytes;
        if (leftover > 0) {
            std::memset(data + framedBytes, 0, static_cast<size_t>(leftover));
        }
        return maxlen;
    }

    qint64 writeData(const char*, qint64) override { return 0; }

    bool isSequential() const override { return true; }

private:
    int    m_channels;
    double m_phaseStep;
    double m_amp;
    double m_phase = 0.0;
};

} // namespace

SilenceTone::SilenceTone(QObject* parent)
    : QObject(parent)
    , m_mediaDevices(new QMediaDevices(this))
{
    // BT 接続/切断や USB DAC 抜き挿しで出力デバイスリストが変化した時、
    // 古い sink は stalled state のまま残るため、リスナで sink を作り直す。
    // 100ms の debounce で短時間の連続発火（接続シーケンス中の複数通知）を 1 回に集約する
    m_restartDebounce.setSingleShot(true);
    m_restartDebounce.setInterval(100);
    connect(&m_restartDebounce, &QTimer::timeout,
            this, &SilenceTone::onAudioOutputsChanged);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, [this]() { m_restartDebounce.start(); });

    // 詳細は SilenceTone.h の m_healthCheck コメント参照
    // 30 秒は BT 機器の再接続シーケンス（通常数秒〜十数秒）が完了する想定時間より十分長く、
    // かつユーザがアプリ起動後に音声を流し始めるまでの典型的猶予内に収まる値として設定する
    m_healthCheck.setInterval(30000);
    connect(&m_healthCheck, &QTimer::timeout, this, [this]() {
        // 復旧トリガも debounce 経由に統一する。
        // 直接 onAudioOutputsChanged を呼ぶと、同時に pending な audioOutputsChanged
        // → m_restartDebounce タイマと衝突し closeSink/openSink が連続実行されて
        // サイレンストーンが一瞬切れる（closeSink 内 stop() の同期ブロックで GUI も短時間劣化する）
        // sink 喪失（null）だけでなく、デバイス消失等で停止・エラー状態のまま
        // 残った sink も不健全として作り直す（判定条件は AudioWorker と共通の isSinkUnhealthy）
        if (m_started && isSinkUnhealthy(m_sink)) {
            m_restartDebounce.start();
        }
    });
}

SilenceTone::~SilenceTone()
{
    stop();
}

void SilenceTone::setFrequency(double hz)
{
    m_freq = hz;
}

void SilenceTone::setAmplitude(double amp)
{
    m_amp = amp;
}

void SilenceTone::start()
{
    if (m_started) return;
    m_started = true;
    openSink();
    m_healthCheck.start();
}

void SilenceTone::stop()
{
    // m_started=false 後に debounce を停止する。
    // pending 中のデバイス変更通知を確実に握り潰し、stop 後の openSink 起動を防ぐ
    m_started = false;
    m_restartDebounce.stop();
    m_healthCheck.stop();
    closeSink();
}

void SilenceTone::onAudioOutputsChanged()
{
    if (!m_started) return;
    closeSink();
    openSink();
}

void SilenceTone::openSink()
{
    if (m_sink) return;

    // 互換性重視で 48kHz / Int16 / Stereo を要求する。
    // 大半の OS / BT スタックがネイティブ対応する形式
    QAudioFormat fmt;
    fmt.setSampleRate(kSinkSampleRate);
    fmt.setChannelCount(kSinkChannelCount);
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull() || !dev.isFormatSupported(fmt)) {
        // BT 過渡状態でのデバイス不在時のログ抑制
        // 切断・再接続中はデバイス不在が頻発するため qDebug に留める。
        // 永続的な互換性問題と一時的な過渡状態を区別できない以上、警告昇格はログ過多を招く
        qDebug() << "SilenceTone: skipped (device unavailable or unsupported format)";
        return;
    }

    // unique_ptr で例外安全に確立してから release してメンバへ移管する。
    // QAudioSink のコンストラクタで例外（bad_alloc 等）が発生した場合でも tone がリークしない
    const double phaseStep = 2.0 * M_PI * m_freq / static_cast<double>(fmt.sampleRate());
    std::unique_ptr<ToneDevice> tone(new ToneDevice(fmt.channelCount(), phaseStep, m_amp));
    if (!tone->open(QIODevice::ReadOnly)) {
        return;
    }

    // parent=nullptr で生成し、closeSink() の delete を唯一の所有経路とする
    std::unique_ptr<QAudioSink> sink(new QAudioSink(dev, fmt, nullptr));
    // バッファアンダーフロー対策で大きめに確保する（約 kSinkBufferMs 分のヒント）。
    // setBufferSize は backend によりヒント扱いで実際の値は bufferSize() で確認する
    sink->setBufferSize(fmt.sampleRate() * fmt.channelCount() * kSinkBytesPerSample
                        * kSinkBufferMs / 1000);
    sink->start(tone.get());

    // start() 失敗時はメンバへ移管せず破棄する。
    // 移管してしまうと、m_healthCheck の isSinkUnhealthy 判定が UnderrunError を健全とみなすため、
    // start に失敗した sink が自己回復の対象から漏れて再生不能状態でロックインされ得る。
    // ここで明示的に状態確認してから保持することで、その取りこぼしを断つ
    if (sink->error() != QAudio::NoError) {
        qDebug() << "SilenceTone: sink->start failed, error =" << sink->error();
        return;
    }

    m_device = tone.release();
    m_sink   = sink.release();
}

void SilenceTone::closeSink()
{
    // QAudioSink::stop は同期完了が保証される。
    // delete で内部 audio thread を join してから ToneDevice を解放することで
    // readData 呼び出しと device delete の競合を完全に防ぐ
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_device) {
        m_device->close();
        delete m_device;
        m_device = nullptr;
    }
}
