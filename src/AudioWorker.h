#pragma once
#include "SpeechEnhancer.h"
#include <QObject>
#include <QAudioFormat>
#include <QAudioBuffer>
#include <QByteArray>
#include <atomic>
#include <memory>

class QAudioSink;
class QIODevice;

namespace soundtouch { class SoundTouch; }

// 専用スレッドで SpeechEnhancer DSP と QAudioSink への書き込みを担うワーカ
// audioBufferReceived は decoder thread で発火するが、本ワーカの専用スレッドに
// QueuedConnection で配送することで GUI thread のブロックから音声経路を独立させる。
// QAudioSink/QIODevice の thread affinity は所属スレッドで一貫させる必要があるため、
// sink の生成・破棄・書き込みはすべて本ワーカのスロット経由で行う
class AudioWorker : public QObject {
    Q_OBJECT
public:
    // コンストラクタ
    // format は QAudioBufferOutput に渡したフォーマットと一致させること。
    // 音声強調の初期状態は OFF（生成は start() で行う。永続化なしで起動時は常に OFF の仕様）
    explicit AudioWorker(const QAudioFormat& format,
                         QObject* parent = nullptr);
    ~AudioWorker() override;

public slots:
    // 所属スレッドで QAudioSink を生成・起動する
    // moveToThread 後、所属スレッド上で実行すること。
    // 実現手段は QThread::started シグナルへの接続だ。emit 元が audio thread 自身のため
    // AutoConnection でも所属スレッド内の直接呼び出しになる
    void start();

    // 受信したバッファに DSP・音量を適用して sink に書き込む
    void onAudioBuffer(const QAudioBuffer& buf);

    // シーク時の sink 積み残し破棄と DSP 状態リセット
    // 50ms スロットリングが効くため、短時間の連続呼び出しでは sink restart が間引かれる
    void reset();

    // ソース切替時の強制リセット
    // 前ソースのサンプルが WASAPI バッファに残らないよう必ず sink reset()→start() を実行する。
    // throttle 適用外。reset() と異なり m_workBuf / m_volumeWork もサイズゼロ化する。
    // あわせて m_suspended を立て、resumeBuffers() が呼ばれるまで受信バッファを破棄する
    void forceReset();

    // forceReset で立てた suspend を解除する
    // 新ソースの再生開始（LoadedMedia 後の play() 直前）に GUI thread から
    // BlockingQueuedConnection で呼ぶ。forceReset 後も配送され続ける旧ソースの
    // pending バッファが新ソースの先頭へ混入するのを防ぐ
    void resumeBuffers();

    // 再生音量を更新する（0.0〜1.0）
    void setVolume(double volume);

    // 音声強調の ON/OFF を切り替える
    // ApplyConfig を内部で呼ぶため audio thread からのみ実行する
    void setSpeechEnhanceEnabled(bool enabled);

    // 再生速度を更新する（音程を保ったまま時間圧縮 / 伸長する）
    // QAudioBufferOutput が pitchCompensation を無視するため AudioWorker 側で時間圧縮する。
    // 本関数は値を atomic に受け取るだけで、実際の SoundTouch::setTempo は audio thread の
    // onAudioBuffer 冒頭で適用する。そのため GUI thread から直接呼んでよい。
    // 0 以下の値は SoundTouch の前提を破るため無視する
    void setPlaybackRate(double rate);

    // デフォルト出力デバイスの切替に追従して sink を作り直す
    // QAudioSink は生成時点のデフォルト出力デバイスを掴んだまま以後の OS 側切替に
    // 追従しないため、再生成でしか新デバイスへ移れない。GUI thread の
    // QMediaDevices::audioOutputsChanged が本スロットへ QueuedConnection で届く。
    // 束縛中デバイスが現デフォルトと同じ場合、および sink 未所持
    // （start() 前 / teardown 後）の場合は何もしない
    // デフォルト出力デバイス不在（現デフォルトの id が空）の場合も、移る先が無いため現 sink を維持する
    void switchToDefaultDevice();

    // 所属スレッドで QAudioSink を停止・破棄する
    // QThread::quit() より前に BlockingQueuedConnection で実行すること
    void teardown();

private:
    // QAudioSink の生成・起動
    // start()（初回）と recoverSink()（再生成）で共用する。audio thread からのみ呼ぶ。
    // 呼び出し時点のデフォルト出力デバイスを明示指定して生成し、その id を m_sinkDeviceId へ記録する
    void createAndStartSink();

    // sink の再生成
    // 呼び出し経路は 2 つある。1 つは自己回復で、外部要因（録画ソフトのエンドポイント
    // 再構成等）が WASAPI セッションを無効化した際に onAudioBuffer 冒頭の死活チェックが呼ぶ。
    // もう 1 つはデフォルト出力デバイス切替への追従（switchToDefaultDevice）だ。
    // いずれも DSP 蓄積分は旧セッション時代の遅延サンプルのため破棄し、
    // 現行デコード位置から鳴らし直す
    void recoverSink();

    QAudioFormat m_format;
    // 音声強調 DSP（WebRTC APM ラッパ）
    // APM は ApplyConfig / ProcessStream / Initialize を同一スレッドから呼ぶ前提のため、
    // 生成は start() スロット（audio thread）で行い affinity を確定する
    std::unique_ptr<SpeechEnhancer> m_enhancer;
    QAudioSink*  m_sink    = nullptr;
    QIODevice*   m_sinkDev = nullptr;
    // 現 sink の束縛先デフォルト出力デバイスの id
    // createAndStartSink が明示指定したデバイスの id を記録するため、常に実束縛先を指す。
    // switchToDefaultDevice はこれと現デフォルトを比較し、重複した sink 再生成を抑える
    QByteArray   m_sinkDeviceId;
    double       m_volume  = 1.0;
    // DSP 処理用の作業バッファ
    // 毎呼び出しの QByteArray 確保は new/delete スパイクを招くため再利用する。
    // 必要サイズに足りないときだけ resize で拡張する
    QByteArray   m_workBuf;
    // QAudioSink への partial write 残量バッファ（pre-volume / post-enhancer の状態で保持）
    // sink の bytesFree() 不足で書ききれなかった末尾サンプルを保持し、
    // 次回 onAudioBuffer 冒頭で最優先に書き戻す。サンプル欠落（プチノイズ）を防ぐ。
    // 音量は書き込み直前に最新値を適用するため、書き込み失敗から書き戻しの間に
    // ユーザが音量を変更しても旧音量のまま出力されることを避ける
    QByteArray   m_pendingTail;
    // 音量適用用の作業バッファ
    // pre-volume のサンプル列に最新音量を乗じてコピーする一時領域。
    // pendingTail を in-place で書き換えると未書き込み残量が post-volume になり
    // 「次回再書き込み時にさらに音量が変わったら旧値が反映される」連鎖が生じるため別領域で扱う
    QByteArray   m_volumeWork;
    // GUI thread からの再生速度更新を受け取る atomic
    // SoundTouch::setTempo はスレッド安全ではないため、setPlaybackRate slot では本変数だけ更新し、
    // 実際の setTempo は onAudioBuffer 冒頭（audio thread 上）で適用する。
    // decoder の rate 変更で流入レートが変わる前に SoundTouch tempo を合わせることで、
    // SoundTouch 内の蓄積急増による sink underrun を防ぐ
    std::atomic<double> m_pendingRate{1.0};
    // audio thread 上で最後に適用済みの SoundTouch tempo
    // m_pendingRate との差分検知に使う（毎回 setTempo を呼ばない最適化）
    double       m_appliedRate = 1.0;
    // ソース切替中のバッファ破棄ゲート
    // forceReset で true にし resumeBuffers で false に戻す。true の間 onAudioBuffer は
    // 受信バッファを破棄する。QueuedConnection の配送キューに残った旧ソースのバッファが
    // forceReset 後に届いて新ソースの先頭へ混入するのを防ぐ
    bool         m_suspended = false;
    // 等速再生時の SoundTouch バイパス状態
    // rate が 1.0 のときは時間圧縮が不要なため SoundTouch を通さず raw を直接 DSP へ送る。
    // SoundTouch は tempo 1.0 でも WSOLA のオーバーラップ加算でつなぎ目に微小な不連続を生み、
    // APM の AGC ゲインがそれを可聴なプチノイズへ増幅するため等速では経路ごと外す。
    // 経路切替（bypass の ON/OFF）を検知して SoundTouch 内の旧残量を破棄するために保持する
    bool         m_bypassActive = false;
    // 1 秒集計の診断ログ用カウンタ群
    // function-local static にすると reset() / シーク跨ぎで初期化されず、
    // リセット直後の集計窓が 1 秒超 / 未満となり診断ログの誤読を招くためメンバ化する
    qint64       m_statsWinStart  = 0;
    qint64       m_statsInBytes   = 0;
    qint64       m_statsOutBytes  = 0;
    qint64       m_statsWrites    = 0;
    qint64       m_statsUnderruns = 0;
    // 初回バッファのフォーマット記録フラグ
    // reset() で false に戻し、ファイル切替・シーク後の最初のバッファでも診断ログを再出力する
    bool         m_firstBufferReported = false;
    // QAudioSink の reset()→start() スロットリング用タイムスタンプ
    // シーク連打で reset() が短時間に複数回キューイングされた際、毎回 WASAPI の
    // 完全再起動（〜30ms ブロック）を走らせると audio thread の HighPriority 占有時間が
    // 加算されて GUI 体感が劣化する。直近 50ms 以内の再 reset では sink への操作を
    // 一切スキップし、上位 DSP リセットのみで応答する。WASAPI バッファに残る旧サンプルは
    // 新規入力で上書きされるに任せる
    qint64       m_lastSinkRestartMs = 0;
    // sink 再生成（recoverSink）の再試行スロットリング用タイムスタンプ
    // デバイス完全消失中は再生成しても start が失敗し続けるため、
    // 毎バッファの空振り再生成（QAudioSink 生成コスト＋警告ログ連発）を 1 秒間隔へ抑える
    qint64       m_lastSinkRecoverMs = 0;
    // SoundTouch インスタンス
    // start() スロットで生成して所属スレッド affinity を確定する
    std::unique_ptr<soundtouch::SoundTouch> m_stretch;
};
