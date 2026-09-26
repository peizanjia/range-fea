#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QSettings>
#include <exception>

#include <rbl_arguments_parser.h>
#include <rbl_error.h>
#include <rbl_logger.h>
#include <rbl_utils.h>

#include "main_task.h"
#include "solver_task.h"
#include "command_processor.h"
#include "linear_model_task.h"
#include <rparallel_execution_context.h>

MainTask::MainTask(QObject *parent) :
    QObject(parent)
{
}

void MainTask::run()
{
    QCoreApplication *application = (QCoreApplication*)this->parent();

    QSettings settings(RVendor::name(), RVendor::shortName());

    try
    {
        // Process command line arguments.
        QList<RArgumentOption> validOptions;

        validOptions.append(RArgumentOption("file",RArgumentOption::Path,QVariant(),"File name",RArgumentOption::Mandatory,false));
        validOptions.append(RArgumentOption("log-file",RArgumentOption::Path,QVariant(),"Log file name",RArgumentOption::Logger,false));
        validOptions.append(RArgumentOption("convergence-file",RArgumentOption::Path,QVariant(),"Convergence file name",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("monitoring-file",RArgumentOption::Path,QVariant(),"Monitoring file name",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("nthreads",RArgumentOption::Integer,QVariant(),"Optional OpenMP thread override; otherwise use the model/runtime policy",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("control-program",RArgumentOption::Path,QVariant(),"External control executable (requires enabled structural control setup)",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("control-args-json",RArgumentOption::String,QVariant(),"JSON string array passed as separate control process arguments",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("control-timeout-ms",RArgumentOption::Integer,QVariant(),"Positive per-request external control timeout in milliseconds (default 5000)",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("thermal-steps",RArgumentOption::Integer,QVariant(),"Run 1..1000 thermal FE macrosteps; requires --thermal-checkpoint",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("thermal-checkpoint",RArgumentOption::Path,QVariant(),"Atomically write/resume the thermal FE .tfec checkpoint",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("typed-config",RArgumentOption::Path,QVariant(),"Start typed FE control from a native model and version-1 JSON configuration",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("linear-model-output",RArgumentOption::Path,QVariant(),"Export Stage-9 SI working-point M/C/K and force/sensor matrices to JSON",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("typed-steps",RArgumentOption::Integer,QVariant(),"Run 1..1000 quasistatic typed cable/piezo control steps from a .sfec bundle",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("typed-checkpoint",RArgumentOption::Path,QVariant(),"Atomic accepted typed FE .sfec destination",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("typed-controller-id",RArgumentOption::String,QVariant(),"Expected typed controller implementation/revision identity",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("restart",RArgumentOption::Switch,QVariant(),"Restart solver",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("task-id",RArgumentOption::Path,QVariant(),"Task ID for inter process communication",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("task-server",RArgumentOption::Path,QVariant(),"Task server for inter process communication",RArgumentOption::Optional,false));
        validOptions.append(RArgumentOption("log-debug",RArgumentOption::Switch,QVariant(),"Switch on debug log level",RArgumentOption::Logger,false));
        validOptions.append(RArgumentOption("log-trace",RArgumentOption::Switch,QVariant(),"Switch on trace log level",RArgumentOption::Logger,false));
        validOptions.append(RArgumentOption("log-ssl",RArgumentOption::Switch,QVariant(),"Enable Qt debug logging",RArgumentOption::Logger,false));
        validOptions.append(RArgumentOption("log-qt",RArgumentOption::Switch,QVariant(),"Enable Qt SSL debug logging",RArgumentOption::Logger,false));

        RArgumentsParser argumentsParser(QCoreApplication::arguments(),validOptions,false);

        if (argumentsParser.isSet("help"))
        {
            argumentsParser.printHelp();
            application->exit(0);
            return;
        }

        if (argumentsParser.isSet("version"))
        {
            argumentsParser.printVersion();
            application->exit(0);
            return;
        }

        if (argumentsParser.isSet("log-debug"))
        {
            RLogger::getInstance().setLevel(R_LOG_LEVEL_DEBUG);
        }
        if (argumentsParser.isSet("log-trace"))
        {
            RLogger::getInstance().setLevel(R_LOG_LEVEL_TRACE);
        }
        if (argumentsParser.isSet("log-qt") ||
            argumentsParser.isSet("log-ssl"))
        {
            RLogger::installQtMessageHandler();
            RLogger::debug("Qt message handler installed\n");

            if (argumentsParser.isSet("log-qt"))
            {
                QLoggingCategory::setFilterRules("qt.*=true");
                RLogger::debug("All Qt logging enabled\n");
            }
            else if (argumentsParser.isSet("log-ssl"))
            {
                QLoggingCategory::setFilterRules(
                    "qt.network.ssl.debug=true\n"
                    "qt.network.ssl.info=true\n"
                    "qt.network.ssl.warning=true\n"
                    "qt.tlsbackend.ossl.debug=true\n"
                    "qt.tlsbackend.ossl.info=true\n"
                    "qt.tlsbackend.ossl.warning=true"
                    );
                RLogger::debug("SSL logging enabled\n");
            }
        }
        if (argumentsParser.isSet("log-file"))
        {
            RLogger::getInstance().setFile(argumentsParser.getValue("log-file").toString());
        }

        if (argumentsParser.isSet("linear-model-output"))
        {
            for (const auto *option:{"control-program","control-args-json","control-timeout-ms","typed-config","typed-steps","typed-checkpoint","typed-controller-id","thermal-steps","thermal-checkpoint","restart","convergence-file","monitoring-file","task-id","task-server"})
                if (argumentsParser.isSet(option))
                    throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                 "Linear-model export cannot combine simulation/control/task output options.");
            // Offline matrix assembly defaults to deterministic serial execution.
            RParallelExecutionConfig policy;
            policy.setMode(RParallelExecutionMode::SerialDeterministic);
            if (argumentsParser.isSet("nthreads")) {
                bool valid=false;
                const auto count=argumentsParser.getValue("nthreads").toString().toUInt(&valid);
                if (!valid || count==0)
                    throw RError(RError::Type::InvalidInput,R_ERROR_REF,"--nthreads must be a positive integer.");
                policy.setMode(RParallelExecutionMode::FixedThreads);
                policy.setRequestedThreads(count);
            }
            RParallelExecutionContext execution(policy);
            exportStructuralLinearModel(argumentsParser.getValue("file").toString(),
                                        argumentsParser.getValue("linear-model-output").toString());
            application->exit(0);
            return;
        }

        // Command processor.
        new CommandProcessor(argumentsParser.getValue("task-server").toString(),
                             argumentsParser.getValue("task-id").toString(),
                             application);

        SolverInput solverInput(argumentsParser.getValue("file").toString());
        const bool typedMode=argumentsParser.isSet("typed-steps");
        if (typedMode != argumentsParser.isSet("typed-checkpoint") ||
            typedMode != argumentsParser.isSet("typed-controller-id"))
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "--typed-steps, --typed-checkpoint and --typed-controller-id must be supplied together.");
        if (argumentsParser.isSet("typed-config") && !typedMode)
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,"--typed-config requires typed FE mode.");
        if (typedMode)
        {
            bool valid=false;
            const auto steps=argumentsParser.getValue("typed-steps").toString().toLongLong(&valid);
            if (!valid || steps<1 || steps>1000)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,"--typed-steps must be an integer in 1..1000.");
            if (!argumentsParser.isSet("control-program") || argumentsParser.isSet("thermal-steps") ||
                argumentsParser.isSet("thermal-checkpoint") || argumentsParser.isSet("restart") ||
                argumentsParser.isSet("convergence-file") || argumentsParser.isSet("monitoring-file"))
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Typed FE mode requires --control-program and cannot combine thermal, restart or legacy result-file options.");
            solverInput.setTypedControlRun(uint(steps),argumentsParser.getValue("typed-checkpoint").toString(),
                argumentsParser.getValue("typed-controller-id").toString(),
                argumentsParser.isSet("typed-config") ? argumentsParser.getValue("typed-config").toString() : QString());
        }
        if (argumentsParser.isSet("thermal-steps") != argumentsParser.isSet("thermal-checkpoint"))
        {
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "--thermal-steps and --thermal-checkpoint must be supplied together.");
        }
        if (argumentsParser.isSet("thermal-steps"))
        {
            bool validStepCount = false;
            const qlonglong steps = argumentsParser.getValue("thermal-steps").toString().toLongLong(&validStepCount);
            if (!validStepCount || steps < 1 || steps > 1000)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "--thermal-steps must be an integer in 1..1000.");
            if (argumentsParser.isSet("restart") || argumentsParser.isSet("control-program") ||
                argumentsParser.isSet("control-args-json") || argumentsParser.isSet("control-timeout-ms"))
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Thermal FE mode cannot be combined with --restart or external force-control options.");
            solverInput.setThermalControlRun(uint(steps),
                argumentsParser.getValue("thermal-checkpoint").toString());
        }
        if (argumentsParser.isSet("convergence-file"))
        {
            solverInput.setConvergenceFileName(argumentsParser.getValue("convergence-file").toString());
        }
        if (argumentsParser.isSet("monitoring-file"))
        {
            solverInput.setMonitoringFileName(argumentsParser.getValue("monitoring-file").toString());
        }
        if (argumentsParser.isSet("nthreads"))
        {
            solverInput.setNThreads(argumentsParser.getValue("nthreads").toUInt());
        }
        if (argumentsParser.isSet("control-args-json") && !argumentsParser.isSet("control-program"))
        {
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "--control-args-json requires --control-program.");
        }
        if (argumentsParser.isSet("control-timeout-ms") && !argumentsParser.isSet("control-program"))
        {
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "--control-timeout-ms requires --control-program.");
        }
        if (argumentsParser.isSet("control-timeout-ms"))
        {
            const int timeout = argumentsParser.getValue("control-timeout-ms").toInt();
            if (timeout <= 0 || timeout > 60000)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "--control-timeout-ms must be in 1..60000.");
            solverInput.setExternalControlTimeoutMs(uint(timeout));
        }
        if (argumentsParser.isSet("control-program"))
        {
            QStringList controlArguments;
            if (argumentsParser.isSet("control-args-json"))
            {
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(
                    argumentsParser.getValue("control-args-json").toString().toUtf8(),&parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isArray())
                    throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                 "--control-args-json must be a JSON string array.");
                for (const QJsonValue &value : document.array())
                {
                    if (!value.isString())
                        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                     "--control-args-json must contain only strings.");
                    controlArguments.push_back(value.toString());
                }
            }
            solverInput.setExternalControlProgram(
                argumentsParser.getValue("control-program").toString(),controlArguments);
        }
        if (argumentsParser.isSet("restart"))
        {
            solverInput.setRestart(true);
        }

        // Start solver.
        QThread* thread = new QThread;
        SolverTask *solverTask = new SolverTask(solverInput, application);

        solverTask->moveToThread(thread);

        QObject::connect(thread, &QThread::started, solverTask, &SolverTask::run);
        QObject::connect(solverTask, &SolverTask::finished, solverTask, &SolverTask::deleteLater);
        QObject::connect(solverTask, &SolverTask::destroyed, thread, &QThread::quit);
        QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        QObject::connect(thread, &QThread::finished, application, &QCoreApplication::quit);

        thread->start();
    }
    catch (const RError &error)
    {
        RLogger::error("Failed to start solver. %s\n",error.getMessage().toUtf8().constData());
        application->exit(1);
    }
    catch (const std::exception &error)
    {
        RLogger::error("Failed to start solver. %s\n",error.what());
        application->exit(1);
    }
}
