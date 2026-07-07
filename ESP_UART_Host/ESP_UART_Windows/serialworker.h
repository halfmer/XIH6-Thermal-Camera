#ifndef SERIALWORKER_H
#define SERIALWORKER_H

#include <QObject>
#include <QSerialPort>
#include <QTimer>
#include <QMetaType>

// Parameters for opening the port, passed to the worker thread in one shot.
struct SerialOpenParams {
    QString portName;
    qint32 baudRate = 1500000;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::Parity   parity   = QSerialPort::NoParity;
    bool dtr = false;
    bool rts = false;
};
Q_DECLARE_METATYPE(SerialOpenParams)

// Owns the QSerialPort on a dedicated thread so the kernel RX buffer is
// drained even while the GUI thread is busy painting / logging / in a modal
// dialog. At high baud rates the Windows serial driver buffers only a few KB
// (~20-40 ms); any GUI stall beyond that silently drops bytes (Qt 6 removed
// the Overrun error, so nothing is reported). Moving readAll() off the GUI
// thread turns that hard kernel limit into an elastic in-process queue of
// queued QByteArray signals.
class SerialWorker : public QObject
{
    Q_OBJECT

public:
    explicit SerialWorker(QObject *parent = nullptr);

public slots:
    void openPort(const SerialOpenParams &params);
    // sendStop: emit 'P' + drain before closing so the MCU leaves stream mode.
    void closePort(bool sendStop);
    void writeBytes(const QByteArray &data);
    // enabled: clear stale input then 'S'; disabled: 'P', drain, clear input.
    void setStream(bool enabled);
    void setDtr(bool on);
    void setRts(bool on);

signals:
    void opened(bool ok, const QString &error);
    void closed();
    void bytesReceived(const QByteArray &data);
    // fatal errors (device gone) auto-close the port; the rest are report-only.
    void portError(const QString &message, bool fatal);
    void txWritten(qint64 bytes);

private:
    void flushRxBatch();

    QSerialPort *m_port = nullptr;
    // CH340's 32-byte bulk endpoint makes readyRead fire thousands of times/s
    // (~4700x/s at 1.5 Mbps, ~6000x/s at 2 Mbps).
    // Batch fragments here (worker thread) and forward at most ~60 batches/s
    // so the GUI thread is not flooded with queued 32-byte events.
    QByteArray   m_rxBatch;
    QTimer      *m_rxFlushTimer = nullptr;
};

#endif // SERIALWORKER_H
