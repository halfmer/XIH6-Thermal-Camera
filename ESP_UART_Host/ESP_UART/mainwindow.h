#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
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

#include "frameparser.h"
#include "thermalwidget.h"
#include "colormap.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void openSerialPort();
    void closeSerialPort();
    void sendData();
    void readData();
    void refreshPorts();
    void clearReceive();
    void clearSend();
    void handleSerialError(QSerialPort::SerialPortError error);

    // Serial debug slots
    void toggleAutoSend(bool enabled);
    void saveLog();
    void onDtrToggled(bool checked);
    void onRtsToggled(bool checked);

    // Thermal mode slots
    void switchMode(int index);
    void onFrameReceived(const ThermalFrame &frame);
    void onParseError(const QString &msg);
    void sendTransportToggleCommand();

private:
    void setupUi();
    void setupSerialDebugPage(QWidget *page);
    void setupThermalPage(QWidget *page);
    void populateBaudRates();
    void connectSignals();
    void updatePortStatus(bool opened);
    QByteArray parseEscapes(const QString &text);
    void updateByteCounter();
    void handleSerialStatusLine(const QByteArray &line);

    // Serial port
    QSerialPort *m_serialPort = nullptr;
    FrameParser  *m_frameParser = nullptr;

    // Mode switch
    QStackedWidget *m_modeStack = nullptr;
    QComboBox      *m_modeCombo = nullptr;

    // Port settings (shared)
    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudRateCombo = nullptr;
    QComboBox *m_dataBitsCombo = nullptr;
    QComboBox *m_stopBitsCombo = nullptr;
    QComboBox *m_parityCombo = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QCheckBox *m_dtrCheck = nullptr;
    QCheckBox *m_rtsCheck = nullptr;

    // ── Serial debug page widgets ────────────────────────
    QTextEdit *m_receiveEdit = nullptr;
    QCheckBox *m_hexDisplayCheck = nullptr;
    QCheckBox *m_autoScrollCheck = nullptr;
    QPushButton *m_saveLogBtn = nullptr;
    QPushButton *m_clearRecvBtn = nullptr;

    QTextEdit *m_sendEdit = nullptr;
    QCheckBox *m_hexSendCheck = nullptr;
    QCheckBox *m_escapeCheck = nullptr;
    QComboBox *m_lineEndingCombo = nullptr;
    QCheckBox *m_autoSendCheck = nullptr;
    QSpinBox *m_autoSendInterval = nullptr;
    QPushButton *m_clearSendBtn = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QTimer *m_autoSendTimer = nullptr;

    // ── Thermal page widgets ─────────────────────────────
    ThermalWidget *m_thermalWidget = nullptr;
    QComboBox     *m_colorMapCombo = nullptr;
    QLabel        *m_thermalStatsLabel = nullptr;
    QLabel        *m_parseErrorLabel = nullptr;
    QPushButton   *m_switchTransportBtn = nullptr;
    QPushButton   *m_clearThermalBtn = nullptr;
    qint64         m_thermalFrames = 0;
    qint64         m_thermalBadFrames = 0;
    QByteArray     m_thermalBuf;  // raw data buffer for thermal mode

    // Byte counters
    qint64 m_txBytes = 0;
    qint64 m_rxBytes = 0;
    QLabel *m_byteCounterLabel = nullptr;

    // Status
    QLabel *m_statusLabel = nullptr;
};

#endif // MAINWINDOW_H
