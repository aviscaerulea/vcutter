#pragma once

// 起動〜再生開始の里程標計測
// 環境変数 AVPLY_STARTUP_TRACE が空でも "0" でもないときだけ有効になり、
// 実行ファイルと同階層の avply_startup.log へ経過時間を追記する。
// 無効時の mark() はアトミックフラグ 1 回の読み取りで抜けるため、
// 音声バッファや映像フレームごとの hot path に置いても実害がない。
// avply.log を使わないのは、avplyMessageHandler が Warning 以上しか記録しない設計のためだ
// （qInfo/qDebug は非 GUI thread も含めどこにも出ない）
namespace StartupTrace {
    // main() 冒頭で 1 回だけ呼ぶ。有効判定・基準時刻の確定・セッション見出し行の出力を行う。
    // 見出し行にはプロセス生成から main() 突入までの時間（DLL ロード等の main 前コスト）を
    // GetProcessTimes の生成時刻から算出して含める
    void init();

    // 里程標を 1 行記録する。同じ label は最初の 1 回だけ記録し、2 回目以降は無視する
    // （first_audio_buffer 等を毎バッファ無条件に呼べるようにするため）。
    // どのスレッドから呼んでもよい
    void mark(const char* label);
}
