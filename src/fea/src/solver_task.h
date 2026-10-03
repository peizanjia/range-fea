#ifndef SOLVER_TASK_H
#define SOLVER_TASK_H

#include <atomic>
#include <memory>

#include <QByteArray>
#include <QPair>
#include <QProcess>

#include <rbl_job.h>

#include "application_settings.h"
#include "model.h"
#include "solver_task_id.h"

class SolverTask : public RJob
{

    Q_OBJECT

    protected:

        //! Application settings.
        const ApplicationSettings *applicationSettings;
        //! Model ID
        uint modelID;
        //! Stable identity captured when the task was created.
        QString sourceDocumentId;
        //! Source identity and revision captured when the task is scheduled.
        QPair<QString, quint64> sourceStamp;
        //! Original source model path retained for provenance and dependency resolution.
        QString sourceModelFileName;
        //! Solver task ID.
        SolverTaskID taskID;
        //! Solver process.
        QProcess *solverProcess;
        //! Cross-thread cancellation request; the worker owns all QProcess calls.
        std::atomic_bool killRequested{false};
        //! Solver executable.
        QString solverExecutable;
        //! Solver executable arguments.
        QStringList solverArguments;
        //! Model file name.
        QString modelFileName;
        //! Log file name.
        QString logFileName;
        //! Convergence file name.
        QString convergenceFileName;
        //! Monitoring file name.
        QString monitoringFileName;
        //! Detached model copy captured on the document thread at scheduling.
        std::unique_ptr<Model> inputSnapshot;
        //! Detached result copy owned and updated by the worker.
        std::unique_ptr<Model> resultSnapshot;
        //! Immutable task input file and retained solver result artifact path.
        QString inputSnapshotFileName;
        QString resultArtifactFileName;
        //! SHA-256 of the serialized immutable input model file.
        QByteArray inputSnapshotSha256;
        //! SHA-256 of the immutable input package, including staged references and history.
        QByteArray inputPackageSha256;

    public:

        //! Constructor.
        explicit SolverTask(const ApplicationSettings *applicationSettings, uint modelID);

        //! Capture the input model and its source stamp before the job is queued.
        bool captureInputSnapshot(const Model &model,
                                  const QPair<QString, quint64> &sourceStamp,
                                  uint currentModelID);

        //! Capture an already detached model copy and source stamp.
        bool captureInputSnapshot(std::unique_ptr<Model> modelSnapshot,
                                  const QPair<QString, quint64> &sourceStamp,
                                  uint currentModelID);

        //! Return the stable source identity selected when this task was created.
        const QString &getSourceDocumentId() const;

        //! Return the captured source identity and revision.
        const QPair<QString, quint64> &getSourceStamp() const;

        //! Return the original source model path captured for this task.
        const QString &getSourceModelFileName() const;

        //! Return the immutable input snapshot path.
        const QString &getInputSnapshotFileName() const;

        //! Return the SHA-256 of the serialized immutable input model file.
        const QByteArray &getInputSnapshotSha256() const;

        //! Return the SHA-256 of the complete immutable input package.
        const QByteArray &getInputPackageSha256() const;

        //! Return the retained result artifact path.
        const QString &getResultArtifactFileName() const;

        //! Return model ID.
        uint getModelID() const;

        //! Return const reference to solver task ID.
        const SolverTaskID & getTaskID() const;

        //! Return reference to solver task ID.
        SolverTaskID & getTaskID();

        //! Return const reference to solver task ID.
        const QString getStopCommand() const;

        //! Kill task.
        void kill();

        //! Return log file name.
        const QString & getLogFileName() const;

        //! Return command line.
        QString getCommandLine() const;

    protected:

        //! Run job.
        virtual int perform() override final;

    private slots:

        //! Catch ready read standard output signal.
        void onProcessReadyReadStandardOutput();

        //! Catch ready read standard error signal.
        void onProcessReadyReadStandardError();

    signals:

        //! Message on standard output is availabe.
        void readyReadStandardOutput(QString message);

        //! Message on standard error is availabe.
        void readyReadStandardError(QString message);

        //! A complete task result artifact was read back successfully.
        void resultArtifactCreated(QString fileName);

};

#endif // SOLVER_TASK_H
