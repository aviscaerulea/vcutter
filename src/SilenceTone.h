#pragma once
#include <QObject>
#include <QTimer>

class QAudioSink;
class QIODevice;
class QMediaDevices;

// Bluetooth コーデック・ヘッドセットのアイドル復帰時プチノイズ抑制用のサイレンストーン出力
// 並列の QAudioSink から不可聴レベルの低振幅トーンを連続再生し、デフォルト音声デバイスを
// 常時アクティブに保つ。これにより BT 機器が無音区間で省電力／コーデックアイドル状態に入り、
// 次の音声開始時に「プチ」とノイズが乗る現象を防ぐ。
// QMediaPlayer の音声出力とは独立した OS ストリームとして動作するため、相互干渉はしない。
// QMediaDevices::audioOutputsChanged に接続して BT 接続/切断時に sink を再生成する
class SilenceTone : public QObject {
    Q_OBJECT
public:
    explicit SilenceTone(QObject* parent = nullptr);
    ~SilenceTone() override;

    // 周波数（Hz）を設定する
    // start() 前または stop() 後に呼ぶ。動作中の変更は次回 openSink() に反映される
    void setFrequency(double hz);

    // 振幅を設定する
    // 0.0〜1.0 の範囲で、1.0 が 16bit フルスケール。動作中の変更は次回 openSink() に反映される
    void setAmplitude(double amp);

    // トーン出力を開始する
    // 既に開始済みの場合は何もしない。デバイス未取得・未対応フォーマット時は静かにスキップする
    void start();

    // トーン出力を同期的に停止して内部リソースを解放する
    // QAudioSink::stop の同期完了を保証してから ToneDevice を delete し、
    // オーディオスレッドの readData 残留呼び出しと delete の競合を回避する
    void stop();

private slots:
    // デフォルト出力デバイスの追加・削除に追従して sink を再生成する
    void onAudioOutputsChanged();

private:
    // m_started=true の状態で sink を起動する。失敗時はメンバを nullptr のまま残す
    void openSink();

    // sink と device を同期破棄する。m_started フラグは触らない（呼び出し側責務）
    void closeSink();

    QMediaDevices* m_mediaDevices = nullptr;
    QAudioSink*    m_sink         = nullptr;
    QIODevice*     m_device       = nullptr;

    // audioOutputsChanged の連続発火を集約する debounce タイマ
    // BT 接続/切断時に通知が短時間で複数回発火しても sink 再生成は最後の 1 回に集約される。
    // stop() 内で stop() を呼ぶことで pending 起動も確実にキャンセルできる
    QTimer m_restartDebounce;

    // sink 不健全状態の自己回復タイマ
    // m_started=true なのに sink が null、または停止・エラー状態のまま残っている状態（過渡的失敗で
    // audioOutputsChanged が来ない／取りこぼした、移管後にデバイスが消失した等）を定期的に検出し、
    // onAudioOutputsChanged() 経由で closeSink→openSink の単一経路により sink を再生成する。
    // 判定条件は AudioSinkHealth.h の isSinkUnhealthy（AudioWorker と共用）だ
    QTimer m_healthCheck;

    double m_freq = 1000.0;
    double m_amp  = 0.0001;

    // start() / stop() で操作する論理状態
    // openSink() の失敗で m_sink=nullptr になっても再起動意思は維持する
    bool m_started = false;
};
