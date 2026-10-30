#ifndef APPLICATION_H
#define APPLICATION_H

#include <rgl_application.h>
#include <rgl_application_settings.h>

#include "application_settings.h"
#include "action_list.h"
#include "main_window.h"
#include "session.h"

#if RANGE_ENABLE_AGENT
#include "agent_task_runner.h"
#include <memory>
class RAgentEvaluationCaptureStore;
#endif

class Application : public RApplication
{

    Q_OBJECT

    protected:

        //! Session.
        Session *session = nullptr;
#if RANGE_ENABLE_AGENT
        AgentTaskRunner::EvaluationArm evaluationArm = AgentTaskRunner::EvaluationArm::Standard;
        std::shared_ptr<RAgentEvaluationCaptureStore> evaluationCaptureStore;
        AgentEvaluationSlotBinding evaluationSlotBinding;
#endif

    public:

#if RANGE_ENABLE_AGENT
        struct EvaluationCaptureConfiguration
        {
            bool enabled = false;
            AgentTaskRunner::EvaluationArm arm = AgentTaskRunner::EvaluationArm::Standard;
            QString capturePath;
            AgentEvaluationSlotBinding slotBinding;
        };

        //! Parse the opt-in environment pair without creating files or changing process state.
        static bool parseEvaluationCaptureConfiguration(bool armIsSet, const QString &armValue,
                                                        bool captureIsSet, const QString &capturePath,
                                                        EvaluationCaptureConfiguration *configuration,
                                                        QString *error = nullptr);
        //! Validate the syntax of the all-or-none v4 slot fields. This is not authentication.
        static bool parseEvaluationSlotBinding(bool runIdIsSet, const QString &runId,
                                               bool holdoutIdIsSet, const QString &holdoutId,
                                               bool slotIdIsSet, const QString &slotId,
                                               bool caseIdIsSet, const QString &caseId,
                                               bool repeatIsSet, const QString &repeatValue,
                                               bool manifestCommitmentIsSet, const QString &manifestCommitmentSha256,
                                               bool promptDigestIsSet, const QString &promptSha256,
                                               AgentTaskRunner::EvaluationArm arm,
                                               AgentEvaluationSlotBinding *binding,
                                               QString *error = nullptr);
#endif

        //! Constructor.
        explicit Application(int &argc, char **argv);

        //! Verify any v4 signed public slot before configuring capture or starting Agent widgets.
        bool configureEvaluationCaptureFromEnvironment(QString *error = nullptr);

        //! Return application instance.
        static Application *instance() noexcept;

        //! Return const pointer to application settings.
        const ApplicationSettings *getApplicationSettings() const;

        //! Return pointer to application settings.
        ApplicationSettings *getApplicationSettings();

        //! Return const pointer to action list.
        const ActionList *getActionList() const;

        //! Return pointer to action list.
        ActionList *getActionList();

        //! Return const reference to session.
        const Session *getSession() const;

        //! Return reference to session.
        Session *getSession();

        //! Return const pointer to main window.
        const MainWindow *getMainWindow() const;

        //! Return pointer to main window.
        MainWindow *getMainWindow();

        //! Initialize main window and other user objects.
        void initialize();
        //! Initialize the same window/session graph without showing a native window.
        void initialize(bool showWindow);

        //! Initialize main window and other user objects.
        void finalize();

};

#endif // APPLICATION_H
