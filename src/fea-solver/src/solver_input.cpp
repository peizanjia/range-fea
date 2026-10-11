#include "solver_input.h"

#include <QByteArray>

void SolverInput::_init(const SolverInput *pSolverInput)
{
    if (pSolverInput)
    {
        this->modelFileName = pSolverInput->modelFileName;
        this->convergenceFileName = pSolverInput->convergenceFileName;
        this->monitoringFileName = pSolverInput->monitoringFileName;
        this->nThreads = pSolverInput->nThreads;
        this->controlProgram = pSolverInput->controlProgram;
        this->controlArguments = pSolverInput->controlArguments;
        this->controlTimeoutMs = pSolverInput->controlTimeoutMs;
        this->thermalMacrosteps = pSolverInput->thermalMacrosteps;
        this->thermalCheckpointFileName = pSolverInput->thermalCheckpointFileName;
        this->typedControlSteps = pSolverInput->typedControlSteps;
        this->typedCheckpointFileName = pSolverInput->typedCheckpointFileName;
        this->typedControllerIdentity = pSolverInput->typedControllerIdentity;
        this->typedConfigurationFileName = pSolverInput->typedConfigurationFileName;
        this->nativeCheckpointDirectory = pSolverInput->nativeCheckpointDirectory;
        this->nativeCheckpointContextJson = pSolverInput->nativeCheckpointContextJson;
        this->nativeMixedStepBudget = pSolverInput->nativeMixedStepBudget;
        this->nativeExecutionGate = pSolverInput->nativeExecutionGate;
        this->restart = pSolverInput->restart;
    }
}

SolverInput::SolverInput(const QString &modelFileName)
    : modelFileName(modelFileName)
    , restart(false)
{
    this->_init();
}

SolverInput::SolverInput(const SolverInput &solverInput)
{
    this->_init(&solverInput);
}

SolverInput::~SolverInput()
{

}

SolverInput &SolverInput::operator =(const SolverInput &solverInput)
{
    this->_init(&solverInput);
    return (*this);
}

void SolverInput::setConvergenceFileName(const QString &convergenceFileName)
{
    this->convergenceFileName = convergenceFileName;
}

void SolverInput::setMonitoringFileName(const QString &monitoringFileName)
{
    this->monitoringFileName = monitoringFileName;
}

void SolverInput::setNThreads(uint nThreads)
{
    this->nThreads = nThreads;
}

std::optional<uint> SolverInput::getNThreadsOverride() const
{
    return this->nThreads;
}

void SolverInput::setExternalControlProgram(const QString &program, const QStringList &arguments)
{
    this->controlProgram = program;
    this->controlArguments = arguments;
}

QString SolverInput::getExternalControlProgram() const
{
    return this->controlProgram;
}

QStringList SolverInput::getExternalControlArguments() const
{
    return this->controlArguments;
}

void SolverInput::setExternalControlTimeoutMs(uint timeoutMs)
{
    this->controlTimeoutMs = timeoutMs;
}

uint SolverInput::getExternalControlTimeoutMs() const
{
    return this->controlTimeoutMs;
}

void SolverInput::setThermalControlRun(uint macrosteps,const QString &checkpointFileName)
{
    this->thermalMacrosteps = macrosteps;
    this->thermalCheckpointFileName = checkpointFileName;
}

void SolverInput::setRestart(bool restart)
{
    this->restart = restart;
}

void SolverInput::setTypedControlRun(uint steps,const QString &checkpointFileName,const QString &controllerIdentity,const QString &configurationFileName)
{
    this->typedControlSteps=steps;
    this->typedCheckpointFileName=checkpointFileName;
    this->typedControllerIdentity=controllerIdentity;
    this->typedConfigurationFileName=configurationFileName;
}

void SolverInput::setNativeCheckpointRun(const QString &directory,const QByteArray &contextJson)
{
    this->nativeCheckpointDirectory = directory;
    this->nativeCheckpointContextJson = contextJson;
}

void SolverInput::setNativeExecutionGate(
    const std::shared_ptr<NativeExecutionGate> &gate)
{
    this->nativeExecutionGate = gate;
}

void SolverInput::setNativeMixedStepBudget(uint steps)
{
    this->nativeMixedStepBudget = steps;
}
