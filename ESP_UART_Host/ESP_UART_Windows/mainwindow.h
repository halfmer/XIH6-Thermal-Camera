#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QLabel>
#include <QGroupBox>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QSpinBox>
#include <QTimer>
#include <QStackedWidget>
#include <QStatusBar>
#include <QNetworkInterface>
#include <QKeyEvent>

#include "frameparser.h"
#include "thermalwidget.h"
#include "colormap.h"
#include "serialworker.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // Serial
    void openSerialPort();
    void closeSerialPort();
    void sendData();
    void refreshPorts();
    void clearReceive();
    void clearSend();
    void toggleAutoSend(bool enabled);
    void saveLog();
    void onDtrToggled(bool checked);
    void onRtsToggled(bool checked);
    // Serial worker feedback (queued from the worker thread)
    void onSerialOpened(bool ok, const QString &error);
    void onSerialClosed();
    void onSerialBytes(const QByteArray &data);
    void onSerialPortError(const QString &message, bool fatal);

    // TCP / WiFi
    void toggleTcpServer();
    void onTcpNewConnection();
    void onTcpReadyRead();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError error);

    // Thermal
    void switchMode(int index);
    void onFrameReceived(const ThermalFrame &frame);
    void onParseError(const QString &msg);
    void saveScreenshot();
    void onSpotTempChanged(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setupUi();
    void setupSerialDebugPage(QWidget *page);
    void setupThermalPage(QWidget *page);
    void populateBaudRates();
    void connectSignals();
    void updateSerialStatus(bool opened);
    void updateTcpStatus(bool listening);
    QByteArray parseEscapes(const QString &text);
    void updateByteCounter();
    void feedThermalData(const QByteArray &data);
    void setSerialThermalStream(bool enabled);
    QString localIpAddresses() const;
    void resetSerialDiagLog();
    void appendSerialDiag(const QString &line);
    void writeSerialDiagFile();
    void logSerialChunk(const QByteArray &data);

    // Transport — serial (QSerialPort lives on m_serialThread; see serialworker.h)
    QThread      *m_serialThread = nullptr;
    SerialWorker *m_serialWorker = nullptr;
    bool          m_serialOpen = false;
    QString       m_serialPortName;
    qint32        m_serialBaud = 0;
    // Transport — TCP
    QTcpServer  *m_tcpServer = nullptr;
    QTcpSocket  *m_tcpClient = nullptr;

    // Frame parser (shared)
    FrameParser *m_frameParser = nullptr;

    // Mode switch
    QStackedWidget *m_modeStack = nullptr;
    QComboBox      *m_modeCombo = nullptr;

    // Settings groups (mutually visible)
    QGroupBox *m_serialSettingsGroup = nullptr;
    QGroupBox *m_networkSettingsGroup = nullptr;

    // ── Serial settings widgets ─────────────────────────
    QComboBox   *m_portCombo = nullptr;
    QComboBox   *m_baudRateCombo = nullptr;
    QComboBox   *m_dataBitsCombo = nullptr;
    QComboBox   *m_stopBitsCombo = nullptr;
    QComboBox   *m_parityCombo = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QCheckBox   *m_dtrCheck = nullptr;
    QCheckBox   *m_rtsCheck = nullptr;

    // ── Network settings widgets ────────────────────────
    QSpinBox    *m_tcpPortSpin = nullptr;
    QLabel      *m_tcpIpLabel = nullptr;
    QPushButton *m_tcpListenBtn = nullptr;
    QLabel      *m_tcpClientLabel = nullptr;

    // ── Serial debug page widgets ───────────────────────
    QTextEdit   *m_receiveEdit = nullptr;
    QCheckBox   *m_hexDisplayCheck = nullptr;
    QCheckBox   *m_autoScrollCheck = nullptr;
    QPushButton *m_saveLogBtn = nullptr;
    QPushButton *m_clearRecvBtn = nullptr;

    QTextEdit   *m_sendEdit = nullptr;
    QCheckBox   *m_hexSendCheck = nullptr;
    QCheckBox   *m_escapeCheck = nullptr;
    QComboBox   *m_lineEndingCombo = nullptr;
    QCheckBox   *m_autoSendCheck = nullptr;
    QSpinBox    *m_autoSendInterval = nullptr;
    QPushButton *m_clearSendBtn = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QTimer      *m_autoSendTimer = nullptr;

    // ── Thermal page widgets ────────────────────────────
    ThermalWidget *m_thermalWidget = nullptr;
    QComboBox     *m_colorMapCombo = nullptr;
    QPushButton   *m_screenshotBtn = nullptr;
    QLabel        *m_spotTempLabel = nullptr;
    QLabel        *m_thermalStatsLabel = nullptr;
    QLabel        *m_parseErrorLabel = nullptr;
    QPushButton   *m_clearThermalBtn = nullptr;
    qint64         m_thermalFrames = 0;
    qint64         m_thermalBadFrames = 0;

    // Buffers
    QByteArray m_debugBuf;
    QTimer    *m_debugFlushTimer = nullptr;
    QTimer    *m_serialDiagFlushTimer = nullptr;

    // Byte counters
    qint64 m_txBytes = 0;
    qint64 m_rxBytes = 0;
    QLabel *m_byteCounterLabel = nullptr;

    // Status
    QLabel *m_statusLabel = nullptr;

    // Rolling serial diagnostics, kept across close and cleared on next open.
    QByteArray m_serialDiagLog;      // bounded in-memory window
    QByteArray m_serialDiagPending;  // not-yet-flushed tail (appended to file)
    QString m_serialDiagPath;
    qint64 m_serialDiagDropped = 0;
    qint64 m_serialChunkCount = 0;
    qint64 m_lastFrameId = -1;
    bool m_serialDiagDirty = false;
    QElapsedTimer m_parseErrorShownAt;   // throttle error-label repaints
    QElapsedTimer m_byteCounterShownAt;  // throttle TX/RX status-bar repaints
};

#endif // MAINWINDOW_H
