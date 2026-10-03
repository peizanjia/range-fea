#ifndef SOLVER_MANAGER_H
#define SOLVER_MANAGER_H

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>

#include <rbl_job_manager.h>
#include "solver_task.h"

class SolverManager : public RJobManager
{

    Q_OBJECT

    protected:

        //! Task server name.
        QString taskServerName;

        //! Task server.
        QLocalServer *taskServer;

        //! Task clients.
        QList<QLocalSocket*> taskClients;

        //! Last solver log file name.
        QString lastLogFileName;
        //! Latest successfully produced task result artifact.
        QString lastResultArtifactFileName;

    public:

        //! Constructor
        explicit SolverManager(QObject *parent = nullptr);

        //! Return static instance of the manager.
        static SolverManager & getInstance();

        //! Return task server name.
        const QString &getTaskServerName() const;

        //! Add new solver task.
        void submit(SolverTask *solverTask);

        //! Check if solver for given model ID is queued.
        bool isSolverQueued(const QString &sourceDocumentId) const;

        //! Return true when any active solver-manager job still holds live models.
        bool hasLiveModelTasks() const;

        //! Stop current solvers.
        void stopRunningTasks();

        //! Kill current solvers.
        void killRunningTasks();

        //! Return const reference to last log file name.
        const QString & getLastLogFileName() const;

        //! Return const reference to the latest solver result artifact path.
        const QString & getLastResultArtifactFileName() const;

        //! Stop server.
        void stopServer();

    signals:

        //! Message on standard output is availabe.
        void readyReadStandardOutput(const QString &message);

        //! Message on standard error is availabe.
        void readyReadStandardError(const QString &message);

    public slots:

        //! Message on standard output is availabe.
        void onReadyReadStandardOutput(const QString &message);

        //! Message on standard error is availabe.
        void onReadyReadStandardError(const QString &message);

    protected slots:

        //! Solver has connected.
        void onSolverTaskNewConnection();

};

#endif // SOLVER_MANAGER_H
