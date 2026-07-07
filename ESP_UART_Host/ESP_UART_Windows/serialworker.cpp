#include "serialworker.h"

SerialWorker::SerialWorker(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))   // child: moves to the worker thread with us
    , m_rxFlushTimer(new QTimer(this))
{
    m_rxFlushTimer->setInterval(15);
    m_rxFlushTimer->setSingleShot(true);
    connect(m_rxFlushTimer, &QTimer::timeout, this, &SerialWorker::flushRxBatch);

    connect(m_port, &QSerialPort::readyRead, this, [this]() {
        // Drain immediately on the worker thread, but batch before forwarding:
        // at 1.5-2 Mbps the CH340 delivers 32-byte chunks thousands of times/s
        // and a queued signal per chunk would flood the GUI event loop.
        m_rxBatch.append(m_port->readAll());
        if (m_rxBatch.size() >= 8192)
            flushRxBatch();
        else if (!m_rxFlushTimer->isActive())
            m_rxFlushTimer->start();
    });

    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError e) {
        // TimeoutError comes from waitForBytesWritten() and is not a link
        // failure. Never close the port on transient read/write errors —
        // the old code closed on ANY error, which silently killed the
        // video stream.
        if (e == QSerialPort::NoError || e == QSerialPort::NotOpenError ||
            e == QSerialPort::TimeoutError)
            return;

        const bool fatal = (e == QSerialPort::ResourceError ||
                            e == QSerialPort::DeviceNotFoundError ||
                            e == QSerialPort::PermissionError ||
                            e == QSerialPort::OpenError);
        emit portError(QString("err=%1 %2").arg(int(e)).arg(m_port->errorString()),
                       fatal);
        if (fatal && m_port->isOpen()) {
            m_port->close();
            emit closed();
        }
    });
}

void SerialWorker::flushRxBatch()
{
    m_rxFlushTimer->stop();
    if (m_rxBatch.isEmpty())
        return;
    emit bytesReceived(m_rxBatch);
    m_rxBatch = QByteArray();   // detach: receiver keeps the old buffer
}

void SerialWorker::openPort(const SerialOpenParams &params)
{
    if (m_port->isOpen())
        m_port->close();
    m_rxBatch.clear();
    m_rxFlushTimer->stop();

    m_port->setPortName(params.portName);
    m_port->setBaudRate(params.baudRate);
    m_port->setDataBits(params.dataBits);
    m_port->setStopBits(params.stopBits);
    m_port->setParity(params.parity);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        emit opened(false, m_port->errorString());
        return;
    }

    m_port->setDataTerminalReady(params.dtr);
    m_port->setRequestToSend(params.rts);
    emit opened(true, QString());
}

void SerialWorker::closePort(bool sendStop)
{
    flushRxBatch();   // hand over anything already drained from the kernel
    if (m_port->isOpen()) {
        if (sendStop) {
            m_port->write("P");
            m_port->flush();
            m_port->waitForBytesWritten(100);
            m_port->clear(QSerialPort::Input);
        }
        m_port->close();
    }
    emit closed();
}

void SerialWorker::writeBytes(const QByteArray &data)
{
    if (!m_port->isOpen() || data.isEmpty())
        return;
    const qint64 n = m_port->write(data);
    if (n > 0)
        emit txWritten(n);
}

void SerialWorker::setStream(bool enabled)
{
    if (!m_port->isOpen())
        return;
    if (enabled) {
        m_rxBatch.clear();                   // drop batched pre-'S' residue
        m_port->clear(QSerialPort::Input);   // drop kernel-side residue
        m_port->write("S");
        m_port->flush();
    } else {
        m_port->write("P");
        m_port->flush();
        m_port->waitForBytesWritten(100);    // worker thread — GUI not blocked
        m_port->clear(QSerialPort::Input);   // drop in-flight frame tail
        m_rxBatch.clear();
    }
}

void SerialWorker::setDtr(bool on)
{
    if (m_port->isOpen())
        m_port->setDataTerminalReady(on);
}

void SerialWorker::setRts(bool on)
{
    if (m_port->isOpen())
        m_port->setRequestToSend(on);
}
