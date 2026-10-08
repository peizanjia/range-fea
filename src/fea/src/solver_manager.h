#ifndef SOLVER_MANAGER_H
#define SOLVER_MANAGER_H

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <functional>

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
        //! Native solver job records, owned and updated on the manager thread.
        QHash<QString,QJsonObject> nativeJobs;
        //! Stable insertion order for bounded job queries.
        QStringList nativeJobOrder;
        //! Live native task pointers, guarded by QObject lifetime tracking.
        QHash<QString,QPointer<SolverTask>> nativeTasks;
        //! Immutable copied broker descriptors, kept out of paginated job status.
        QHash<QString,QJsonObject> nativeResultDescriptors;

    public:

        //! Constructor
        explicit SolverManager(QObject *parent = nullptr);

        //! Return static instance of the manager.
        static SolverManager & getInstance();

        //! Return task server name.
        const QString &getTaskServerName() const;

        //! Add new solver task.
        void submit(SolverTask *solverTask);

        //! Check native setup and return a detached validation report.
        QJsonObject validateRun(uint modelID) const;

        //! Validate, snapshot and enqueue a native solver task. The returned jobId is the task ID.
        QString submitValidated(uint modelID, const QJsonObject &projectSnapshotRef,
                                bool warningsAccepted, QString *error = nullptr);

        //! Submit an already constructed task through the same validated registry path.
        QString submitValidated(SolverTask *solverTask, const QJsonObject &projectSnapshotRef,
                                bool warningsAccepted, QString *error = nullptr);

        //! Return one native task record, or found=false when the task ID is unknown.
        QJsonObject jobQuery(const QString &jobId) const;

        //! Return a bounded page of native task records. Valid limits are 1 through 200.
        QJsonObject jobsQuery(int offset = 0, int limit = 100) const;

        //! Query only registered native artifacts or accepted live frame snapshots.
        //! Optional arguments.artifactId binds the request to the registry identity.
        QJsonObject nativeResultQuery(const QString &jobId, const QString &operation,
                                     const QJsonObject &arguments) const;
        //! Trusted scheduling lookup; retained hashes pin managed restart ancestry.
        QJsonObject nativeResultManifestForSource(const QString &sourcePath) const;

        //! Request cancellation for one native job ID.
        bool cancelJob(const QString &jobId, QString *error = nullptr);

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

    private:

        bool invokeOnOwner(const std::function<void()> &callback) const;
        QJsonObject validateRunOnOwner(uint modelID) const;
        QString submitTaskOnOwner(SolverTask *solverTask, const QJsonObject &projectSnapshotRef,
                                  bool warningsAccepted, QString *error);
        QString validateProjectSnapshotRef(uint modelID, const QPair<QString,quint64> &sourceStamp,
                                           const QJsonObject &projectSnapshotRef, QString *error) const;
        QString resolveFrozenSnapshotPath(const QJsonObject &projectSnapshotRef, QString *error) const;
        bool loadVerifiedFrozenSnapshot(const QJsonObject &projectSnapshotRef, RModel *frozenModel,
                                        QString *frozenPath, QString *frozenSha256, QString *error) const;
        void registerNativeTask(SolverTask *solverTask, const QString &jobId,
                                const QJsonObject &projectSnapshotRef, const QString &projectLink,
                                bool frozenSnapshotVerified, const QString &frozenSnapshotPath,
                                const QString &frozenSnapshotSha256);
        void updateNativeJob(const QString &jobId, const QJsonObject &fields);
        void appendNativeJobLog(const QString &jobId, const QString &stream, const QString &message);
        bool cancelJobOnOwner(const QString &jobId, QString *error);

};

#endif // SOLVER_MANAGER_H
