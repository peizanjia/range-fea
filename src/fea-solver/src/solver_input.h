#ifndef SOLVER_INPUT_H
#define SOLVER_INPUT_H

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <memory>
#include <optional>

class NativeExecutionGate;
class SolverTask;

class SolverInput
{

    protected:

        //! Model file.
        QString modelFileName;
        //! Convergence file.
        QString convergenceFileName;
        //! Monitoring file.
        QString monitoringFileName;
        //! Number of threads to use.
        //! Explicit CLI override; unset leaves the model/runtime policy intact.
        std::optional<uint> nThreads;
        //! Optional executable implementing the versioned control protocol.
        QString controlProgram;
        //! Passed directly to QProcess, never interpreted by a shell.
        QStringList controlArguments;
        //! Wall-clock limit for one external control response.
        uint controlTimeoutMs = 5000;
        //! Number of persisted thermal FE macrosteps requested by the CLI.
        uint thermalMacrosteps = 0;
        //! Atomic single-file destination for the accepted thermal state.
        QString thermalCheckpointFileName;
        uint typedControlSteps = 0;
        QString typedCheckpointFileName;
        QString typedControllerIdentity;
        QString typedConfigurationFileName;
        //! Optional directory for periodic accepted structural checkpoints.
        QString nativeCheckpointDirectory;
        //! Canonical range.native-checkpoint-context/v1 JSON bytes.
        QByteArray nativeCheckpointContextJson;
        //! Per-invocation mixed FE step budget; zero runs to the original target.
        std::optional<uint> nativeMixedStepBudget;
        //! Shared command/worker gate for an explicitly enabled run.
        std::shared_ptr<NativeExecutionGate> nativeExecutionGate;
        //! Force to restart solver
        bool restart;

    private:

        //! Internal initialization function.
        void _init(const SolverInput *pSolverInput = nullptr);

    public:

        //! Constructor.
        SolverInput(const QString &modelFileName);

        //! Copy constructor.
        SolverInput(const SolverInput &solverInput);

        //! Destructor.
        ~SolverInput();

        //! Assignment operator.
        SolverInput & operator =(const SolverInput &solverInput);

        //! Set convergence file name.
        void setConvergenceFileName(const QString &convergenceFileName);

        //! Set monitoring file name.
        void setMonitoringFileName(const QString &monitoringFileName);

        //! Set number of threads to use.
        void setNThreads(uint nThreads);

        //! Empty when --nthreads was not supplied.
        std::optional<uint> getNThreadsOverride() const;

        void setExternalControlProgram(const QString &program, const QStringList &arguments = {});
        QString getExternalControlProgram() const;
        QStringList getExternalControlArguments() const;
        void setExternalControlTimeoutMs(uint timeoutMs);
        uint getExternalControlTimeoutMs() const;

        void setThermalControlRun(uint macrosteps,const QString &checkpointFileName);
        void setTypedControlRun(uint steps,const QString &checkpointFileName,const QString &controllerIdentity,const QString &configurationFileName = {});

        //! Enable native accepted structural checkpoints with versioned context.
        void setNativeCheckpointRun(const QString &directory,const QByteArray &contextJson);

        void setNativeMixedStepBudget(uint steps);

        //! Bind the per-run accepted-boundary gate shared with command IPC.
        void setNativeExecutionGate(const std::shared_ptr<NativeExecutionGate> &gate);

        //! Set force restart solver.
        void setRestart(bool restart);

        friend class SolverTask;

};

#endif // SOLVER_INPUT_H
