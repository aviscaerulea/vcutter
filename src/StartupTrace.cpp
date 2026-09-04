#include "StartupTrace.h"

#include <windows.h>

#include <atomic>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QSet>
#include <QTextStream>

#include "Config.h"

namespace {

std::atomic<bool> g_enabled{false};
QElapsedTimer     g_timer;
QMutex            g_mutex;
QFile             g_file;
QSet<QByteArray>  g_seen;

// プロセス生成から現在までの経過ミリ秒
// GetProcessTimes の生成時刻（FILETIME、100ns 単位）と現在時刻の差を取る。取得失敗時は -1
qint64 msSinceProcessCreation()
{
    FILETIME create{}, exit_{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit_, &kernel, &user)) return -1;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c{}, n{};
    c.LowPart = create.dwLowDateTime; c.HighPart = create.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;    n.HighPart = now.dwHighDateTime;
    return static_cast<qint64>((n.QuadPart - c.QuadPart) / 10000);
}

// 1 行書いて即 flush する（クラッシュや強制終了でも直前までの里程標を残す）
void writeLine(const QString& line)
{
    if (!g_file.isOpen()) return;
    QTextStream ts(&g_file);
    ts << line << '\n';
    ts.flush();
}

} // namespace

void StartupTrace::init()
{
    const QByteArray env = qgetenv("AVPLY_STARTUP_TRACE");
    if (env.isEmpty() || env == "0") return;

    g_file.setFileName(Config::exeDirectory() + "/avply_startup.log");
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;

    g_timer.start();
    g_enabled.store(true, std::memory_order_release);
    writeLine(QString("=== %1 pid=%2 process_create->main=%3ms")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(GetCurrentProcessId())
        .arg(msSinceProcessCreation()));
}

void StartupTrace::mark(const char* label)
{
    if (!g_enabled.load(std::memory_order_acquire)) return;
    const qint64 ms = g_timer.elapsed();
    QMutexLocker lock(&g_mutex);
    const QByteArray key(label);
    if (g_seen.contains(key)) return;
    g_seen.insert(key);
    writeLine(QString("%1 %2").arg(ms, 7).arg(QLatin1String(label)));
}
