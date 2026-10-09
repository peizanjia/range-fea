#include <QBuffer>
#include <QDataStream>

#include <stdexcept>
#include <utility>

#include <rbl_application_state.h>
#include <rbl_logger.h>

#include "command_processor.h"
#include "native_execution_gate.h"

namespace
{
constexpr quint32 maximumCommandCharacters = 512;
constexpr qsizetype maximumBufferedCommandBytes = 64 * 1024;
}

CommandProcessor::CommandProcessor(const QString &taskServer,
                                   const QString &taskID,
                                   QCoreApplication *application,
                                   std::shared_ptr<NativeExecutionGate> nativeExecutionGate)
    : QObject(application)
    , localSocket(new QLocalSocket(this))
    , taskServer(taskServer)
    , taskID(taskID)
    , nativeExecutionGate(std::move(nativeExecutionGate))
{
    if (!this->taskServer.isEmpty() && !this->taskID.isEmpty())
    {
        if (!this->start() && this->nativeExecutionGate)
            throw std::runtime_error("Native execution gate cannot start without its task command server.");
    }
}

bool CommandProcessor::start(void)
{
    this->localSocket->connectToServer(taskServer);
    if (!this->localSocket->waitForConnected())
    {
        RLogger::error("Failed to connect to: %s\n",taskServer.toUtf8().constData());
        return false;
    }
    RLogger::info("Connected to: %s\n",this->localSocket->serverName().toUtf8().constData());
    QObject::connect(this->localSocket, &QLocalSocket::readyRead,
                     this, &CommandProcessor::readSocket);
    if (this->nativeExecutionGate)
    {
        QObject::connect(this->localSocket,&QLocalSocket::disconnected,
                         this,&CommandProcessor::onCommandServerDisconnected);
        QObject::connect(this->localSocket,&QLocalSocket::errorOccurred,
                         this,&CommandProcessor::onCommandServerError);
    }
    return true;
}

void CommandProcessor::stop(void)
{
    this->localSocket->disconnectFromServer();
}

void CommandProcessor::readSocket(void)
{
    const QByteArray incoming = this->localSocket->readAll();
    if (incoming.isEmpty())
        return;
    if (incoming.size() > maximumBufferedCommandBytes - this->incomingBytes.size())
    {
        this->incomingBytes.clear();
        RLogger::warning("Rejected oversized local command stream.\n");
        return;
    }
    this->incomingBytes.append(incoming);
    this->processAvailableFrames();
}

void CommandProcessor::processAvailableFrames(void)
{
    while (this->incomingBytes.size() >= qsizetype(sizeof(quint32)))
    {
        QByteArray headerBytes = this->incomingBytes.left(qsizetype(sizeof(quint32)));
        QBuffer headerBuffer(&headerBytes);
        if (!headerBuffer.open(QIODevice::ReadOnly))
        {
            this->incomingBytes.clear();
            RLogger::warning("Rejected malformed local command header.\n");
            return;
        }
        QDataStream header(&headerBuffer);
        header.setVersion(QDataStream::Qt_6_8);
        quint32 characterCount = 0;
        header >> characterCount;
        if (header.status() != QDataStream::Ok || characterCount == 0 ||
            characterCount > maximumCommandCharacters)
        {
            this->incomingBytes.clear();
            RLogger::warning("Rejected malformed or oversized local command frame.\n");
            return;
        }

        const qsizetype stringHeaderBytes = qsizetype(sizeof(quint32));
        const qsizetype frameHeaderBytes = qsizetype(sizeof(quint32)) + stringHeaderBytes;
        if (this->incomingBytes.size() < frameHeaderBytes)
            return;

        QByteArray stringHeaderBytesData = this->incomingBytes.mid(
            qsizetype(sizeof(quint32)),stringHeaderBytes);
        QBuffer stringHeaderBuffer(&stringHeaderBytesData);
        if (!stringHeaderBuffer.open(QIODevice::ReadOnly))
        {
            this->incomingBytes.clear();
            RLogger::warning("Rejected malformed local command string header.\n");
            return;
        }
        QDataStream stringHeader(&stringHeaderBuffer);
        stringHeader.setVersion(QDataStream::Qt_6_8);
        quint32 stringByteCount = 0;
        stringHeader >> stringByteCount;
        if (stringHeader.status() != QDataStream::Ok ||
            stringByteCount != characterCount * quint32(sizeof(ushort)) ||
            (stringByteCount % sizeof(ushort)) != 0)
        {
            this->incomingBytes.clear();
            RLogger::warning("Rejected malformed local command string size.\n");
            return;
        }

        const qsizetype frameBytes = frameHeaderBytes + qsizetype(stringByteCount);
        if (this->incomingBytes.size() < frameBytes)
            return;

        QByteArray frameBytesData = this->incomingBytes.left(frameBytes);
        QBuffer frameBuffer(&frameBytesData);
        QString message;
        quint32 declaredCharacters = 0;
        bool validFrame = frameBuffer.open(QIODevice::ReadOnly);
        if (validFrame)
        {
            QDataStream frame(&frameBuffer);
            frame.setVersion(QDataStream::Qt_6_8);
            frame >> declaredCharacters >> message;
            validFrame = frame.status() == QDataStream::Ok && frameBuffer.atEnd() &&
                         declaredCharacters == characterCount &&
                         message.size() == qsizetype(characterCount);
        }
        this->incomingBytes.remove(0,frameBytes);
        if (!validFrame)
        {
            RLogger::warning("Rejected malformed local command frame.\n");
            continue;
        }
        this->processCommand(message);
    }
}

void CommandProcessor::processCommand(const QString &message)
{
    const QString taskPrefix = this->taskID + QLatin1Char(':');
    if (!message.startsWith(taskPrefix,Qt::CaseSensitive))
        return;

    const QString command = message.mid(taskPrefix.size());
    if (command == QStringLiteral("STOP"))
    {
        // Preserve the legacy stop signal while waking a native pause gate.
        RApplicationState::getInstance().setStateType(R_APPLICATION_STATE_STOP);
        if (this->nativeExecutionGate)
            this->nativeExecutionGate->requestCancel();
        return;
    }

    if (!this->nativeExecutionGate)
        return;
    if (command == QStringLiteral("RESUME"))
    {
        this->nativeExecutionGate->requestResume();
        return;
    }
    if (command.startsWith(QStringLiteral("PAUSE_AT:"),Qt::CaseSensitive))
    {
        const QString sequenceText = command.mid(QStringLiteral("PAUSE_AT:").size());
        if (sequenceText.isEmpty())
            return;
        for (const QChar character : sequenceText)
            if (character < QLatin1Char('0') || character > QLatin1Char('9'))
                return;
        bool valid = false;
        const quint64 sequence = sequenceText.toULongLong(&valid,10);
        if (valid && sequence > 0 && QString::number(sequence) == sequenceText)
            this->nativeExecutionGate->requestPauseAt(sequence);
    }
}

void CommandProcessor::onCommandServerDisconnected(void)
{
    this->handleCommandServerLoss();
}

void CommandProcessor::onCommandServerError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error);
    this->handleCommandServerLoss();
}

void CommandProcessor::handleCommandServerLoss(void)
{
    if (!this->nativeExecutionGate || this->commandServerLossHandled)
        return;
    this->commandServerLossHandled = true;
    RApplicationState::getInstance().setStateType(R_APPLICATION_STATE_STOP);
    if (this->nativeExecutionGate->requestCancel())
    {
        RLogger::warning("Native execution command host disconnected; cancelling at the next accepted boundary.\n");
    }
}
