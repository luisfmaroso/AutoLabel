#include "backend/SamBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

SamBackend::SamBackend(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
{
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &SamBackend::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &SamBackend::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred,
            this, &SamBackend::onErrorOccurred);
    connect(m_process, &QProcess::finished,
            this, &SamBackend::onFinished);
    connect(m_process, &QProcess::started,
            this, &SamBackend::started);
}

SamBackend::~SamBackend()
{
    // Sever process->backend signals first: waitForFinished() below pumps the
    // event loop, and a buffered stdout line would otherwise emit
    // responseReceived() into an already-destructing owner (MainWindow).
    m_process->disconnect(this);

    if (m_process->state() != QProcess::NotRunning) {
        // Best-effort graceful shutdown, then force-kill if it lingers.
        m_process->write(QByteArrayLiteral("{\"command\":\"shutdown\"}\n"));
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(1000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

bool SamBackend::isRunning() const
{
    return m_process->state() == QProcess::Running;
}

QProcessEnvironment SamBackend::childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // A debugger (GDB is built with an embedded Python for pretty-printing) sets
    // PYTHONHOME/PYTHONPATH to its own bundled Python. A child python.exe that
    // inherits those then fails to find its stdlib ("Failed to import encodings
    // module"). Strip them so each interpreter uses its own correct defaults.
    env.remove(QStringLiteral("PYTHONHOME"));
    env.remove(QStringLiteral("PYTHONPATH"));
    return env;
}

bool SamBackend::interpreterWorks(const QString &interpreter)
{
    // A candidate "python" on PATH may be a broken launcher: the Microsoft Store
    // alias stub, or the PyManager shim that fails to bootstrap its stdlib when
    // launched from a GUI environment ("Failed to import encodings module"). We
    // probe each candidate by importing a core module -- in the SAME environment
    // QProcess will use for the real launch -- and only accept ones that succeed.
    QProcess probe;
    probe.setProcessEnvironment(childEnvironment());
    probe.start(interpreter, { QStringLiteral("-c"), QStringLiteral("import encodings") });
    if (!probe.waitForStarted(3000)) {
        qWarning().noquote() << "[SamBackend] probe failed to start:" << interpreter
                             << "->" << probe.errorString();
        return false;
    }
    if (!probe.waitForFinished(8000)) {
        qWarning().noquote() << "[SamBackend] probe timed out:" << interpreter;
        probe.kill();
        probe.waitForFinished(1000);
        return false;
    }
    const bool ok = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    if (!ok) {
        qWarning().noquote() << "[SamBackend] probe exitCode" << probe.exitCode()
                             << "stderr:"
                             << QString::fromUtf8(probe.readAllStandardError()).trimmed();
    }
    return ok;
}

QString SamBackend::locatePython()
{
    // Build an ordered candidate list, then return the first that actually runs.
    // PATH alone is unreliable on Windows: it often exposes only the Microsoft
    // Store stub or the PyManager shim, both of which can fail to bootstrap from
    // a GUI process -- while the real interpreter (which also has our installed
    // packages) lives in a versioned folder that isn't on PATH by name.
    qInfo().noquote() << "[SamBackend] env PYTHONHOME=" << qEnvironmentVariable("PYTHONHOME")
                      << "PYTHONPATH=" << qEnvironmentVariable("PYTHONPATH");

    QStringList candidates;

    // 0. Explicit override always wins (also the way to point at a venv).
    const QString envOverride = qEnvironmentVariable("AUTOLABEL_PYTHON");
    if (!envOverride.isEmpty()) {
        candidates << envOverride;
    }

#ifdef Q_OS_WIN
    const QStringList names = { "python.exe", "python3.exe" };
#else
    const QStringList names = { "python3", "python" };
#endif

    // 1. Everything named python* on PATH, in order.
    const QString rawPath = qEnvironmentVariable("PATH");
    for (const QString &dir : rawPath.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        for (const QString &name : names) {
            candidates << QDir(dir).filePath(name);
        }
    }

#ifdef Q_OS_WIN
    // 2. Known real-interpreter locations (newest first), since PATH may only
    //    expose fragile shims. Covers the python.org "install manager" runtimes
    //    and the classic per-user / all-users python.org installers.
    const auto addGlob = [&candidates](const QString &root, const QString &pattern) {
        QDir dir(root);
        if (!dir.exists()) {
            return;
        }
        const QStringList subs =
            dir.entryList({ pattern }, QDir::Dirs, QDir::Name | QDir::Reversed);
        for (const QString &sub : subs) {
            candidates << dir.filePath(sub + QStringLiteral("/python.exe"));
        }
    };
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        addGlob(localAppData + QStringLiteral("/Python"), QStringLiteral("pythoncore-*"));
        addGlob(localAppData + QStringLiteral("/Programs/Python"), QStringLiteral("Python3*"));
    }
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty()) {
        addGlob(programFiles, QStringLiteral("Python3*"));
    }
#endif

    QStringList tried;
    for (const QString &candidate : candidates) {
        const QString path = QDir::cleanPath(candidate);
        if (tried.contains(path) || !QFileInfo::exists(path)) {
            continue;
        }
        tried << path;
        if (interpreterWorks(path)) {
            qInfo().noquote() << "[SamBackend] using interpreter:" << path;
            return path;
        }
        qWarning().noquote() << "[SamBackend] skipping non-working interpreter:" << path;
    }
    return QString();
}

bool SamBackend::start()
{
    if (m_process->state() != QProcess::NotRunning) {
        return true;  // already running or starting
    }

    const QString python = locatePython();
    if (python.isEmpty()) {
        emit errorOccurred(tr("Python interpreter not found on PATH."));
        return false;
    }

    // The service script is copied next to the executable by a CMake post-build
    // step (same convention InferenceVisualizer uses for its model/DLL).
    const QString script = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("sam_service.py"));
    if (!QFileInfo::exists(script)) {
        emit errorOccurred(tr("sam_service.py not found next to the executable:\n%1")
                               .arg(script));
        return false;
    }

    m_stdoutBuffer.clear();
    m_process->setProcessEnvironment(childEnvironment());  // strip debugger's PYTHONHOME
    m_process->setProgram(python);
    m_process->setArguments({ QStringLiteral("-u"), script });  // -u: unbuffered stdio
    m_process->start();
    return true;
}

void SamBackend::stop()
{
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    sendRequest(QJsonObject{ { "command", "shutdown" } });
}

void SamBackend::loadModel(const QString &checkpoint, const QString &config,
                           const QString &sam2Root, const QString &device)
{
    sendRequest(QJsonObject{
        { "command", "load_model" },
        { "checkpoint", checkpoint },
        { "config", config },
        { "sam2_root", sam2Root },
        { "device", device },
    });
}

void SamBackend::seedMemory(const QStringList &frames, int seedFrame,
                            const QList<QPointF> &polygon)
{
    QJsonArray jsonFrames;
    for (const QString &f : frames) {
        jsonFrames.append(f);
    }
    QJsonArray jsonPolygon;
    for (const QPointF &p : polygon) {
        jsonPolygon.append(QJsonArray{ p.x(), p.y() });
    }
    sendRequest(QJsonObject{
        { "command", "track_seed" },
        { "frames", jsonFrames },
        { "seed_frame", seedFrame },
        { "polygon", jsonPolygon },
    });
}

void SamBackend::predictFrame(int targetFrame)
{
    sendRequest(QJsonObject{
        { "command", "track_step" },
        { "target", targetFrame },
    });
}

void SamBackend::resetTracking()
{
    sendRequest(QJsonObject{ { "command", "reset_tracking" } });
}

void SamBackend::sendRequest(const QJsonObject &request)
{
    if (m_process->state() != QProcess::Running) {
        emit errorOccurred(tr("SAM backend is not running."));
        return;
    }
    const QByteArray line =
        QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    m_process->write(line);
}

void SamBackend::onReadyReadStandardOutput()
{
    m_stdoutBuffer += m_process->readAllStandardOutput();

    int newline;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit errorOccurred(tr("Malformed response from backend: %1")
                                   .arg(QString::fromUtf8(line)));
            continue;
        }
        emit responseReceived(doc.object());
    }
}

void SamBackend::onReadyReadStandardError()
{
    const QByteArray err = m_process->readAllStandardError();
    if (!err.isEmpty()) {
        qWarning().noquote() << "[SamBackend/py]" << QString::fromUtf8(err).trimmed();
    }
}

void SamBackend::onErrorOccurred(QProcess::ProcessError error)
{
    emit errorOccurred(tr("Backend process error: %1").arg(static_cast<int>(error)));
}

void SamBackend::onFinished(int exitCode, QProcess::ExitStatus status)
{
    qWarning().noquote() << "[SamBackend] process finished, exit code" << exitCode
                         << "status" << static_cast<int>(status);
    emit stopped();
}
