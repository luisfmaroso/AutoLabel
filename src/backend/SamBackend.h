#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QProcess>
#include <QStringList>

// SamBackend: the C++ side of the bridge to the Python SAM service. It owns a
// long-lived QProcess running sam_service.py and speaks line-delimited JSON over
// its stdin/stdout. Asynchronous by design: requests are written and responses
// arrive later via responseReceived() -- the GUI thread never blocks waiting on
// (eventually slow) SAM2 inference.
//
// This is the analogue of InferenceVisualizer's "controller as facade": the rest
// of the app talks to SamBackend's signals/slots and never sees QProcess.
class SamBackend : public QObject
{
    Q_OBJECT

public:
    explicit SamBackend(QObject *parent = nullptr);
    ~SamBackend() override;

    bool isRunning() const;

    // Low-level: send one JSON request (a newline is appended). No-op (and emits
    // errorOccurred) if the process isn't running.
    void sendRequest(const QJsonObject &request);

public slots:
    bool start();          // locate python + sam_service.py and launch (idempotent)
    void stop();           // ask the service to shut down, then ensure it's gone
    void ping();           // convenience: sendRequest({"command":"ping"})

    // Build the SAM2 predictor once in the resident process.
    void loadModel(const QString &checkpoint, const QString &config,
                   const QString &sam2Root, const QString &device);

    // Run SAM2 on an image with point prompts (labels: 1 = positive, 0 = negative).
    void segment(const QString &imagePath,
                 const QList<QPointF> &points, const QList<int> &labels);

    // Load `polygon` on frame `seedFrame` into SAM2's video memory (a conditioning
    // frame). `frames` is the whole ordered sequence. Memory accumulates across calls.
    void seedMemory(const QStringList &frames, int seedFrame, const QList<QPointF> &polygon);

    // Predict ONE frame from the current memory (on-demand, per frame).
    void predictFrame(int targetFrame);

    // Forget all tracking memory (start a fresh object/sequence).
    void resetTracking();

signals:
    void started();                                   // process is up
    void responseReceived(const QJsonObject &reply);  // one parsed JSON line
    void errorOccurred(const QString &message);
    void stopped();

private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onErrorOccurred(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    static QString             locatePython();
    static bool                interpreterWorks(const QString &interpreter);
    static QProcessEnvironment childEnvironment();

    QProcess   *m_process;
    QByteArray  m_stdoutBuffer;   // accumulates partial stdout until a full line
};
