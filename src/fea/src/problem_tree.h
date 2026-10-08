#ifndef PROBLEM_TREE_H
#define PROBLEM_TREE_H

#include <QTreeWidget>
#include <QHash>

#include <rml_problem.h>

#include <rgl_file_chooser_button.h>

class ProblemTree : public QTreeWidget
{

    Q_OBJECT

    protected:

        //! Pending editor text is intentionally kept only in this GUI instance.
        QHash<QString, QString> numericDraftText;

        QString numericDraftKey(const QString &runtimeDocumentId, const QString &scope,
                                const QString &field) const;
        void clearDraftCacheForRuntimeDocument(const QString &runtimeDocumentId);

    public:

        //! Constructor.
        explicit ProblemTree(QWidget *parent = nullptr);

    protected slots:

        //! Populate.
        void populate();

        //! Model has changed.
        void onModelChanged(uint);

        //! Problem has changed.
        void onProblemChanged(uint);

        //! Model selection has changed.
        void onModelSelectionChanged(uint);

        //! Numeric modal editor draft changed.
        void onModalDraftStateChanged(const QString &field, const QString &state, const QString &text);

        //! Time solver has changed.
        void onTimeSolverChanged(const RTimeSolver &timeSolver);

        //! Modal setup has changed.
        void onModalSetupChanged(const RModalSetup &modalSetup);

        //! Acoustic setup has changed.
        void onAcousticSetupChanged(const RAcousticSetup &acousticSetup);

        //! Radiation setup has changed.
        void onRadiationSetupChanged(const RRadiationSetup &radiationSetup);

        //! Mesh setup has changed.
        void onMeshSetupChanged(const RMeshSetup &meshSetup);

};

#endif // PROBLEM_TREE_H
