#include "mainwindow.h"
#include <QMessageBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QFileDialog>
#include <QTextStream>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QFile>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tcpServer(new QTcpServer(this))
    , m_frameParser(new FrameParser(this))
    , m_autoSendTimer(new QTimer(this))
    , m_debugFlushTimer(new QTimer(this))
    , m_serialDiagFlushTimer(new QTimer(this))
{
    // Serial I/O runs on its own thread: at 2 Mbps the kernel buffers only a
    // few KB (tens of ms); if readAll() lived on the GUI thread, any paint /
    // log / dialog stall would silently drop bytes (Qt 6 has no Overrun error)
    // and corrupt every frame in flight — the "endless checksum mismatch".
    qRegisterMetaType<SerialOpenParams>("SerialOpenParams");
    m_serialThread = new QThread(this);
    m_serialWorker = new SerialWorker();          // no parent: lives on m_serialThread
    m_serialWorker->moveToThread(m_serialThread);
    connect(m_serialThread, &QThread::finished, m_serialWorker, &QObject::deleteLater);
    connect(m_serialWorker, &SerialWorker::opened, this, &MainWindow::onSerialOpened);
    connect(m_serialWorker, &SerialWorker::closed, this, &MainWindow::onSerialClosed);
    connect(m_serialWorker, &SerialWorker::bytesReceived, this, &MainWindow::onSerialBytes);
    connect(m_serialWorker, &SerialWorker::portError, this, &MainWindow::onSerialPortError);
    connect(m_serialWorker, &SerialWorker::txWritten, this, [this](qint64 n) {
        m_txBytes += n;
        updateByteCounter();
    });
    m_serialThread->start();

    setupUi();
    connectSignals();
    populateBaudRates();
    refreshPorts();
    setWindowTitle("UART 上位机 — 串口调试 & 热成像");
    resize(1000, 720);

    // Hex mode periodic flush
    m_debugFlushTimer->setInterval(80);
    m_debugFlushTimer->setSingleShot(true);
    connect(m_debugFlushTimer, &QTimer::timeout, this, [this]() {
        if (!m_debugBuf.isEmpty() && m_hexDisplayCheck->isChecked()) {
            m_receiveEdit->append(QString("<font color='#06c'>[%1 RX] %2</font>")
                .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                     QString::fromLatin1(m_debugBuf.toHex(' ').toUpper())));
            m_debugBuf.clear();
            if (m_autoScrollCheck->isChecked())
                m_receiveEdit->moveCursor(QTextCursor::End);
        }
    });

    m_serialDiagFlushTimer->setInterval(1000);   // appends are incremental now
    m_serialDiagFlushTimer->setSingleShot(true);
    connect(m_serialDiagFlushTimer, &QTimer::timeout, this, &MainWindow::writeSerialDiagFile);
}

MainWindow::~MainWindow()
{
    m_autoSendTimer->stop();
    if (m_serialDiagDirty)
        writeSerialDiagFile();
    if (m_tcpClient) { m_tcpClient->disconnectFromHost(); }
    m_tcpServer->close();
    if (m_serialOpen) {
        // Synchronous: let the worker send 'P' and close before the thread dies.
        QMetaObject::invokeMethod(m_serialWorker, "closePort",
                                  Qt::BlockingQueuedConnection, Q_ARG(bool, true));
    }
    m_serialThread->quit();
    m_serialThread->wait(2000);
}

// ═══════════════════════════════════════════════════════════════════════
// UI setup
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupUi()
{
    auto *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ── Top bar: mode switch + subtle logo ───────────────────
    auto *topBar = new QHBoxLayout();
    topBar->addWidget(new QLabel("工作模式:", this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({"串口调试", "热成像(串口)", "WiFi热成像"});
    m_modeCombo->setFixedWidth(140);
    topBar->addWidget(m_modeCombo);
    topBar->addStretch();

    auto *logoLabel = new QLabel(this);
    QPixmap logo(":/logo_dark.png");
    if (!logo.isNull()) {
        // Slim elongated banner, right-aligned, blends with dark bg
        logoLabel->setPixmap(logo.scaledToHeight(32, Qt::SmoothTransformation));
        logoLabel->setToolTip("嵌赛演示上位机");
    }
    logoLabel->setStyleSheet("background: transparent;");
    topBar->addWidget(logoLabel);
    mainLayout->addLayout(topBar);

    // ── Serial settings group (modes 0, 1) ────────────────────
    m_serialSettingsGroup = new QGroupBox("串口设置", this);
    auto *serGrid = new QGridLayout(m_serialSettingsGroup);
    serGrid->setSpacing(6);

    serGrid->addWidget(new QLabel("串口:", this), 0, 0);
    m_portCombo = new QComboBox(this);
    m_portCombo->setMinimumWidth(130);
    serGrid->addWidget(m_portCombo, 0, 1);
    m_refreshBtn = new QPushButton("刷新", this);
    m_refreshBtn->setObjectName("refreshBtn");
    m_refreshBtn->setFixedWidth(56);
    serGrid->addWidget(m_refreshBtn, 0, 2);
    serGrid->addWidget(new QLabel("波特率:", this), 0, 3);
    m_baudRateCombo = new QComboBox(this);
    m_baudRateCombo->setEditable(true);
    serGrid->addWidget(m_baudRateCombo, 0, 4);

    serGrid->addWidget(new QLabel("数据位:", this), 1, 0);
    m_dataBitsCombo = new QComboBox(this);
    m_dataBitsCombo->addItems({"8", "7", "6", "5"});
    m_dataBitsCombo->setCurrentIndex(0);
    serGrid->addWidget(m_dataBitsCombo, 1, 1);
    serGrid->addWidget(new QLabel("停止位:", this), 1, 2);
    m_stopBitsCombo = new QComboBox(this);
    m_stopBitsCombo->addItems({"1", "1.5", "2"});
    serGrid->addWidget(m_stopBitsCombo, 1, 3);
    serGrid->addWidget(new QLabel("校验位:", this), 1, 4);
    m_parityCombo = new QComboBox(this);
    m_parityCombo->addItems({"无", "奇校验", "偶校验"});
    serGrid->addWidget(m_parityCombo, 1, 5);

    m_dtrCheck = new QCheckBox("DTR", this);
    m_dtrCheck->setToolTip("Data Terminal Ready — 复位/EN 引脚");
    serGrid->addWidget(m_dtrCheck, 2, 0);
    m_rtsCheck = new QCheckBox("RTS", this);
    m_rtsCheck->setToolTip("Request To Send — BOOT0/IO0 引脚");
    serGrid->addWidget(m_rtsCheck, 2, 1);
    m_openBtn = new QPushButton("打开串口", this);
    m_openBtn->setObjectName("openBtn");
    m_openBtn->setFixedSize(100, 30);
    serGrid->addWidget(m_openBtn, 2, 5, Qt::AlignRight);

    mainLayout->addWidget(m_serialSettingsGroup);

    // ── Network settings group (mode 2) ───────────────────────
    m_networkSettingsGroup = new QGroupBox("WiFi 网络设置", this);
    auto *netGrid = new QGridLayout(m_networkSettingsGroup);
    netGrid->setSpacing(6);

    netGrid->addWidget(new QLabel("本机 IP:", this), 0, 0);
    m_tcpIpLabel = new QLabel(localIpAddresses(), this);
    m_tcpIpLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");
    netGrid->addWidget(m_tcpIpLabel, 0, 1, 1, 3);

    netGrid->addWidget(new QLabel("监听端口:", this), 1, 0);
    m_tcpPortSpin = new QSpinBox(this);
    m_tcpPortSpin->setRange(1024, 65535);
    m_tcpPortSpin->setValue(8888);
    m_tcpPortSpin->setFixedWidth(90);
    netGrid->addWidget(m_tcpPortSpin, 1, 1);
    m_tcpListenBtn = new QPushButton("开始监听", this);
    m_tcpListenBtn->setObjectName("tcpListenBtn");
    m_tcpListenBtn->setFixedSize(100, 30);
    netGrid->addWidget(m_tcpListenBtn, 1, 2);
    m_tcpClientLabel = new QLabel("等待 ESP32 连接...", this);
    m_tcpClientLabel->setStyleSheet("color: #888;");
    netGrid->addWidget(m_tcpClientLabel, 1, 3);

    mainLayout->addWidget(m_networkSettingsGroup);
    m_networkSettingsGroup->hide();

    // ── Stacked pages ─────────────────────────────────────────
    m_modeStack = new QStackedWidget(this);

    auto *debugPage = new QWidget();
    setupSerialDebugPage(debugPage);
    m_modeStack->addWidget(debugPage);   // index 0: debug

    auto *thermalPage = new QWidget();
    setupThermalPage(thermalPage);
    m_modeStack->addWidget(thermalPage); // index 1: thermal (serial + WiFi)

    mainLayout->addWidget(m_modeStack, 1);

    // ── Status bar ────────────────────────────────────────────
    m_statusLabel = new QLabel("串口已关闭", this);
    statusBar()->addWidget(m_statusLabel, 1);
    m_byteCounterLabel = new QLabel("TX: 0  |  RX: 0", this);
    statusBar()->addPermanentWidget(m_byteCounterLabel);
}

// ═══════════════════════════════════════════════════════════════════════
// Serial Debug page
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupSerialDebugPage(QWidget *page)
{
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(10);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *recvGroup = new QGroupBox("接收区", page);
    auto *recvLayout = new QVBoxLayout(recvGroup);
    auto *recvToolbar = new QHBoxLayout();
    m_hexDisplayCheck = new QCheckBox("Hex 显示", page);
    recvToolbar->addWidget(m_hexDisplayCheck);
    m_autoScrollCheck = new QCheckBox("自动滚动", page);
    m_autoScrollCheck->setChecked(true);
    recvToolbar->addWidget(m_autoScrollCheck);
    recvToolbar->addStretch();
    m_saveLogBtn = new QPushButton("保存日志", page);
    recvToolbar->addWidget(m_saveLogBtn);
    m_clearRecvBtn = new QPushButton("清空接收", page);
    recvToolbar->addWidget(m_clearRecvBtn);
    recvLayout->addLayout(recvToolbar);
    m_receiveEdit = new QTextEdit(page);
    m_receiveEdit->setReadOnly(true);
    m_receiveEdit->setFont(monoFont);
    recvLayout->addWidget(m_receiveEdit);
    layout->addWidget(recvGroup, 1);

    auto *sendGroup = new QGroupBox("发送区", page);
    auto *sendLayout = new QVBoxLayout(sendGroup);
    m_sendEdit = new QTextEdit(page);
    m_sendEdit->setFont(monoFont);
    m_sendEdit->setFixedHeight(80);
    sendLayout->addWidget(m_sendEdit);

    auto *sendToolbar = new QHBoxLayout();
    m_hexSendCheck = new QCheckBox("Hex 发送", page);
    sendToolbar->addWidget(m_hexSendCheck);
    m_escapeCheck = new QCheckBox("解析转义", page);
    m_escapeCheck->setToolTip("解析 \\r \\n \\t \\\\ \\xHH");
    sendToolbar->addWidget(m_escapeCheck);
    sendToolbar->addWidget(new QLabel("换行:", page));
    m_lineEndingCombo = new QComboBox(page);
    m_lineEndingCombo->addItems({"\\n", "\\r\\n", "\\r", "无"});
    m_lineEndingCombo->setFixedWidth(64);
    sendToolbar->addWidget(m_lineEndingCombo);
    sendToolbar->addSpacing(8);
    m_autoSendCheck = new QCheckBox("定时发送", page);
    sendToolbar->addWidget(m_autoSendCheck);
    m_autoSendInterval = new QSpinBox(page);
    m_autoSendInterval->setRange(100, 60000);
    m_autoSendInterval->setValue(1000);
    m_autoSendInterval->setSuffix(" ms");
    m_autoSendInterval->setFixedWidth(90);
    sendToolbar->addWidget(m_autoSendInterval);
    sendToolbar->addStretch();
    m_clearSendBtn = new QPushButton("清空发送", page);
    sendToolbar->addWidget(m_clearSendBtn);
    m_sendBtn = new QPushButton("发送", page);
    m_sendBtn->setFixedSize(72, 32);
    sendToolbar->addWidget(m_sendBtn);
    sendLayout->addLayout(sendToolbar);
    layout->addWidget(sendGroup);
}

// ═══════════════════════════════════════════════════════════════════════
// Thermal Imaging page (shared by serial and WiFi)
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupThermalPage(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *toolbar = new QHBoxLayout();
    toolbar->addWidget(new QLabel("色表:", page));
    m_colorMapCombo = new QComboBox(page);
    m_colorMapCombo->addItems(ColorMap::presetNames());
    m_colorMapCombo->setFixedWidth(160);
    toolbar->addWidget(m_colorMapCombo);
    toolbar->addSpacing(12);
    m_thermalStatsLabel = new QLabel("等待数据...", page);
    toolbar->addWidget(m_thermalStatsLabel);
    toolbar->addStretch();
    m_screenshotBtn = new QPushButton("截图 (Ctrl+S)", page);
    toolbar->addWidget(m_screenshotBtn);
    m_clearThermalBtn = new QPushButton("清空显示", page);
    toolbar->addWidget(m_clearThermalBtn);
    layout->addLayout(toolbar);

    m_parseErrorLabel = new QLabel(page);
    m_parseErrorLabel->setStyleSheet("color: #e07060; font-size: 10px;");
    m_parseErrorLabel->hide();
    layout->addWidget(m_parseErrorLabel);

    m_spotTempLabel = new QLabel("点击图像测量温度  |  F11 全屏  |  Ctrl+S 截图", page);
    m_spotTempLabel->setStyleSheet("color: #aaa; font-size: 10px; padding: 2px 6px; "
                                    "background: rgba(0,0,0,0.3);");
    layout->addWidget(m_spotTempLabel);

    m_thermalWidget = new ThermalWidget(page);
    layout->addWidget(m_thermalWidget, 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Signals
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::connectSignals()
{
    // Serial
    connect(m_openBtn, &QPushButton::clicked, this, &MainWindow::openSerialPort);
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::sendData);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_clearRecvBtn, &QPushButton::clicked, this, &MainWindow::clearReceive);
    connect(m_clearSendBtn, &QPushButton::clicked, this, &MainWindow::clearSend);
    connect(m_autoSendCheck, &QCheckBox::toggled, this, &MainWindow::toggleAutoSend);
    connect(m_autoSendTimer, &QTimer::timeout, this, &MainWindow::sendData);
    connect(m_saveLogBtn, &QPushButton::clicked, this, &MainWindow::saveLog);
    connect(m_dtrCheck, &QCheckBox::toggled, this, &MainWindow::onDtrToggled);
    connect(m_rtsCheck, &QCheckBox::toggled, this, &MainWindow::onRtsToggled);

    // TCP
    connect(m_tcpListenBtn, &QPushButton::clicked, this, &MainWindow::toggleTcpServer);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &MainWindow::onTcpNewConnection);

    // Mode
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::switchMode);

    // Thermal
    connect(m_colorMapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { m_thermalWidget->setColorMap(static_cast<ColorMap::Preset>(idx)); });
    connect(m_frameParser, &FrameParser::frameReady, this, &MainWindow::onFrameReceived);
    connect(m_frameParser, &FrameParser::parseError, this, &MainWindow::onParseError);
    connect(m_frameParser, &FrameParser::diagnostic, this,
            [this](const QString &msg) { appendSerialDiag("parser " + msg); });
    connect(m_screenshotBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);
    connect(m_thermalWidget, &ThermalWidget::spotTemperatureChanged,
            this, &MainWindow::onSpotTempChanged);
    connect(m_clearThermalBtn, &QPushButton::clicked, this, [this]() {
        m_thermalWidget->update();
        m_thermalFrames = 0;
        m_thermalBadFrames = 0;
        m_thermalStatsLabel->setText("等待数据...");
        m_parseErrorLabel->hide();
    });
}

void MainWindow::populateBaudRates()
{
    const QList<qint32> rates = {
        9600, 14400, 19200, 38400, 56000, 57600,
        115200, 128000, 230400, 256000, 460800, 921600,
        1000000, 1500000, 2000000, 3000000
    };
    for (qint32 r : rates)
        m_baudRateCombo->addItem(QString::number(r), r);
    // 1.5 Mbps: field-measured verdict (README_6 §7) — at 2 Mbps the CH340C's
    // ~32 B internal FIFO drops ~7 bytes/frame (syncDelta median -7). Firmware
    // LEPTON_STREAM_BAUD is 1500000 to match; keep both sides in sync.
    m_baudRateCombo->setCurrentText("1500000");
}

// ═══════════════════════════════════════════════════════════════════════
// Mode switching
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::switchMode(int index)
{
    m_frameParser->reset();
    m_debugBuf.clear();
    m_debugFlushTimer->stop();
    setSerialThermalStream(index == 1);

    if (index == 0) {
        // Serial debug mode
        m_modeStack->setCurrentIndex(0);
        m_serialSettingsGroup->show();
        m_networkSettingsGroup->hide();
        m_openBtn->setText(m_serialOpen ? "关闭串口" : "打开串口");
        m_statusLabel->setText(m_serialOpen
            ? QString("已连接 — %1 @ %2 bps").arg(m_serialPortName).arg(m_serialBaud)
            : "串口已关闭");
        m_statusLabel->setStyleSheet(m_serialOpen ? "color: #5cb85c; font-weight: bold;" : "");
    } else if (index == 1) {
        // Thermal serial mode
        m_modeStack->setCurrentIndex(1);
        m_serialSettingsGroup->show();
        m_networkSettingsGroup->hide();
        m_openBtn->setText(m_serialOpen ? "关闭串口" : "打开串口");
        m_thermalFrames = 0;
        m_thermalBadFrames = 0;
        m_thermalStatsLabel->setText("等待数据...");
        m_statusLabel->setText(m_serialOpen
            ? QString("热成像(串口) — %1 @ %2 bps").arg(m_serialPortName).arg(m_serialBaud)
            : "热成像(串口) — 串口已关闭");
        m_statusLabel->setStyleSheet(m_serialOpen ? "color: #5cb85c; font-weight: bold;" : "");
    } else {
        // WiFi thermal mode
        m_modeStack->setCurrentIndex(1);
        m_serialSettingsGroup->hide();
        m_networkSettingsGroup->show();
        m_thermalFrames = 0;
        m_thermalBadFrames = 0;
        m_thermalStatsLabel->setText("等待数据...");
        m_tcpIpLabel->setText(localIpAddresses());
        bool hasClient = m_tcpClient && m_tcpClient->state() == QAbstractSocket::ConnectedState;
        m_statusLabel->setText(hasClient
            ? QString("WiFi热成像 — ESP32 已连接 (%1)").arg(m_tcpClient->peerAddress().toString())
            : "WiFi热成像 — 等待 ESP32 连接");
        m_statusLabel->setStyleSheet(hasClient ? "color: #5cb85c; font-weight: bold;" : "");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Serial operations
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::refreshPorts()
{
    QString current = m_portCombo->currentText();
    m_portCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &info : ports) {
        QString desc = info.description();
        QString label = info.portName() + " — " + desc;
        QString tip;
        if (desc.contains("CH340", Qt::CaseInsensitive))
            tip = " [CH340 — STM32/ESP32]";
        else if (desc.contains("CP210", Qt::CaseInsensitive))
            tip = " [CP210x — STM32/ESP32]";
        else if (desc.contains("FT232", Qt::CaseInsensitive))
            tip = " [FT232 — STM32/ESP32]";
        if (!tip.isEmpty()) {
            label += tip;
            m_portCombo->setItemData(m_portCombo->count(), QColor("#5cb85c"), Qt::ForegroundRole);
        }
        m_portCombo->addItem(label, info.portName());
    }
    if (!current.isEmpty()) {
        int idx = m_portCombo->findText(current, Qt::MatchStartsWith);
        if (idx >= 0) m_portCombo->setCurrentIndex(idx);
    }
}

void MainWindow::openSerialPort()
{
    if (m_serialOpen) { closeSerialPort(); return; }
    if (m_portCombo->count() == 0) {
        QMessageBox::warning(this, "提示", "没有可用串口，请检查设备连接。");
        return;
    }

    SerialOpenParams params;
    params.portName = m_portCombo->currentData().toString();
    params.baudRate = m_baudRateCombo->currentText().toInt();
    params.dataBits = static_cast<QSerialPort::DataBits>(m_dataBitsCombo->currentText().toInt());

    const QString stopStr = m_stopBitsCombo->currentText();
    if (stopStr == "1.5")      params.stopBits = QSerialPort::OneAndHalfStop;
    else if (stopStr == "2")   params.stopBits = QSerialPort::TwoStop;
    else                       params.stopBits = QSerialPort::OneStop;

    const QString parityStr = m_parityCombo->currentText();
    if (parityStr == "奇校验")       params.parity = QSerialPort::OddParity;
    else if (parityStr == "偶校验")  params.parity = QSerialPort::EvenParity;
    else                            params.parity = QSerialPort::NoParity;

    params.dtr = m_dtrCheck->isChecked();
    params.rts = m_rtsCheck->isChecked();

    m_serialPortName = params.portName;
    m_serialBaud     = params.baudRate;

    QMetaObject::invokeMethod(m_serialWorker, "openPort",
                              Q_ARG(SerialOpenParams, params));
}

void MainWindow::onSerialOpened(bool ok, const QString &error)
{
    if (!ok) {
        QMessageBox::critical(this, "错误",
            "无法打开串口 " + m_serialPortName + ":\n" + error);
        return;
    }

    m_serialOpen = true;
    m_rxBytes = 0;   // per-session counters: totalRx must reconcile against
    m_txBytes = 0;   // [STRM] frame count x 38415 (README_6 §5)

    resetSerialDiagLog();
    appendSerialDiag(QString("open port=%1 baud=%2 dataBits=%3 stopBits=%4 parity=%5 mode=%6 dtr=%7 rts=%8 rxThread=worker")
                         .arg(m_serialPortName)
                         .arg(m_serialBaud)
                         .arg(m_dataBitsCombo->currentText())
                         .arg(m_stopBitsCombo->currentText())
                         .arg(m_parityCombo->currentText())
                         .arg(m_modeCombo->currentText())
                         .arg(m_dtrCheck->isChecked() ? 1 : 0)
                         .arg(m_rtsCheck->isChecked() ? 1 : 0));

    m_frameParser->reset();
    m_thermalFrames = 0;
    m_thermalBadFrames = 0;
    m_thermalStatsLabel->setText("等待数据...");
    setSerialThermalStream(m_modeCombo->currentIndex() == 1);

    updateSerialStatus(true);
}

void MainWindow::closeSerialPort()
{
    // Ask the worker to send 'P' (stop the MCU stream) and close; UI state is
    // updated when the worker reports closed().
    appendSerialDiag(QString("close_requested port=%1 rx=%2 tx=%3 frames=%4 bad=%5")
                         .arg(m_serialPortName)
                         .arg(m_rxBytes)
                         .arg(m_txBytes)
                         .arg(m_thermalFrames)
                         .arg(m_frameParser->badFrames()));

    m_autoSendTimer->stop();
    m_debugFlushTimer->stop();
    if (!m_debugBuf.isEmpty()) {
        if (m_hexDisplayCheck->isChecked())
            m_receiveEdit->append(QString("<font color='#06c'>[%1 RX] %2</font>")
                .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                     QString::fromLatin1(m_debugBuf.toHex(' ').toUpper())));
        else
            m_receiveEdit->append(QString("<font color='#06c'>[%1 RX] %2</font>")
                .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                     QString::fromUtf8(m_debugBuf)));
        m_debugBuf.clear();
    }

    QMetaObject::invokeMethod(m_serialWorker, "closePort", Q_ARG(bool, true));
}

void MainWindow::onSerialClosed()
{
    if (!m_serialOpen)
        return;
    m_serialOpen = false;
    appendSerialDiag("closed");
    writeSerialDiagFile();
    updateSerialStatus(false);
}

void MainWindow::onSerialBytes(const QByteArray &data)
{
    if (data.isEmpty()) return;
    m_rxBytes += data.size();
    updateByteCounter();
    logSerialChunk(data);

    if (m_modeCombo->currentIndex() >= 1) {
        feedThermalData(data);
    } else if (m_hexDisplayCheck->isChecked()) {
        m_debugBuf.append(data);
        if (!m_debugFlushTimer->isActive())   // restart-on-every-chunk starved the flush
            m_debugFlushTimer->start();
    } else {
        m_debugBuf.append(data);
        int start = 0;
        for (int i = 0; i < m_debugBuf.size(); ++i) {
            if (m_debugBuf.at(i) == '\n') {
                int end = i;
                if (end > start && m_debugBuf.at(end - 1) == '\r') end--;
                QByteArray line = m_debugBuf.mid(start, end - start);
                start = i + 1;
                m_receiveEdit->append(QString("<font color='#06c'>[%1 RX] %2</font>")
                    .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                         QString::fromUtf8(line)));
            }
        }
        m_debugBuf.remove(0, start);
        if (m_autoScrollCheck->isChecked())
            m_receiveEdit->moveCursor(QTextCursor::End);
    }
}

void MainWindow::onSerialPortError(const QString &message, bool fatal)
{
    // Report-only for transient errors; the worker closes the port itself on
    // fatal ones (device unplugged) and closed() will follow.
    appendSerialDiag(QString("port_error fatal=%1 %2").arg(fatal ? 1 : 0).arg(message));
    const QString text = QString("[%1] 串口%2: %3")
                             .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                                  fatal ? "已断开" : "警告", message);
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet("color: #d9534f;");
    if (m_modeCombo->currentIndex() == 0)
        m_receiveEdit->append(QString("<font color='red'>%1</font>").arg(text));
}

// ═══════════════════════════════════════════════════════════════════════
// TCP / WiFi Server
// ═══════════════════════════════════════════════════════════════════════

QString MainWindow::localIpAddresses() const
{
    QStringList ips;
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : ifaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            const auto entries = iface.addressEntries();
            for (const auto &entry : entries) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                    ips.append(entry.ip().toString());
            }
        }
    }
    return ips.isEmpty() ? "未检测到网络" : ips.join("  |  ");
}

void MainWindow::toggleTcpServer()
{
    if (m_tcpServer->isListening()) {
        // Stop
        if (m_tcpClient) { m_tcpClient->disconnectFromHost(); }
        m_tcpServer->close();
        updateTcpStatus(false);
        return;
    }

    quint16 port = static_cast<quint16>(m_tcpPortSpin->value());
    if (!m_tcpServer->listen(QHostAddress::Any, port)) {
        QMessageBox::critical(this, "错误",
            QString("无法在端口 %1 启动 TCP 监听:\n%2").arg(port).arg(m_tcpServer->errorString()));
        return;
    }

    updateTcpStatus(true);
}

void MainWindow::onTcpNewConnection()
{
    // Accept only one client at a time
    if (m_tcpClient) {
        QTcpSocket *rejected = m_tcpServer->nextPendingConnection();
        rejected->disconnectFromHost();
        rejected->deleteLater();
        return;
    }

    m_tcpClient = m_tcpServer->nextPendingConnection();
    m_frameParser->reset();
    m_thermalFrames = 0;
    m_thermalBadFrames = 0;
    m_rxBytes = 0;
    updateByteCounter();

    m_tcpClient->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(m_tcpClient, &QTcpSocket::readyRead, this, &MainWindow::onTcpReadyRead);
    connect(m_tcpClient, &QTcpSocket::disconnected, this, &MainWindow::onTcpDisconnected);
    connect(m_tcpClient, &QTcpSocket::errorOccurred, this, &MainWindow::onTcpError);

    m_tcpClientLabel->setText(QString("已连接: %1:%2")
        .arg(m_tcpClient->peerAddress().toString()).arg(m_tcpClient->peerPort()));
    m_tcpClientLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");

    m_statusLabel->setText(QString("WiFi热成像 — ESP32 已连接 (%1)")
        .arg(m_tcpClient->peerAddress().toString()));
    m_statusLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");

    // Stop accepting further connections
    m_tcpServer->pauseAccepting();
}

void MainWindow::onTcpReadyRead()
{
    if (!m_tcpClient) return;
    const QByteArray data = m_tcpClient->readAll();
    if (data.isEmpty()) return;
    m_rxBytes += data.size();
    updateByteCounter();

    if (m_modeCombo->currentIndex() == 2)
        feedThermalData(data);
}

void MainWindow::onTcpDisconnected()
{
    if (m_tcpClient) {
        m_tcpClient->deleteLater();
        m_tcpClient = nullptr;
    }

    m_frameParser->reset();
    m_tcpClientLabel->setText("已断开，等待重连...");
    m_tcpClientLabel->setStyleSheet("color: #d9534f;");
    m_statusLabel->setText("WiFi热成像 — 等待 ESP32 连接");
    m_statusLabel->setStyleSheet("");

    // Resume accepting
    if (m_tcpServer->isListening()) m_tcpServer->resumeAccepting();
}

void MainWindow::onTcpError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    if (m_tcpClient)
        m_statusLabel->setText(QString("WiFi热成像 — 错误: %1").arg(m_tcpClient->errorString()));
}

// ═══════════════════════════════════════════════════════════════════════
// Shared thermal data feed
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::feedThermalData(const QByteArray &data)
{
    m_frameParser->feed(data);
}

void MainWindow::setSerialThermalStream(bool enabled)
{
    if (!m_serialOpen)
        return;

    if (enabled) {
        appendSerialDiag("stream_cmd S");
        m_frameParser->reset();
        m_parseErrorLabel->hide();
    } else {
        appendSerialDiag("stream_cmd P");
    }
    // Input clearing + 'S'/'P' + drain all happen on the worker thread; no
    // waitForBytesWritten() on the GUI thread (its timeout used to surface as
    // TimeoutError and the old error handler closed the port on it).
    QMetaObject::invokeMethod(m_serialWorker, "setStream", Q_ARG(bool, enabled));
}

// ═══════════════════════════════════════════════════════════════════════
// Thermal frame handlers
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::onFrameReceived(const ThermalFrame &frame)
{
    m_thermalFrames++;
    const qint64 expectedNext = (m_lastFrameId < 0) ? frame.frameId : ((m_lastFrameId + 1) & 0xffff);
    appendSerialDiag(QString("frame_ok fid=%1 expectedNext=%2 gap=%3 payload=%4 size=%5x%6 totalFrames=%7 bad=%8")
                         .arg(frame.frameId)
                         .arg(expectedNext)
                         .arg((m_lastFrameId >= 0 && frame.frameId != expectedNext) ? 1 : 0)
                         .arg(frame.pixelData.size())
                         .arg(frame.width)
                         .arg(frame.height)
                         .arg(m_thermalFrames)
                         .arg(m_frameParser->badFrames()));
    m_lastFrameId = frame.frameId;

    const quint16 *raw = reinterpret_cast<const quint16 *>(frame.pixelData.constData());
    int pixelCount = frame.pixelData.size() / 2;

    QByteArray hostData;
    hostData.resize(frame.pixelData.size());
    quint16 *host = reinterpret_cast<quint16 *>(hostData.data());
    for (int i = 0; i < pixelCount; ++i) {
        const quint8 *src = reinterpret_cast<const quint8 *>(raw) + i * 2;
        host[i] = (static_cast<quint16>(src[0]) << 8) | src[1];
    }

    const bool shown = m_thermalWidget->displayFrame(host, frame.width, frame.height);
    if (!shown)
        appendSerialDiag(QString("frame_torn fid=%1 torn_total=%2")
                             .arg(frame.frameId)
                             .arg(m_thermalWidget->tornFrames()));

    m_thermalStatsLabel->setText(
        QString("帧: %1  |  错误帧: %2  |  撕裂拒显: %3  |  %4×%5  |  %6 FPS")
            .arg(m_thermalFrames).arg(m_frameParser->badFrames())
            .arg(m_thermalWidget->tornFrames())
            .arg(frame.width).arg(frame.height)
            .arg(m_thermalWidget->currentFps(), 0, 'f', 1));
    m_parseErrorLabel->hide();
}

void MainWindow::onParseError(const QString &msg)
{
    m_thermalBadFrames++;
    appendSerialDiag(QString("parse_error count=%1 msg=%2")
                         .arg(m_thermalBadFrames)
                         .arg(msg));
    // Repaint the label at most 4x/s — an error storm must not add GUI load
    // on top of an already struggling link.
    if (m_parseErrorShownAt.isValid() && m_parseErrorShownAt.elapsed() < 250)
        return;
    m_parseErrorShownAt.start();
    m_parseErrorLabel->setText("协议错误: " + msg);
    m_parseErrorLabel->show();
}

// ═══════════════════════════════════════════════════════════════════════
// Serial diagnostics
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::resetSerialDiagLog()
{
    m_serialDiagLog.clear();
    m_serialDiagPending.clear();
    m_serialDiagDropped = 0;
    m_serialChunkCount = 0;
    m_lastFrameId = -1;
    m_serialDiagPath = QCoreApplication::applicationDirPath() + "/serial_diag.log";
    QFile::remove(m_serialDiagPath);   // fresh session, fresh file
    appendSerialDiag(QString("diag_reset path=%1 memWindow=1MiB fileRotate=4MiB").arg(m_serialDiagPath));
}

void MainWindow::appendSerialDiag(const QString &line)
{
    static constexpr int MaxDiagBytes = 1024 * 1024;

    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const QByteArray encoded = QString("[%1] %2\n").arg(stamp, line).toUtf8();
    m_serialDiagLog.append(encoded);
    m_serialDiagPending.append(encoded);

    // Amortized trim: once past the cap, cut the window in HALF in one move
    // instead of shifting a ~1 MiB buffer twice per appended line (the old
    // per-line remove+prepend was two big memmoves per rx chunk — a GUI-thread
    // stall generator precisely when the link was already struggling).
    if (m_serialDiagLog.size() > MaxDiagBytes) {
        int cut = m_serialDiagLog.size() / 2;
        const int nl = m_serialDiagLog.indexOf('\n', cut);
        if (nl >= 0)
            cut = nl + 1;
        m_serialDiagLog.remove(0, cut);
        m_serialDiagDropped += cut;
        m_serialDiagLog.append(
            QString("[serial_diag] trimmed_old_bytes_total=%1\n").arg(m_serialDiagDropped).toUtf8());
    }

    m_serialDiagDirty = true;
    if (!m_serialDiagFlushTimer->isActive())
        m_serialDiagFlushTimer->start();
}

void MainWindow::writeSerialDiagFile()
{
    if (m_serialDiagPath.isEmpty() || m_serialDiagPending.isEmpty()) {
        m_serialDiagDirty = false;
        return;
    }

    // Append only the new tail; the old code rewrote the whole 1 MiB buffer
    // (Truncate) every 250 ms — a steady 4 MB/s disk write on the GUI thread.
    QFile file(m_serialDiagPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    if (file.size() > 4 * 1024 * 1024) {          // rotate: keep the memory window
        file.close();
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        file.write(m_serialDiagLog);
    } else {
        file.write(m_serialDiagPending);
    }
    m_serialDiagPending.clear();
    m_serialDiagDirty = false;
}

void MainWindow::logSerialChunk(const QByteArray &data)
{
    m_serialChunkCount++;

    // Sampled: chunk logging at 2 Mbps fires dozens of times per second; hex
    // formatting every chunk is pure GUI-thread overhead. Keep the first 32
    // (open/start-of-stream forensics) then 1 in 32.
    if (m_serialChunkCount > 32 && (m_serialChunkCount & 31) != 0)
        return;

    int syncCount = 0;
    for (int i = 0; i + 1 < data.size(); ++i) {
        if (static_cast<quint8>(data.at(i)) == 0xaa &&
            static_cast<quint8>(data.at(i + 1)) == 0x55) {
            syncCount++;
        }
    }

    appendSerialDiag(QString("rx_chunk idx=%1 len=%2 totalRx=%3 syncAA55=%4 head=%5 tail=%6")
                         .arg(m_serialChunkCount)
                         .arg(data.size())
                         .arg(m_rxBytes)
                         .arg(syncCount)
                         .arg(QString::fromLatin1(data.left(24).toHex(' ')))
                         .arg(QString::fromLatin1(data.right(16).toHex(' '))));
}

// ═══════════════════════════════════════════════════════════════════════
// Serial Debug send / save / DTR-RTS
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::sendData()
{
    if (!m_serialOpen) {
        QMessageBox::warning(this, "提示", "请先打开串口。");
        m_autoSendTimer->stop();
        m_autoSendCheck->setChecked(false);
        return;
    }
    if (m_modeCombo->currentIndex() == 1) {
        QMessageBox::information(this, "提示",
            "热成像(串口)模式下 UART4 是二进制视频流专用通道。\n"
            "请切回“串口调试”模式再发送普通文本。");
        return;
    }

    QByteArray data;
    const QString text = m_sendEdit->toPlainText();

    if (m_hexSendCheck->isChecked()) {
        QString hex = text;
        hex.remove(QRegularExpression("[\\s,;]"));
        data = QByteArray::fromHex(hex.toUtf8());
        if (data.isEmpty() && !hex.isEmpty()) {
            QMessageBox::warning(this, "提示", "Hex 格式不正确。");
            return;
        }
    } else if (m_escapeCheck->isChecked()) {
        data = parseEscapes(text);
    } else {
        data = text.toUtf8();
    }

    const QString le = m_lineEndingCombo->currentText();
    if (le == "\\n")       data.append('\n');
    else if (le == "\\r\\n") data.append("\r\n");
    else if (le == "\\r")   data.append('\r');
    if (data.isEmpty()) return;

    QMetaObject::invokeMethod(m_serialWorker, "writeBytes", Q_ARG(QByteArray, data));

    QString echo = m_hexDisplayCheck->isChecked() ? data.toHex(' ').toUpper() : QString::fromUtf8(data);
    QDateTime now = QDateTime::currentDateTime();
    m_receiveEdit->append(QString("<font color='#888'>[%1 TX] %2</font>")
                              .arg(now.toString("HH:mm:ss.zzz"), echo));
    if (m_autoScrollCheck->isChecked())
        m_receiveEdit->moveCursor(QTextCursor::End);
}

void MainWindow::clearReceive() { m_receiveEdit->clear(); m_debugBuf.clear(); }
void MainWindow::clearSend()    { m_sendEdit->clear(); }

void MainWindow::toggleAutoSend(bool enabled)
{
    if (enabled) {
        if (!m_serialOpen) {
            QMessageBox::warning(this, "提示", "请先打开串口再启用定时发送。");
            m_autoSendCheck->setChecked(false);
            return;
        }
        m_autoSendTimer->start(m_autoSendInterval->value());
    } else {
        m_autoSendTimer->stop();
    }
}

void MainWindow::saveLog()
{
    QString filePath = QFileDialog::getSaveFileName(
        this, "保存接收日志", "uart_log.txt", "文本文件 (*.txt *.log);;所有文件 (*)");
    if (filePath.isEmpty()) return;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "错误", "无法写入文件:\n" + file.errorString());
        return;
    }
    QTextStream stream(&file);
    stream << m_receiveEdit->toPlainText();
    file.close();
    QDateTime now = QDateTime::currentDateTime();
    m_receiveEdit->append(QString("<font color='#888'>[%1 ---] 日志已保存至 %2</font>")
                              .arg(now.toString("HH:mm:ss.zzz"), filePath));
}

void MainWindow::onDtrToggled(bool checked)
{
    QMetaObject::invokeMethod(m_serialWorker, "setDtr", Q_ARG(bool, checked));
}

void MainWindow::onRtsToggled(bool checked)
{
    QMetaObject::invokeMethod(m_serialWorker, "setRts", Q_ARG(bool, checked));
}

// ═══════════════════════════════════════════════════════════════════════
// Status updates
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::updateSerialStatus(bool opened)
{
    if (opened) {
        m_openBtn->setText("关闭串口");
        m_openBtn->setStyleSheet("background-color: #d9534f; color: white;");
        int mode = m_modeCombo->currentIndex();
        m_statusLabel->setText(mode == 1
            ? QString("热成像(串口) — %1 @ %2 bps").arg(m_serialPortName).arg(m_serialBaud)
            : QString("已连接 — %1 @ %2 bps").arg(m_serialPortName).arg(m_serialBaud));
        m_statusLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");
        m_portCombo->setEnabled(false);  m_baudRateCombo->setEnabled(false);
        m_dataBitsCombo->setEnabled(false); m_stopBitsCombo->setEnabled(false);
        m_parityCombo->setEnabled(false);   m_refreshBtn->setEnabled(false);
        m_sendBtn->setEnabled(true);
        m_dtrCheck->setEnabled(true);       m_rtsCheck->setEnabled(true);
    } else {
        m_autoSendTimer->stop();
        m_autoSendCheck->setChecked(false);
        m_openBtn->setText("打开串口");
        m_openBtn->setStyleSheet("");
        int mode = m_modeCombo->currentIndex();
        m_statusLabel->setText(mode == 1 ? "热成像(串口) — 串口已关闭" : "串口已关闭");
        m_statusLabel->setStyleSheet("");
        m_portCombo->setEnabled(true);      m_baudRateCombo->setEnabled(true);
        m_dataBitsCombo->setEnabled(true);  m_stopBitsCombo->setEnabled(true);
        m_parityCombo->setEnabled(true);    m_refreshBtn->setEnabled(true);
        m_sendBtn->setEnabled(false);
        m_dtrCheck->setEnabled(false);      m_rtsCheck->setEnabled(false);
    }
}

void MainWindow::updateTcpStatus(bool listening)
{
    if (listening) {
        m_tcpListenBtn->setText("停止监听");
        m_tcpListenBtn->setStyleSheet("background-color: #d9534f; color: white;");
        m_tcpPortSpin->setEnabled(false);
        m_tcpClientLabel->setText("等待 ESP32 连接...");
        m_tcpClientLabel->setStyleSheet("color: #888;");
        m_statusLabel->setText("WiFi热成像 — 等待 ESP32 连接");
        m_statusLabel->setStyleSheet("");
    } else {
        m_tcpListenBtn->setText("开始监听");
        m_tcpListenBtn->setStyleSheet("");
        m_tcpPortSpin->setEnabled(true);
        m_tcpClientLabel->setText("等待 ESP32 连接...");
        m_tcpClientLabel->setStyleSheet("color: #888;");
        m_statusLabel->setText("WiFi热成像 — 未监听");
        m_statusLabel->setStyleSheet("");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Screenshot & fullscreen
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::saveScreenshot()
{
    if (m_modeCombo->currentIndex() == 0) return;  // not in thermal mode

    QString defaultName = QString("thermal_%1.png")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString filePath = QFileDialog::getSaveFileName(
        this, "保存热成像截图", defaultName, "PNG 图片 (*.png);;所有文件 (*)");
    if (filePath.isEmpty()) return;

    if (m_thermalWidget->saveScreenshot(filePath)) {
        m_statusLabel->setText(QString("截图已保存: %1").arg(filePath));
        m_statusLabel->setStyleSheet("color: #5cb85c;");
        // Reset after 3 seconds
        QTimer::singleShot(3000, this, [this]() {
            if (m_statusLabel->text().startsWith("截图已保存"))
                switchMode(m_modeCombo->currentIndex());
        });
    } else {
        QMessageBox::warning(this, "错误", "截图保存失败。");
    }
}

void MainWindow::onSpotTempChanged(const QString &text)
{
    m_spotTempLabel->setText(text.isEmpty()
        ? "点击图像测量温度  |  F11 全屏  |  Ctrl+S 截图"
        : text);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F11) {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
        return;
    }
    if (event->key() == Qt::Key_S && event->modifiers() == Qt::ControlModifier) {
        saveScreenshot();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

QByteArray MainWindow::parseEscapes(const QString &text)
{
    QByteArray result;
    result.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\') && i + 1 < text.size()) {
            const QChar next = text.at(i + 1);
            switch (next.toLatin1()) {
            case 'r':  result.append('\r'); i++; break;
            case 'n':  result.append('\n'); i++; break;
            case 't':  result.append('\t'); i++; break;
            case '\\': result.append('\\'); i++; break;
            case 'x': case 'X':
                if (i + 3 < text.size()) {
                    QByteArray hex; hex.append(text.at(i+2).toLatin1()); hex.append(text.at(i+3).toLatin1());
                    bool ok; quint8 byte = hex.toUInt(&ok, 16);
                    if (ok) { result.append(static_cast<char>(byte)); i += 3; }
                    else result.append('\\');
                } else result.append('\\');
                break;
            default: result.append('\\'); break;
            }
        } else {
            result.append(c.toLatin1());
        }
    }
    return result;
}

void MainWindow::updateByteCounter()
{
    // Status-bar label repaint at most 10x/s; the counters themselves stay exact.
    if (m_byteCounterShownAt.isValid() && m_byteCounterShownAt.elapsed() < 100)
        return;
    m_byteCounterShownAt.start();
    m_byteCounterLabel->setText(QString("TX: %1  |  RX: %2").arg(m_txBytes).arg(m_rxBytes));
}
