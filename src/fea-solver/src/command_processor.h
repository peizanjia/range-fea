#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

#include <QObject>
#include <QCoreApplication>
#include <QByteArray>
#include <QLocalSocket>

#include <memory>

class NativeExecutionGate;

class CommandProcessor : public QObject
{

    Q_OBJECT

    protected:

        //! Local socket for IPC.
        QLocalSocket *localSocket;

        //! Task server name for IPC.
        QString taskServer;

        //! Task ID.
        QString taskID;

        //! Per-run accepted-boundary gate; null preserves legacy STOP behavior.
        std::shared_ptr<NativeExecutionGate> nativeExecutionGate;

        //! Persistent bytes allow one frame to arrive across several readyRead events.
        QByteArray incomingBytes;

        //! A closed controller can cancel a gate-enabled run only once.
        bool commandServerLossHandled = false;

    public:

        //! Constructor.
        explicit CommandProcessor(const QString &taskServer,
                                  const QString &taskID,
                                  QCoreApplication *application = nullptr,
                                  std::shared_ptr<NativeExecutionGate> nativeExecutionGate = {});

    protected:

        //! Start listening and processing.
        bool start(void);

        //! Stop listening and processing.
        void stop(void);
        
    public slots:

        //! Read local socket.
        void readSocket(void);
        void onCommandServerDisconnected(void);
        void onCommandServerError(QLocalSocket::LocalSocketError error);

    protected:

        //! Parse one bounded, complete QDataStream frame and apply exact-task commands.
        void processAvailableFrames(void);
        void processCommand(const QString &message);
        void handleCommandServerLoss(void);
        
};

#endif // COMMAND_PROCESSOR_H
