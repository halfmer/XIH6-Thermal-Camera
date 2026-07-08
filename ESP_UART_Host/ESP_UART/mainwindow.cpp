#include "mainwindow.h"
#include <QMessageBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QFileDialog>
#include <QTextStream>
#include <QRegularExpression>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_serialPort(new QSerialPort(this))
    , m_frameParser(new FrameParser(this))
    , m_autoSendTimer(new QTimer(this))
{
    setupUi();
    connectSignals();
    populateBaudRates();
    refreshPorts();
    setWindowTitle("ESP UART — 串口调试 & 热成像");
    resize(800, 650);
}

MainWindow::~MainWindow()
{
    m_autoSendTimer->stop();
    if (m_serialPort->isOpen())
        m_serialPort->close();
}

// ═══════════════════════════════════════════════════════════════════════
// Top-level UI — mode switch + stacked pages
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupUi()
{
    auto *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ── Toolbar: mode switch ─────────────────────────────────
    auto *topBar = new QHBoxLayout();
    topBar->addWidget(new QLabel("工作模式:", this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({"串口调试", "热成像"});
    m_modeCombo->setFixedWidth(120);
    topBar->addWidget(m_modeCombo);
    topBar->addStretch();
    mainLayout->addLayout(topBar);

    // ── Shared port settings ─────────────────────────────────
    auto *settingsGroup = new QGroupBox("串口设置", this);
    auto *settingsGrid = new QGridLayout(settingsGroup);
    settingsGrid->setSpacing(6);
    settingsGrid->setColumnStretch(6, 1);

    // Row 0
    settingsGrid->addWidget(new QLabel("串口:", this), 0, 0);
    m_portCombo = new QComboBox(this);
    m_portCombo->setMinimumWidth(130);
    settingsGrid->addWidget(m_portCombo, 0, 1);
    m_refreshBtn = new QPushButton("刷新", this);
    m_refreshBtn->setFixedWidth(56);
    settingsGrid->addWidget(m_refreshBtn, 0, 2);
    settingsGrid->addWidget(new QLabel("波特率:", this), 0, 3);
    m_baudRateCombo = new QComboBox(this);
    m_baudRateCombo->setEditable(true);
    settingsGrid->addWidget(m_baudRateCombo, 0, 4);

    // Row 1
    settingsGrid->addWidget(new QLabel("数据位:", this), 1, 0);
    m_dataBitsCombo = new QComboBox(this);
    m_dataBitsCombo->addItems({"8", "7", "6", "5"});
    m_dataBitsCombo->setCurrentIndex(0);
    settingsGrid->addWidget(m_dataBitsCombo, 1, 1);
    settingsGrid->addWidget(new QLabel("停止位:", this), 1, 2);
    m_stopBitsCombo = new QComboBox(this);
    m_stopBitsCombo->addItems({"1", "1.5", "2"});
    settingsGrid->addWidget(m_stopBitsCombo, 1, 3);
    settingsGrid->addWidget(new QLabel("校验位:", this), 1, 4);
    m_parityCombo = new QComboBox(this);
    m_parityCombo->addItems({"无", "奇校验", "偶校验"});
    settingsGrid->addWidget(m_parityCombo, 1, 5);

    // Row 2 — DTR / RTS / Open
    m_dtrCheck = new QCheckBox("DTR", this);
    m_dtrCheck->setToolTip("Data Terminal Ready — ESP32 EN 引脚");
    settingsGrid->addWidget(m_dtrCheck, 2, 0);
    m_rtsCheck = new QCheckBox("RTS", this);
    m_rtsCheck->setToolTip("Request To Send — ESP32 IO0 引脚");
    settingsGrid->addWidget(m_rtsCheck, 2, 1);
    m_openBtn = new QPushButton("打开串口", this);
    m_openBtn->setFixedSize(100, 30);
    settingsGrid->addWidget(m_openBtn, 2, 5, Qt::AlignRight);

    mainLayout->addWidget(settingsGroup);

    // ── Stacked pages ────────────────────────────────────────
    m_modeStack = new QStackedWidget(this);

    auto *debugPage = new QWidget();
    setupSerialDebugPage(debugPage);
    m_modeStack->addWidget(debugPage);  // index 0

    auto *thermalPage = new QWidget();
    setupThermalPage(thermalPage);
    m_modeStack->addWidget(thermalPage);  // index 1

    mainLayout->addWidget(m_modeStack, 1);

    // ── Status bar ───────────────────────────────────────────
    m_statusLabel = new QLabel("串口已关闭", this);
    statusBar()->addWidget(m_statusLabel, 1);
    m_byteCounterLabel = new QLabel("TX: 0  |  RX: 0", this);
    statusBar()->addPermanentWidget(m_byteCounterLabel);
}

// ═══════════════════════════════════════════════════════════════════════
// Serial Debug page (index 0)
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupSerialDebugPage(QWidget *page)
{
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(10);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Recv
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

    // Send
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
// Thermal Imaging page (index 1)
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::setupThermalPage(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Toolbar
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
    m_switchTransportBtn = new QPushButton("切换传输模式", page);
    m_switchTransportBtn->setToolTip("发送 MODE_TOGGLE 给 STM32");
    toolbar->addWidget(m_switchTransportBtn);
    m_clearThermalBtn = new QPushButton("清空显示", page);
    toolbar->addWidget(m_clearThermalBtn);
    layout->addLayout(toolbar);

    // Parse error indicator
    m_parseErrorLabel = new QLabel(page);
    m_parseErrorLabel->setStyleSheet("color: #e07060; font-size: 10px;");
    m_parseErrorLabel->hide();
    layout->addWidget(m_parseErrorLabel);

    // Thermal display widget
    m_thermalWidget = new ThermalWidget(page);
    layout->addWidget(m_thermalWidget, 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Signals
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::connectSignals()
{
    connect(m_openBtn, &QPushButton::clicked, this, &MainWindow::openSerialPort);
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::sendData);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_clearRecvBtn, &QPushButton::clicked, this, &MainWindow::clearReceive);
    connect(m_clearSendBtn, &QPushButton::clicked, this, &MainWindow::clearSend);
    connect(m_serialPort, &QSerialPort::readyRead, this, &MainWindow::readData);
    connect(m_serialPort, &QSerialPort::errorOccurred, this, &MainWindow::handleSerialError);
    connect(m_autoSendCheck, &QCheckBox::toggled, this, &MainWindow::toggleAutoSend);
    connect(m_autoSendTimer, &QTimer::timeout, this, &MainWindow::sendData);
    connect(m_saveLogBtn, &QPushButton::clicked, this, &MainWindow::saveLog);
    connect(m_dtrCheck, &QCheckBox::toggled, this, &MainWindow::onDtrToggled);
    connect(m_rtsCheck, &QCheckBox::toggled, this, &MainWindow::onRtsToggled);

    // Mode switching
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::switchMode);

    // Thermal
    connect(m_colorMapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { m_thermalWidget->setColorMap(static_cast<ColorMap::Preset>(idx)); });
    connect(m_frameParser, &FrameParser::frameReady, this, &MainWindow::onFrameReceived);
    connect(m_frameParser, &FrameParser::parseError, this, &MainWindow::onParseError);
    connect(m_switchTransportBtn, &QPushButton::clicked,
            this, &MainWindow::sendTransportToggleCommand);
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
    m_baudRateCombo->setCurrentText("1500000");
}

// ═══════════════════════════════════════════════════════════════════════
// Mode switching
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::switchMode(int index)
{
    m_modeStack->setCurrentIndex(index);
    m_frameParser->reset();
    m_thermalBuf.clear();

    if (index == 1) {
        // Thermal mode
        m_statusLabel->setText(m_serialPort->isOpen()
            ? QString("热成像模式 — %1 @ %2 bps").arg(m_serialPort->portName()).arg(m_serialPort->baudRate())
            : "热成像模式 — 串口已关闭");
    } else {
        // Serial debug mode
        m_statusLabel->setText(m_serialPort->isOpen()
            ? QString("已连接 — %1 @ %2 bps").arg(m_serialPort->portName()).arg(m_serialPort->baudRate())
            : "串口已关闭");
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
    for (const auto &info : ports)
        m_portCombo->addItem(info.portName() + " — " + info.description(), info.portName());

    if (!current.isEmpty()) {
        int idx = m_portCombo->findText(current, Qt::MatchStartsWith);
        if (idx >= 0) m_portCombo->setCurrentIndex(idx);
    }
}

void MainWindow::openSerialPort()
{
    if (m_serialPort->isOpen()) {
        closeSerialPort();
        return;
    }

    if (m_portCombo->count() == 0) {
        QMessageBox::warning(this, "提示", "没有可用串口，请检查设备连接。");
        return;
    }

    const QString portName = m_portCombo->currentData().toString();
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(m_baudRateCombo->currentText().toInt());
    m_serialPort->setDataBits(static_cast<QSerialPort::DataBits>(m_dataBitsCombo->currentText().toInt()));

    const QString stopStr = m_stopBitsCombo->currentText();
    if (stopStr == "1.5")      m_serialPort->setStopBits(QSerialPort::OneAndHalfStop);
    else if (stopStr == "2")   m_serialPort->setStopBits(QSerialPort::TwoStop);
    else                       m_serialPort->setStopBits(QSerialPort::OneStop);

    const QString parityStr = m_parityCombo->currentText();
    if (parityStr == "奇校验")       m_serialPort->setParity(QSerialPort::OddParity);
    else if (parityStr == "偶校验")  m_serialPort->setParity(QSerialPort::EvenParity);
    else                            m_serialPort->setParity(QSerialPort::NoParity);

    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this, "错误", "无法打开串口 " + portName + ":\n" + m_serialPort->errorString());
        return;
    }

    m_serialPort->setDataTerminalReady(m_dtrCheck->isChecked());
    m_serialPort->setRequestToSend(m_rtsCheck->isChecked());

    m_frameParser->reset();
    m_thermalBuf.clear();
    m_thermalFrames = 0;
    m_thermalBadFrames = 0;

    updatePortStatus(true);
}

void MainWindow::closeSerialPort()
{
    m_autoSendTimer->stop();
    if (m_serialPort->isOpen())
        m_serialPort->close();
    updatePortStatus(false);
}

// ── Read data — dual-mode dispatch ────────────────────────────────────

void MainWindow::readData()
{
    const QByteArray data = m_serialPort->readAll();
    if (data.isEmpty()) return;

    if (m_modeStack->currentIndex() == 1) {
        // Thermal mode — feed to frame parser
        m_frameParser->feed(data);
        // Also count bytes
        m_rxBytes += data.size();
        updateByteCounter();
    } else {
        // Serial debug mode — display as text
        m_rxBytes += data.size();
        updateByteCounter();

        QDateTime now = QDateTime::currentDateTime();
        const QList<QByteArray> lines = data.split('\n');
        for (QByteArray line : lines) {
            if (line.endsWith('\r'))
                line.chop(1);
            handleSerialStatusLine(line);
        }

        QString display = m_hexDisplayCheck->isChecked()
                              ? data.toHex(' ').toUpper()
                              : QString::fromUtf8(data);

        m_receiveEdit->append(QString("<font color='#06c'>[%1 RX] %2</font>")
                                  .arg(now.toString("HH:mm:ss.zzz"), display));
        if (m_autoScrollCheck->isChecked())
            m_receiveEdit->moveCursor(QTextCursor::End);
    }
}

void MainWindow::sendTransportToggleCommand()
{
    if (!m_serialPort->isOpen()) {
        QMessageBox::warning(this, "提示", "请先打开 STM32 控制串口。");
        return;
    }

    const QByteArray cmd("MODE_TOGGLE\n");
    const qint64 written = m_serialPort->write(cmd);
    if (written < 0) {
        QMessageBox::critical(this, "错误", "发送失败: " + m_serialPort->errorString());
        return;
    }

    m_txBytes += written;
    updateByteCounter();
    m_statusLabel->setText("已发送传输模式切换命令，等待 STM32 返回");
    m_statusLabel->setStyleSheet("color: #f0ad4e; font-weight: bold;");
}

// ── Thermal frame handler ─────────────────────────────────────────────

void MainWindow::onFrameReceived(const ThermalFrame &frame)
{
    m_thermalFrames++;
    const quint16 *raw = reinterpret_cast<const quint16 *>(frame.pixelData.constData());
    int pixelCount = frame.pixelData.size() / 2;

    // Convert big-endian to host
    QByteArray hostData;
    hostData.resize(frame.pixelData.size());
    quint16 *host = reinterpret_cast<quint16 *>(hostData.data());
    for (int i = 0; i < pixelCount; ++i) {
        // Read BE from raw buffer
        const quint8 *src = reinterpret_cast<const quint8 *>(raw) + i * 2;
        host[i] = (static_cast<quint16>(src[0]) << 8) | src[1];
    }

    m_thermalWidget->displayFrame(host, frame.width, frame.height);

    m_thermalStatsLabel->setText(
        QString("帧: %1  |  错误帧: %2  |  %3×%4  |  %5 FPS")
            .arg(m_thermalFrames).arg(m_frameParser->badFrames())
            .arg(frame.width).arg(frame.height)
            .arg(m_thermalWidget->currentFps(), 0, 'f', 1));

    m_parseErrorLabel->hide();
}

void MainWindow::onParseError(const QString &msg)
{
    m_parseErrorLabel->setText("协议错误: " + msg);
    m_parseErrorLabel->show();
    m_thermalBadFrames++;
}

// ═══════════════════════════════════════════════════════════════════════
// Serial Debug — send / save / timer / DTR-RTS (unchanged)
// ═══════════════════════════════════════════════════════════════════════

void MainWindow::sendData()
{
    if (!m_serialPort->isOpen()) {
        QMessageBox::warning(this, "提示", "请先打开串口。");
        m_autoSendTimer->stop();
        m_autoSendCheck->setChecked(false);
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

    qint64 written = m_serialPort->write(data);
    if (written < 0) {
        QMessageBox::critical(this, "错误", "发送失败: " + m_serialPort->errorString());
        return;
    }

    m_txBytes += written;
    updateByteCounter();

    QString echo = m_hexDisplayCheck->isChecked() ? data.toHex(' ').toUpper() : QString::fromUtf8(data);
    QDateTime now = QDateTime::currentDateTime();
    m_receiveEdit->append(QString("<font color='#888'>[%1 TX] %2</font>")
                              .arg(now.toString("HH:mm:ss.zzz"), echo));
    if (m_autoScrollCheck->isChecked())
        m_receiveEdit->moveCursor(QTextCursor::End);
}

void MainWindow::clearReceive() { m_receiveEdit->clear(); }
void MainWindow::clearSend()    { m_sendEdit->clear(); }

void MainWindow::handleSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || error == QSerialPort::NotOpenError) return;

    QDateTime now = QDateTime::currentDateTime();
    m_receiveEdit->append(QString("<font color='red'>[%1 ERR] %2</font>")
                              .arg(now.toString("HH:mm:ss.zzz"), m_serialPort->errorString()));
    if (m_serialPort->isOpen())
        m_serialPort->close();
    updatePortStatus(false);
}

void MainWindow::toggleAutoSend(bool enabled)
{
    if (enabled) {
        if (!m_serialPort->isOpen()) {
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
        this, "保存接收日志", "esp_uart_log.txt", "文本文件 (*.txt *.log);;所有文件 (*)");
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

void MainWindow::onDtrToggled(bool checked) { m_serialPort->setDataTerminalReady(checked); }
void MainWindow::onRtsToggled(bool checked) { m_serialPort->setRequestToSend(checked); }

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
    m_byteCounterLabel->setText(QString("TX: %1  |  RX: %2").arg(m_txBytes).arg(m_rxBytes));
}

void MainWindow::handleSerialStatusLine(const QByteArray &line)
{
    const QString text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty())
        return;

    if (text.contains("change over", Qt::CaseInsensitive)) {
        m_statusLabel->setText("STM32 已完成传输模式切换");
        m_statusLabel->setStyleSheet("color: #5bc0de; font-weight: bold;");
    } else if (text.compare("ok", Qt::CaseInsensitive) == 0) {
        m_statusLabel->setText("视频流传输成功");
        m_statusLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");
    } else if (text.startsWith("error:", Qt::CaseInsensitive)) {
        m_statusLabel->setText("STM32 " + text);
        m_statusLabel->setStyleSheet("color: #d9534f; font-weight: bold;");
    }
}

void MainWindow::updatePortStatus(bool opened)
{
    if (opened) {
        m_openBtn->setText("关闭串口");
        m_openBtn->setStyleSheet("background-color: #d9534f; color: white;");

        if (m_modeStack->currentIndex() == 1)
            m_statusLabel->setText(QString("热成像模式 — %1 @ %2 bps")
                                       .arg(m_serialPort->portName()).arg(m_serialPort->baudRate()));
        else
            m_statusLabel->setText(QString("已连接 — %1 @ %2 bps")
                                       .arg(m_serialPort->portName()).arg(m_serialPort->baudRate()));
        m_statusLabel->setStyleSheet("color: #5cb85c; font-weight: bold;");

        m_portCombo->setEnabled(false);
        m_baudRateCombo->setEnabled(false);
        m_dataBitsCombo->setEnabled(false);
        m_stopBitsCombo->setEnabled(false);
        m_parityCombo->setEnabled(false);
        m_refreshBtn->setEnabled(false);
        m_sendBtn->setEnabled(true);
        m_dtrCheck->setEnabled(true);
        m_rtsCheck->setEnabled(true);
    } else {
        m_autoSendTimer->stop();
        m_autoSendCheck->setChecked(false);

        m_openBtn->setText("打开串口");
        m_openBtn->setStyleSheet("");
        if (m_modeStack->currentIndex() == 1)
            m_statusLabel->setText("热成像模式 — 串口已关闭");
        else
            m_statusLabel->setText("串口已关闭");
        m_statusLabel->setStyleSheet("");

        m_portCombo->setEnabled(true);
        m_baudRateCombo->setEnabled(true);
        m_dataBitsCombo->setEnabled(true);
        m_stopBitsCombo->setEnabled(true);
        m_parityCombo->setEnabled(true);
        m_refreshBtn->setEnabled(true);
        m_sendBtn->setEnabled(false);
        m_dtrCheck->setEnabled(false);
        m_rtsCheck->setEnabled(false);
    }
}
