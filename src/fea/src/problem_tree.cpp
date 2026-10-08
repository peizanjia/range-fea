#include "problem_tree.h"
#include "acoustic_setup_widget.h"
#include "application.h"
#include "mesh_setup_widget.h"
#include "modal_setup_widget.h"
#include "radiation_setup_widget.h"
#include "time_solver_setup_widget.h"
#include "model_command_host.h"
#include "value_line_edit.h"
#include <QLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QStringList>

typedef enum _ProblemTreeColumn
{
    PROBLEM_TREE_COLUMN_1 = 0,
    PROBLEM_TREE_N_COLUMNS
} ProblemTreeColumn;

ProblemTree::ProblemTree(QWidget *parent) :
    QTreeWidget(parent)
{
    this->setColumnCount(PROBLEM_TREE_N_COLUMNS);
    this->setSelectionMode(QAbstractItemView::NoSelection);
    this->setRootIsDecorated(false);
    QTreeWidgetItem* headerItem = new QTreeWidgetItem();
    headerItem->setText(PROBLEM_TREE_COLUMN_1,QString(tr("Problem setup")));
    this->setHeaderItem(headerItem);
    this->setHeaderHidden(true);

    this->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    this->populate();

    auto *session = Application::instance()->getSession();
    QObject::connect(session,
                     &Session::problemSelectionChanged,
                     this,
                     &ProblemTree::onProblemChanged);
    QObject::connect(session,
                     &Session::modelChanged,
                     this,
                     &ProblemTree::onModelChanged);
    QObject::connect(session,
                     &Session::modelSelectionChanged,
                     this,
                     &ProblemTree::onModelSelectionChanged);
    QObject::connect(session, &Session::runtimeDocumentRemoved, this,
                     [this](const QString &documentId) { this->clearDraftCacheForRuntimeDocument(documentId); });
    QObject::connect(session, &Session::documentDraftsCleared, this,
                     [this](const QString &documentId) {
                         this->clearDraftCacheForRuntimeDocument(documentId);
                         QTimer::singleShot(0, this, &ProblemTree::populate);
                     });


}

void ProblemTree::populate()
{
    this->clear();

    auto *session = Application::instance()->getSession();
    QList<uint> modelIDs = session->getSelectedModelIDs();
    auto createModalEditor = [this, session, &modelIDs](const RModalSetup &setup) {
        QStringList recoveredDraftRuntimeIds;
        for (const uint modelID : modelIDs)
        {
            const QJsonObject draft = session->documentProjectContext(modelID)
                                          .value(QStringLiteral("draft")).toObject();
            const QString status = draft.value(QStringLiteral("status")).toString();
            if (status == QStringLiteral("unknown"))
                session->clearNumericDraft(modelID, QStringLiteral("analysis.modal"),
                                           QStringLiteral("convergenceValue"));
            else if (status == QStringLiteral("pending")
                     && draft.value(QStringLiteral("scope")).toString()
                            == QStringLiteral("analysis.modal")
                     && draft.value(QStringLiteral("changedFields")).toArray()
                            .contains(QStringLiteral("convergenceValue")))
            {
                const QString runtimeId = session->modelCommandStamp(modelID).first;
                const QString key = this->numericDraftKey(runtimeId, QStringLiteral("analysis.modal"),
                                                          QStringLiteral("convergenceValue"));
                if (!this->numericDraftText.contains(key)) recoveredDraftRuntimeIds.append(runtimeId);
            }
        }
        auto *editor = new ModalSetupWidget(setup);
        if (!modelIDs.isEmpty())
        {
            const QString runtimeId = session->modelCommandStamp(modelIDs.first()).first;
            const QString key = this->numericDraftKey(runtimeId, QStringLiteral("analysis.modal"),
                                                      QStringLiteral("convergenceValue"));
            if (this->numericDraftText.contains(key))
            {
                auto *line = editor->findChild<ValueLineEdit *>(QStringLiteral("modalTolerance"));
                if (line) line->restoreDraftText(this->numericDraftText.value(key));
            }
        }
        if (!recoveredDraftRuntimeIds.isEmpty())
        {
            auto *notice = new QLabel(
                tr("Previous input could not be restored. Keep the accepted value or enter a new value."), editor);
            notice->setObjectName(QStringLiteral("recoveredModalDraftNotice"));
            notice->setWordWrap(true);
            editor->layout()->addWidget(notice);

            auto *keepAcceptedValue = new QPushButton(tr("Keep accepted value"), editor);
            keepAcceptedValue->setObjectName(QStringLiteral("keepAcceptedModalValue"));
            editor->layout()->addWidget(keepAcceptedValue);
            connect(keepAcceptedValue, &QPushButton::clicked, this,
                    [this, session, recoveredDraftRuntimeIds] {
                for (const QString &runtimeId : recoveredDraftRuntimeIds)
                {
                    const uint modelID = session->findModelByCommandIdentity(runtimeId);
                    if (modelID >= session->getNModels()) continue;
                    this->numericDraftText.remove(this->numericDraftKey(
                        runtimeId, QStringLiteral("analysis.modal"), QStringLiteral("convergenceValue")));
                    session->clearNumericDraft(modelID, QStringLiteral("analysis.modal"),
                                               QStringLiteral("convergenceValue"));
                }
                QTimer::singleShot(0, this, &ProblemTree::populate);
            });
        }
        connect(editor, &ModalSetupWidget::changed, this, &ProblemTree::onModalSetupChanged);
        connect(editor, &ModalSetupWidget::draftStateChanged, this, &ProblemTree::onModalDraftStateChanged);
        return editor;
    };

    if (modelIDs.size() == 0)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem(this);
        item->setText(PROBLEM_TREE_COLUMN_1,tr("No model is selected!"));
        item->setForeground(PROBLEM_TREE_COLUMN_1,QColor(Qt::red));
        return;
    }
    else if (modelIDs.size() > 1)
    {
        bool allModal = true;
        for (const uint id : modelIDs)
            allModal = allModal && (Application::instance()->getSession()->getModel(id).getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL);
        QTreeWidgetItem *item = new QTreeWidgetItem(this);
        item->setText(PROBLEM_TREE_COLUMN_1, allModal
            ? tr("Modal changes apply to all %1 selected models.").arg(modelIDs.size())
            : tr("Select models that all support modal analysis to edit them together."));
        if (allModal)
        {
            auto *row = new QTreeWidgetItem(this);
            auto *editor = createModalEditor(session->getModel(modelIDs[0]).getProblemSetup().getModalSetup());
            this->setItemWidget(row, PROBLEM_TREE_COLUMN_1, editor);
        }
        return;
    }

    const Model &rModel = Application::instance()->getSession()->getModel(modelIDs[0]);

    if (RProblem::getTimeSolverEnabled(rModel.getProblemTaskTree().getProblemTypeMask()))
    {
        QTreeWidgetItem *itemTimeSolver = new QTreeWidgetItem(this);
        TimeSolverSetupWidget *timeSolverWidget = new TimeSolverSetupWidget(rModel.getTimeSolver());
        this->setItemWidget(itemTimeSolver,PROBLEM_TREE_COLUMN_1,timeSolverWidget);
        QObject::connect(timeSolverWidget,&TimeSolverSetupWidget::changed,this,&ProblemTree::onTimeSolverChanged);
    }

    if (rModel.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL)
    {
        QTreeWidgetItem *modalSetup = new QTreeWidgetItem(this);
        ModalSetupWidget *modalSetupWidget = createModalEditor(rModel.getProblemSetup().getModalSetup());
        this->setItemWidget(modalSetup,PROBLEM_TREE_COLUMN_1,modalSetupWidget);
    }

    if (rModel.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_ACOUSTICS)
    {
        QTreeWidgetItem *acousticSetup = new QTreeWidgetItem(this);
        AcousticSetupWidget *acousticSetupWidget = new AcousticSetupWidget(rModel.getProblemSetup().getAcousticSetup());
        this->setItemWidget(acousticSetup,PROBLEM_TREE_COLUMN_1,acousticSetupWidget);
        QObject::connect(acousticSetupWidget,&AcousticSetupWidget::changed,this,&ProblemTree::onAcousticSetupChanged);
    }

    if (rModel.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_RADIATIVE_HEAT)
    {
        QString vfFileName = rModel.buildDataFileName(RViewFactorMatrix::getDefaultFileExtension(true),rModel.getTimeSolver().getEnabled());

        QTreeWidgetItem *itemRadiationSetup = new QTreeWidgetItem(this);
        RadiationSetupWidget *radiationSetupWidget = new RadiationSetupWidget(rModel.getProblemSetup().getRadiationSetup(),vfFileName);
        this->setItemWidget(itemRadiationSetup,PROBLEM_TREE_COLUMN_1,radiationSetupWidget);
        QObject::connect(radiationSetupWidget,&RadiationSetupWidget::changed,this,&ProblemTree::onRadiationSetupChanged);
    }

    if (rModel.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_MESH)
    {
        QTreeWidgetItem *meshSetup = new QTreeWidgetItem(this);
        MeshSetupWidget *meshSetupWidget = new MeshSetupWidget(rModel.getProblemSetup().getMeshSetup(),
                                                               RProblem::getVariableTypes(rModel.getProblemTaskTree().getProblemTypeMask()));
        this->setItemWidget(meshSetup,PROBLEM_TREE_COLUMN_1,meshSetupWidget);
        QObject::connect(meshSetupWidget,&MeshSetupWidget::changed,this,&ProblemTree::onMeshSetupChanged);
    }
}

void ProblemTree::onModelChanged(uint)
{
    this->populate();
}

void ProblemTree::onProblemChanged(uint)
{
    this->populate();
}

void ProblemTree::onModelSelectionChanged(uint)
{
    this->populate();
}

void ProblemTree::onModalDraftStateChanged(const QString &field, const QString &state, const QString &text)
{
    auto *session = Application::instance()->getSession();
    const QList<uint> modelIDs = session->getSelectedModelIDs();
    const QString scope = QStringLiteral("analysis.modal");
    for (const uint modelID : modelIDs)
    {
        const QString runtimeId = session->modelCommandStamp(modelID).first;
        if (runtimeId.isEmpty()) continue;
        const QString key = this->numericDraftKey(runtimeId, scope, field);
        if (state == QStringLiteral("none"))
        {
            this->numericDraftText.remove(key);
            session->clearNumericDraft(modelID, scope, field);
        }
        else
        {
            this->numericDraftText.insert(key, text);
            session->setNumericDraft(modelID, scope, field, state, text);
        }
    }
}

void ProblemTree::onTimeSolverChanged(const RTimeSolver &timeSolver)
{
    QList<uint> modelIDs = Application::instance()->getSession()->getSelectedModelIDs();

    for (int i=0;i<modelIDs.size();i++)
    {
        Application::instance()->getSession()->getModel(modelIDs[i]).setTimeSolver(timeSolver);
        Application::instance()->getSession()->setProblemChanged(modelIDs[i]);
    }
}

void ProblemTree::onModalSetupChanged(const RModalSetup &modalSetup)
{
    auto *session = Application::instance()->getSession();
    const QList<uint> modelIDs = session->getSelectedModelIDs();
    QString failure;
    if (!session->getModelCommandHost()->configureModal(modelIDs, modalSetup, &failure))
    {
        QMessageBox::warning(this, tr("Modal configuration"), failure);
        QTimer::singleShot(0, this, &ProblemTree::populate);
        return;
    }
}

QString ProblemTree::numericDraftKey(const QString &runtimeDocumentId, const QString &scope,
                                    const QString &field) const
{
    return runtimeDocumentId + QChar(0x1f) + scope + QChar(0x1f) + field;
}

void ProblemTree::clearDraftCacheForRuntimeDocument(const QString &runtimeDocumentId)
{
    const QString prefix = runtimeDocumentId + QChar(0x1f);
    const QStringList keys = this->numericDraftText.keys();
    for (const QString &key : keys)
        if (key.startsWith(prefix)) this->numericDraftText.remove(key);
}

void ProblemTree::onAcousticSetupChanged(const RAcousticSetup &acousticSetup)
{
    QList<uint> modelIDs = Application::instance()->getSession()->getSelectedModelIDs();

    for (int i=0;i<modelIDs.size();i++)
    {
        Application::instance()->getSession()->getModel(modelIDs[i]).getProblemSetup().setAcousticSetup(acousticSetup);
        Application::instance()->getSession()->setProblemChanged(modelIDs[i]);
    }
}

void ProblemTree::onRadiationSetupChanged(const RRadiationSetup &radiationSetup)
{
    QList<uint> modelIDs = Application::instance()->getSession()->getSelectedModelIDs();

    for (int i=0;i<modelIDs.size();i++)
    {
        Application::instance()->getSession()->getModel(modelIDs[i]).getProblemSetup().setRadiationSetup(radiationSetup);
        Application::instance()->getSession()->setProblemChanged(modelIDs[i]);
    }
}

void ProblemTree::onMeshSetupChanged(const RMeshSetup &meshSetup)
{
    QList<uint> modelIDs = Application::instance()->getSession()->getSelectedModelIDs();

    for (int i=0;i<modelIDs.size();i++)
    {
        Application::instance()->getSession()->getModel(modelIDs[i]).getProblemSetup().setMeshSetup(meshSetup);
        Application::instance()->getSession()->setProblemChanged(modelIDs[i]);
    }
}
