#include "application.h"
#include "solver_manager.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

#include <exception>
#include <memory>
#include <utility>

SolverManager::SolverManager(QObject *parent)
    : RJobManager(parent)
    , taskServer(0)
{
    this->taskServerName = "RANGE_SOLVER_TASK_SERVER_" + Application::instance()->getSession()->getID();
    this->taskServer = new QLocalServer(this);
    if (!this->taskServer->listen(this->taskServerName))
    {
        RLogger::warning("Local server failed to listen: %s\n", this->taskServer->errorString().toUtf8().constData());
    }
    RLogger::info("Listening on: %s\n", this->taskServer->serverName().toUtf8().constData());
    QObject::connect(this->taskServer, &QLocalServer::newConnection, this, &SolverManager::onSolverTaskNewConnection);
}

SolverManager &SolverManager::getInstance()
{
    static SolverManager solverManager;
    return solverManager;
}

const QString &SolverManager::getTaskServerName() const
{
    return this->taskServerName;
}

void SolverManager::submit(SolverTask *solverTask)
{
    if (QThread::currentThread() != this->thread())
    {
        const bool invoked = QMetaObject::invokeMethod(this, [this, solverTask]()
        {
            this->submit(solverTask);
        }, Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            RLogger::error("Solver task could not be scheduled on the application thread.\n");
            solverTask->deleteLater();
        }
        return;
    }

    if (this->isSolverQueued(solverTask->getSourceDocumentId()))
    {
        solverTask->deleteLater();
        return;
    }

    Session *session = Application::instance()->getSession();
    if (QThread::currentThread() != session->thread())
    {
        RLogger::error("Solver input snapshots must be captured on the document thread.\n");
        solverTask->deleteLater();
        return;
    }

    if (solverTask->getSourceStamp().first.isEmpty())
    {
        RJobManager &legacyJobs = RJobManager::getInstance();
        if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0)
        {
            RLogger::warning("Solver task snapshot rejected while a legacy model job is active or queued.\n");
            solverTask->deleteLater();
            return;
        }

        QElapsedTimer captureTimer;
        captureTimer.start();
        if (!session->trylock())
        {
            RLogger::warning("Solver task snapshot rejected because the session is in use.\n");
            solverTask->deleteLater();
            return;
        }

        struct SessionUnlockGuard
        {
            Session *session;
            ~SessionUnlockGuard() { if (session) session->unlock(); }
        } unlockGuard{session};

        // Recheck the legacy queue after acquiring the document lock. A legacy
        // job submitted between the first check and trylock must not retain a
        // live model pointer while this snapshot is staged.
        if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0)
        {
            RLogger::warning("Solver task snapshot rejected while a legacy model job is active or queued.\n");
            solverTask->deleteLater();
            return;
        }

        const uint modelID = session->findModelByCommandIdentity(solverTask->getSourceDocumentId());
        if (modelID == RConstants::eod)
        {
            RLogger::warning("Solver task target was closed before scheduling.\n");
            solverTask->deleteLater();
            return;
        }

        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(modelID);
        if (sourceStamp.first != solverTask->getSourceDocumentId())
        {
            RLogger::warning("Solver task target identity changed before scheduling.\n");
            solverTask->deleteLater();
            return;
        }

        std::unique_ptr<Model> modelSnapshot;
        try
        {
            modelSnapshot = std::make_unique<Model>(session->getModel(modelID));
        }
        catch (const std::exception &error)
        {
            RLogger::error("Could not copy the model for solver snapshot: %s\n", error.what());
            solverTask->deleteLater();
            return;
        }
        if (session->findModelByCommandIdentity(sourceStamp.first) != modelID ||
            session->modelCommandStamp(modelID) != sourceStamp)
        {
            RLogger::warning("Solver task source changed while its snapshot was being copied.\n");
            solverTask->deleteLater();
            return;
        }
        const uint currentModelID = modelID;
        unlockGuard.session->unlock();
        unlockGuard.session = nullptr;

        if (!solverTask->captureInputSnapshot(std::move(modelSnapshot), sourceStamp, currentModelID))
        {
            RLogger::error("Failed to capture the solver input snapshot for the selected document.\n");
            solverTask->deleteLater();
            return;
        }
        const qint64 elapsed = captureTimer.elapsed();
        if (elapsed >= 1000)
        {
            RLogger::warning("Solver input snapshot capture took %lld ms (%lld bytes).\n",
                             static_cast<long long>(elapsed),
                             static_cast<long long>(QFileInfo(solverTask->getInputSnapshotFileName()).size()));
        }
        else
        {
            RLogger::info("Solver input snapshot captured in %lld ms (%lld bytes).\n",
                          static_cast<long long>(elapsed),
                          static_cast<long long>(QFileInfo(solverTask->getInputSnapshotFileName()).size()));
        }
    }
    else if (solverTask->getSourceStamp().first != solverTask->getSourceDocumentId() ||
             solverTask->getInputSnapshotSha256().isEmpty() ||
             solverTask->getInputPackageSha256().isEmpty() ||
             !QFileInfo::exists(solverTask->getInputSnapshotFileName()))
    {
        RLogger::error("Pre-captured solver task has incomplete or mismatched snapshot metadata.\n");
        solverTask->deleteLater();
        return;
    }

    QObject::connect(solverTask,
                     &SolverTask::readyReadStandardOutput,
                     this,
                     &SolverManager::onReadyReadStandardOutput);
    QObject::connect(solverTask,
                     &SolverTask::readyReadStandardError,
                     this,
                     &SolverManager::onReadyReadStandardError);
    QObject::connect(solverTask,
                     &SolverTask::resultArtifactCreated,
                     this,
                     [this](const QString &fileName) { this->lastResultArtifactFileName = fileName; },
                     Qt::QueuedConnection);

    this->lastLogFileName = solverTask->getLogFileName();
    this->RJobManager::submit(solverTask);
}

bool SolverManager::isSolverQueued(const QString &sourceDocumentId) const
{
    QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
    const auto matches = [&sourceDocumentId](RJob *job)
    {
        auto *solverTask = dynamic_cast<SolverTask*>(job);
        return solverTask && !solverTask->isFinished() &&
                !solverTask->getSourceDocumentId().isEmpty() &&
                solverTask->getSourceDocumentId() == sourceDocumentId;
    };
    for (RJob *job : this->waitingJobs) if (matches(job)) return true;
    for (RJob *job : this->waitingParallelJobs) if (matches(job)) return true;
    return false;
}

bool SolverManager::hasLiveModelTasks() const
{
    QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
    const auto holdsLiveModel = [](RJob *job)
    {
        if (!job || job->isFinished()) return false;
        const auto *solverTask = dynamic_cast<const SolverTask*>(job);
        return !solverTask || solverTask->getSourceStamp().first.isEmpty();
    };

    for (RJob *job : this->runningJobs)
        if (holdsLiveModel(job)) return true;
    for (RJob *job : this->waitingJobs)
        if (holdsLiveModel(job)) return true;
    for (RJob *job : this->waitingParallelJobs)
        if (holdsLiveModel(job)) return true;
    return false;
}

void SolverManager::stopRunningTasks()
{
    QList<SolverTask*> solverTasks;
    {
        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        for (RJob *job : this->runningJobs)
        {
            if (auto *solverTask = dynamic_cast<SolverTask*>(job)) solverTasks.append(solverTask);
        }
    }
    for (SolverTask *solverTask : solverTasks)
    {
        RLogger::info("Stopping solver task (#%s).\n",solverTask->getTaskID().toString().toUtf8().constData());
        QString message = solverTask->getStopCommand();
        foreach (QLocalSocket *localSocket, this->taskClients)
        {
            QByteArray block;
            QDataStream out(&block, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_8);
            RLogger::info("Sendig signal: %s\n",message.toUtf8().constData());
            out << quint32(message.size());
            out << message;
            localSocket->write(block);
            localSocket->flush();
        }
    }
}

void SolverManager::killRunningTasks()
{
    QList<SolverTask*> solverTasks;
    {
        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        for (RJob *job : this->runningJobs)
        {
            if (auto *solverTask = dynamic_cast<SolverTask*>(job)) solverTasks.append(solverTask);
        }
    }
    for (SolverTask *solverTask : solverTasks)
    {
        RLogger::info("Killing solver task (#%s).\n",solverTask->getTaskID().toString().toUtf8().constData());
        solverTask->kill();
    }
}

const QString &SolverManager::getLastLogFileName() const
{
    return this->lastLogFileName;
}

const QString &SolverManager::getLastResultArtifactFileName() const
{
    return this->lastResultArtifactFileName;
}

void SolverManager::stopServer()
{
    QObject::disconnect(this->taskServer, &QLocalServer::newConnection, this, &SolverManager::onSolverTaskNewConnection);
    this->taskServer->close();
}

void SolverManager::onReadyReadStandardOutput(const QString &message)
{
    emit this->readyReadStandardOutput(message);
}

void SolverManager::onReadyReadStandardError(const QString &message)
{
    emit this->readyReadStandardError(message);
}

void SolverManager::onSolverTaskNewConnection()
{
    RLogger::info("Task client connected\n");
    while (this->taskServer->hasPendingConnections())
    {
        this->taskClients.append(this->taskServer->nextPendingConnection());
    }
}
