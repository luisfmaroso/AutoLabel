#include "app/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QTextStream>

// ---------------------------------------------------------------------------
// Diagnostic logging
//
// On Windows, a Qt GUI executable (linked WIN32) detaches from the console,
// so qDebug() output goes to OutputDebugString() and is invisible to VS Code's
// Debug Console. We install a custom message handler that mirrors every
// qDebug/qWarning/qCritical message to a log file next to the .exe.
//
// The file is truncated on each launch so it always reflects the latest run.
// (Same approach as InferenceVisualizer -- kept identical on purpose so the
// two apps behave the same way when something goes wrong.)
// ---------------------------------------------------------------------------
namespace {

QFile       g_logFile;
QTextStream g_logStream;
QMutex      g_logMutex;

void fileMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    QMutexLocker lock(&g_logMutex);
    if (!g_logFile.isOpen()) {
        return;
    }
    const char *level = "DBG ";
    switch (type) {
        case QtDebugMsg:    level = "DBG "; break;
        case QtInfoMsg:     level = "INFO"; break;
        case QtWarningMsg:  level = "WARN"; break;
        case QtCriticalMsg: level = "CRIT"; break;
        case QtFatalMsg:    level = "FATL"; break;
    }
    g_logStream << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
                << ' ' << level << ' ';
    if (ctx.category && QString::fromUtf8(ctx.category) != QStringLiteral("default")) {
        g_logStream << '[' << ctx.category << "] ";
    }
    g_logStream << msg << '\n';
    g_logStream.flush();
}

void installFileLogger()
{
    const QString path = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/autolabel.log");
    g_logFile.setFileName(path);
    if (g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        g_logStream.setDevice(&g_logFile);
        qInstallMessageHandler(fileMessageHandler);
        qDebug() << "===== AutoLabel log opened =====" << "path:" << path;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("AutoLabel");
    QApplication::setApplicationName("AutoLabel");
    QApplication::setApplicationVersion("0.1.0");

    installFileLogger();

    MainWindow window;
    window.show();

    return QApplication::exec();
}
