#include <rml_file_utils.h>
#include <algorithm>
#include <QRegularExpression>

#include <rgl_message_box.h>

#if RANGE_ENABLE_AGENT
#include <ragent_evaluation_capture_store.h>
#include <rtrusted_holdout_pack_verifier.h>
#endif

#include "application.h"
#include "action.h"
#include "solver_manager.h"

Application::Application(int &argc, char **argv)
    : RApplication{argc,argv,false}
{
    // These optional services are normally created by onStarted(). Headless
    // evidence reopening does not enter that lifecycle, so define their absence.
    this->cloudSessionManager = nullptr;
    this->cloudConnectionHandler = nullptr;
#if RANGE_ENABLE_AGENT
    this->aiAgentSettingsManager = nullptr;
#endif
    // Set translator names
    this->translatorNames << "fea";
    this->translatorNames << "range-gui-lib";
    this->translatorNames << "range-cloud-lib";

    this->applicationSettings = new ApplicationSettings(this);
    this->applicationSettings->setDirApplicationPath(this->applicationDirPath());

}

#if RANGE_ENABLE_AGENT
bool Application::parseEvaluationCaptureConfiguration(bool armIsSet, const QString &armValue,
                                                       bool captureIsSet, const QString &capturePath,
                                                       EvaluationCaptureConfiguration *configuration,
                                                       QString *error)
{
    if (error) error->clear();
    if (!configuration)
    {
        if (error) *error = QStringLiteral("Evaluation capture configuration output is required.");
        return false;
    }
    *configuration = {};
    if (!armIsSet && !captureIsSet) return true;
    if (armIsSet != captureIsSet)
    {
        if (error) *error = QStringLiteral("Set both RANGE_AGENT_EVALUATION_ARM and RANGE_AGENT_EVALUATION_CAPTURE, or leave both unset.");
        return false;
    }

    if (armValue == QStringLiteral("direct_nl"))
        configuration->arm = AgentTaskRunner::EvaluationArm::DirectNl;
    else if (armValue == QStringLiteral("tools_feedback"))
        configuration->arm = AgentTaskRunner::EvaluationArm::ToolsFeedback;
    else if (armValue == QStringLiteral("full_trusted"))
        configuration->arm = AgentTaskRunner::EvaluationArm::FullTrusted;
    else
    {
        if (error) *error = QStringLiteral("RANGE_AGENT_EVALUATION_ARM must be exactly direct_nl, tools_feedback, or full_trusted.");
        return false;
    }

    if (capturePath.trimmed().isEmpty())
    {
        if (error) *error = QStringLiteral("RANGE_AGENT_EVALUATION_CAPTURE must name a new JSONL file.");
        return false;
    }

    configuration->enabled = true;
    configuration->capturePath = capturePath;
    return true;
}

bool Application::parseEvaluationSlotBinding(bool runIdIsSet, const QString &runId,
                                             bool holdoutIdIsSet, const QString &holdoutId,
                                             bool slotIdIsSet, const QString &slotId,
                                             bool caseIdIsSet, const QString &caseId,
                                             bool repeatIsSet, const QString &repeatValue,
                                             bool manifestCommitmentIsSet, const QString &manifestCommitmentSha256,
                                             bool promptDigestIsSet, const QString &promptSha256,
                                             AgentTaskRunner::EvaluationArm arm,
                                             AgentEvaluationSlotBinding *binding,
                                             QString *error)
{
    if (error) error->clear();
    if (!binding)
    {
        if (error) *error = QStringLiteral("Evaluation slot binding output is required.");
        return false;
    }
    *binding = {};
    const QList<bool> presence{runIdIsSet, holdoutIdIsSet, slotIdIsSet, caseIdIsSet,
                               repeatIsSet, manifestCommitmentIsSet, promptDigestIsSet};
    const int presentCount = std::count(presence.cbegin(), presence.cend(), true);
    if (presentCount == 0) return true;
    if (presentCount != presence.size())
    {
        if (error) *error = QStringLiteral("Set all seven RANGE_AGENT_EVALUATION_* slot-binding values, or leave all unset.");
        return false;
    }
    if (arm == AgentTaskRunner::EvaluationArm::Standard)
    {
        if (error) *error = QStringLiteral("A v4 slot binding requires a direct_nl, tools_feedback, or full_trusted evaluation arm.");
        return false;
    }

    const QRegularExpression identifier(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\\z"));
    static const QRegularExpression digest(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    const auto validId = [&identifier](const QString &value) {
        return identifier.match(value).hasMatch();
    };
    if (!validId(runId) || !validId(holdoutId) || !validId(caseId)
        || !digest.match(manifestCommitmentSha256).hasMatch()
        || !digest.match(promptSha256).hasMatch())
    {
        if (error) *error = QStringLiteral("The v4 run, holdout, case, manifest, or prompt identity is malformed.");
        return false;
    }
    if (!QRegularExpression(QStringLiteral("\\A[1-3]\\z")).match(repeatValue).hasMatch())
    {
        if (error) *error = QStringLiteral("RANGE_AGENT_EVALUATION_REPEAT must be exactly 1, 2, or 3.");
        return false;
    }
    const int repeat = repeatValue.toInt();
    const QString armId = AgentTaskRunner::evaluationArmId(arm);
    const QString expectedSlotId = QStringLiteral("%1:%2:%3:r%4").arg(armId, holdoutId, caseId).arg(repeat);
    if (slotId != expectedSlotId || slotId.toUtf8().size() > 512)
    {
        if (error) *error = QStringLiteral("RANGE_AGENT_EVALUATION_SLOT_ID does not match the arm, holdout, case, and repeat values.");
        return false;
    }
    binding->enabled = true;
    binding->runId = runId;
    binding->holdoutId = holdoutId;
    binding->slotId = slotId;
    binding->caseId = caseId;
    binding->manifestCommitmentSha256 = manifestCommitmentSha256;
    binding->promptSha256 = promptSha256;
    binding->repeat = repeat;
    return true;
}
#endif

bool Application::configureEvaluationCaptureFromEnvironment(QString *error)
{
    if (error) error->clear();
    const bool armIsSet = qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_ARM");
    const bool captureIsSet = qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_CAPTURE");
#if RANGE_ENABLE_AGENT
    EvaluationCaptureConfiguration configuration;
    if (!parseEvaluationCaptureConfiguration(armIsSet,
            qEnvironmentVariable("RANGE_AGENT_EVALUATION_ARM"),
            captureIsSet, qEnvironmentVariable("RANGE_AGENT_EVALUATION_CAPTURE"),
            &configuration, error))
        return false;
    AgentEvaluationSlotBinding parsedSlotBinding;
    QString slotError;
    if (!parseEvaluationSlotBinding(
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_RUN_ID"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_RUN_ID"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_HOLDOUT_ID"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_HOLDOUT_ID"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_SLOT_ID"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_SLOT_ID"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_CASE_ID"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_CASE_ID"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_REPEAT"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_REPEAT"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_MANIFEST_SHA256"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_MANIFEST_SHA256"),
            qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_PROMPT_SHA256"), qEnvironmentVariable("RANGE_AGENT_EVALUATION_PROMPT_SHA256"),
            configuration.arm, &parsedSlotBinding, &slotError))
    {
        if (error) *error = slotError;
        return false;
    }
    if (parsedSlotBinding.enabled && !configuration.enabled)
    {
        if (error) *error = QStringLiteral("A v4 slot binding requires the evaluation arm and capture path.");
        return false;
    }

    AgentEvaluationSlotBinding slotBinding;
    const QString manifestPath = qEnvironmentVariable("RANGE_AGENT_EVALUATION_MANIFEST_PATH");
    const QString publicCasesPath = qEnvironmentVariable("RANGE_AGENT_EVALUATION_PUBLIC_CASES_PATH");
    const QString publicKeyPath = qEnvironmentVariable("RANGE_AGENT_EVALUATION_PUBLIC_KEY_PATH");
    const QString publicKeySha256 = qEnvironmentVariable("RANGE_AGENT_EVALUATION_PUBLIC_KEY_SHA256");
    const QString externalCommitmentSha256 = qEnvironmentVariable("RANGE_AGENT_EVALUATION_EXTERNAL_PRE_RUN_COMMITMENT_SHA256");
    const QList<bool> verifierInputs{
        qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_MANIFEST_PATH"),
        qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_PUBLIC_CASES_PATH"),
        qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_PUBLIC_KEY_PATH"),
        qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_PUBLIC_KEY_SHA256"),
        qEnvironmentVariableIsSet("RANGE_AGENT_EVALUATION_EXTERNAL_PRE_RUN_COMMITMENT_SHA256")};
    const int verifierInputCount = std::count(verifierInputs.cbegin(), verifierInputs.cend(), true);
    if ((parsedSlotBinding.enabled && verifierInputCount != verifierInputs.size())
        || (!parsedSlotBinding.enabled && verifierInputCount != 0))
    {
        if (error) *error = QStringLiteral("Signed-pack paths, key fingerprint, and pre-run commitment are required only as a complete group for a v4 slot.");
        return false;
    }
    if (parsedSlotBinding.enabled)
    {
        RTrustedHoldoutPackVerifier::Request request;
        request.manifestPath = manifestPath;
        request.publicCasesPath = publicCasesPath;
        request.publicKeyPath = publicKeyPath;
        request.independentlyPinnedKeySha256 = publicKeySha256;
        request.externallyRecordedPreRunCommitmentSha256 = externalCommitmentSha256;
        request.runId = parsedSlotBinding.runId;
        request.armId = AgentTaskRunner::evaluationArmId(configuration.arm);
        request.slotId = parsedSlotBinding.slotId;
        const auto verified = RTrustedHoldoutPackVerifier::verifyPublicSlot(request);
        if (!verified.valid)
        {
            if (error) *error = QStringLiteral("Signed Stage 7 public-pack verification failed (%1): %2")
                                    .arg(verified.code, verified.message);
            return false;
        }

        const auto &verifiedSlot = verified.slot;
        if (verifiedSlot.runId != parsedSlotBinding.runId
            || verifiedSlot.armId != AgentTaskRunner::evaluationArmId(configuration.arm)
            || verifiedSlot.holdoutId != parsedSlotBinding.holdoutId
            || verifiedSlot.slotId != parsedSlotBinding.slotId
            || verifiedSlot.caseId != parsedSlotBinding.caseId
            || verifiedSlot.repeat != parsedSlotBinding.repeat
            || verifiedSlot.manifestCommitmentSha256 != parsedSlotBinding.manifestCommitmentSha256
            || verifiedSlot.promptSha256 != parsedSlotBinding.promptSha256)
        {
            if (error) *error = QStringLiteral("Environment slot fields do not match the verified signed public pack.");
            return false;
        }

        // Bind the Agent only to verifier-derived values; the process environment
        // is treated as a request, never as evidence that the pack was verified.
        slotBinding.enabled = true;
        slotBinding.runId = verifiedSlot.runId;
        slotBinding.holdoutId = verifiedSlot.holdoutId;
        slotBinding.slotId = verifiedSlot.slotId;
        slotBinding.caseId = verifiedSlot.caseId;
        slotBinding.repeat = verifiedSlot.repeat;
        slotBinding.manifestCommitmentSha256 = verifiedSlot.manifestCommitmentSha256;
        slotBinding.promptSha256 = verifiedSlot.promptSha256;
    }
    configuration.slotBinding = slotBinding;
    if (!configuration.enabled) return true;

    auto store = std::make_shared<RAgentEvaluationCaptureStore>();
    QString storeError;
    if (!store->configure(configuration.capturePath, &storeError))
    {
        if (error) *error = QStringLiteral("Evaluation capture setup failed: %1").arg(storeError);
        return false;
    }
    this->evaluationArm = configuration.arm;
    this->evaluationSlotBinding = configuration.slotBinding;
    this->evaluationCaptureStore = std::move(store);
    return true;
#else
    if (armIsSet || captureIsSet)
    {
        if (error) *error = QStringLiteral("Evaluation capture requires an Agent-enabled build.");
        return false;
    }
    return true;
#endif
}

Application *Application::instance() noexcept
{
    return qobject_cast<Application*>(RApplication::instance());
}

const ApplicationSettings *Application::getApplicationSettings() const
{
    return qobject_cast<ApplicationSettings*>(this->applicationSettings);
}

ApplicationSettings *Application::getApplicationSettings()
{
    return qobject_cast<ApplicationSettings*>(this->applicationSettings);
}

const ActionList *Application::getActionList() const
{
    return qobject_cast<ActionList*>(this->actionList);
}

ActionList *Application::getActionList()
{
    return qobject_cast<ActionList*>(this->actionList);
}

const Session *Application::getSession() const
{
    return this->session;
}

Session *Application::getSession()
{
    return this->session;
}

const MainWindow *Application::getMainWindow() const
{
    return qobject_cast<MainWindow*>(this->mainWindow);
}

MainWindow *Application::getMainWindow()
{
    return qobject_cast<MainWindow*>(this->mainWindow);
}

void Application::initialize()
{
    this->initialize(true);
}

void Application::initialize(bool showWindow)
{
    // A non-visible initialization must remain non-interactive, otherwise
    // configuration prompts can block headless tools forever.
    const bool allowInteractivePrompts = showWindow;
    // Initialize action list
    this->actionList = new ActionList(Action::generateActionDefinitionList(),this);

    // Initialize session
    this->session = new Session(this);

    RJobSettings defaultJobSettings = RJob::getDefaultJobSettings();
    defaultJobSettings.registerEmitMutex(this->session->getMutex());
    defaultJobSettings.setNOmpThreads(this->getApplicationSettings()->getNThreads());
    RJob::setDefaultJobSettings(defaultJobSettings);

    // Prepare main window
#if RANGE_ENABLE_AGENT
    this->mainWindow = new MainWindow(this->session,this->cloudSessionManager,this->applicationSettings,
                                      this->getActionList(), nullptr, this->evaluationArm,
                                      this->evaluationCaptureStore, this->evaluationSlotBinding);
#else
    this->mainWindow = new MainWindow(this->session,this->cloudSessionManager,this->applicationSettings,this->getActionList());
#endif
    // The trusted-evidence CLI initializes without RApplication::onStarted(),
    // which normally creates the cloud connection handler.  The verifier does
    // not use cloud UI, so leave it absent in that headless path.
    if (this->cloudConnectionHandler)
        this->cloudConnectionHandler->setMessageBoxParentWidget(this->mainWindow);
    if (showWindow) this->mainWindow->show();

    this->userAppInfo.append(QString("Materials directory: \"%1\"").arg(this->getApplicationSettings()->getMaterialsDir()));

    this->updaterSourceDirs.append("materials");

    if (!this->filesToLoad.isEmpty())
    {
        // Load files
        try
        {
            Application::instance()->getSession()->readModels(this->filesToLoad);
        }
        catch (const RError &rError)
        {
            RLogger::warning("Failed to read model file(s). ERROR: %s\n",rError.getMessage().toUtf8().constData());
        }
    }
    else
    {
        // Load session
        QString sessionFileName = this->getApplicationSettings()->getSessionFileName();
        if (!sessionFileName.isEmpty() && RFileUtils::fileExists(sessionFileName))
        {
            try
            {
                Application::instance()->getSession()->read(sessionFileName);
            }
            catch (const RError &rError)
            {
                RLogger::warning("Failed to read the session file \'%s\'. ERROR: %s\n",sessionFileName.toUtf8().constData(),rError.getMessage().toUtf8().constData());
            }
        }
    }

    // Check help directory
    if (allowInteractivePrompts &&
        this->applicationSettings->findHelpDir() != this->applicationSettings->getHelpDir())
    {
        QString messageStr = tr("Default and configured help directories differ") + ":"
                           + QString("<ul>")
                           + "<li><tt>" + this->applicationSettings->findHelpDir() + "</tt></li>"
                           + "<li><tt>" + this->applicationSettings->getHelpDir() + "</tt></li>"
                           + QString("</ul>")
                           + "<strong>" + tr("Would you like to set it to default?") + "</strong>";
        if (RMessageBox::question(Application::instance()->getMainWindow(),tr("Unusual configuration"),messageStr) == RMessageBox::Yes)
        {
            this->applicationSettings->setHelpDir(this->applicationSettings->findHelpDir());
        }
    }
    // Check solver path
    if (allowInteractivePrompts &&
        this->getApplicationSettings()->getDefaultSolverPath() != this->getApplicationSettings()->getSolverPath())
    {
        QString messageStr = tr("Default and configured paths to solver executable differ") + ":"
                           + QString("<ul>")
                           + "<li><tt>" + this->getApplicationSettings()->getDefaultSolverPath() + "</tt></li>"
                           + "<li><tt>" + this->getApplicationSettings()->getSolverPath() + "</tt></li>"
                           + QString("</ul>")
                           + "<strong>" + tr("Would you like to set it to default?") + "</strong>";
        if (RMessageBox::question(Application::instance()->getMainWindow(),tr("Unusual configuration"),messageStr) == RMessageBox::Yes)
        {
            this->getApplicationSettings()->setSolverPath(this->getApplicationSettings()->getDefaultSolverPath());
        }
    }

    this->getActionList()->processAvailability();
}

void Application::finalize()
{
    // Store session
    QString sessionFileName = Application::instance()->getSession()->getFileName();
    if (sessionFileName.isEmpty())
    {
        sessionFileName = Application::instance()->getSession()->getDefaultFileName();
    }
    try
    {
        Application::instance()->getSession()->write(sessionFileName);
    }
    catch (const RError &error)
    {
        RLogger::error("Failed to write the session file \'%s\'. ERROR: %s\n",sessionFileName.toUtf8().constData(),error.getMessage().toUtf8().constData());
    }

    // Stop solver manager server.
    RLogger::info("Stoping solver task server\n");
    SolverManager::getInstance().stopServer();
}
