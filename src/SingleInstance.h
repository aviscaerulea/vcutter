#pragma once
#include <QString>

class MainWindow;
class QObject;

// 単一インスタンス強制と引数転送（QLocalServer / QLocalSocket 利用）
// 「常にひとつのプレイヤーで再生する」設定が ON のときに使用する
namespace SingleInstance {

// primary 確定を試みる（名前付き mutex によるアトミック判定）
// 同時複数起動でも primary は必ず 1 プロセスに決まる。
// プロセス内で 1 回だけ呼ぶ前提（呼ぶ度に mutex ハンドルがひとつ増える）。
// 戻り値 true：自身が primary（CreateMutexW 失敗で判定不能の場合も primary として続行）
// 戻り値 false：既存 primary あり、forwardWithRetry で転送すべき
// mutex ハンドルは意図的に保持し続ける（プロセス終了時に OS が解放する）
bool tryBecomePrimary();

// 既存インスタンスへの引数転送を試みる
// 戻り値 true：既存に転送済み、呼び出し側は exit すべき
// 戻り値 false：転送できなかった（既存が listen 未開始、応答なし、ack 不一致のいずれか）。
// primary 判定は行わず、false 後の扱いは forwardWithRetry に委ねる
// arg は空文字でも構わない（その場合は接続のみで前面化要求のシグナルになる）
bool tryForwardAndExit(const QString& arg);

// リトライ付きの引数転送
// 既存 primary が起動直後で listen 未開始のケースを吸収するため、
// tryForwardAndExit を一定間隔でリトライする。
// 戻り値 false：タイムアウト（primary 異常終了等）。呼び出し側が primary として続行する
bool forwardWithRetry(const QString& arg);

// primary プロセスとして IPC サーバを起動する
// 受信したパスを win->loadFileFromIpc() に転送する
// parent はサーバオブジェクトの寿命管理用（通常は MainWindow を渡す）
void startServer(MainWindow* win, QObject* parent);

} // namespace SingleInstance
