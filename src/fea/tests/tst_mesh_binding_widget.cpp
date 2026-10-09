#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScreen>
#include <QScopeGuard>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <array>
#include <cmath>
#include <rmodel_command_service.h>
#include <rmesh_rebind_service.h>
#include "application.h"
#include "central_tab_widget.h"
#include "geometry_command_widget.h"
#include "main_window.h"
#include "material_manager_tree.h"
#include "mesh_binding_widget.h"
#include "mesh_command_widget.h"
#include "mesh_job_controller.h"
#include "model_command_host.h"
#include "model_io.h"
#include "model_tree.h"
#include "results_variable_selector.h"
#include "results_variable_tree.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_setup_checker.h"
#include "sub_window.h"
#include "value_scale.h"
#include "workflow_process.h"

namespace {
const RMaterial &bindingMaterial()
{
    static const auto material = [] {
        auto value = RMaterial::generateDefault();
        value.setName("Mesh binding GUI fixture");
        return value;
    }();
    return material;
}
Model shellFixture(bool withVolume = false)
{
    Model model;
    model.setName("WF14 GUI tetrahedron");
    model.setNNodes(4); model.setNSurfaces(1);
    model.getNode(0).set(0,0,0); model.getNode(1).set(1,0,0);
    model.getNode(2).set(0,1,0); model.getNode(3).set(0,0,1);
    model.getSurface(0).setMaterial(bindingMaterial());
    const uint faces[4][3]{{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (uint i=0; i<4; ++i) {
        model.setNodePhysicalId(i,101+10*i);
        RElement face(R_ELEMENT_TRI1);
        for (uint j=0; j<3; ++j) face.setNodeId(j,faces[i][j]);
        model.addElement(face,true,0); model.setElementPhysicalId(i,501+10*i);
    }
    if (withVolume) {
        model.setNVolumes(1); model.getVolume(0).setMaterial(bindingMaterial());
        RElement tetra(R_ELEMENT_TETRA1);
        for (uint j=0; j<4; ++j) tetra.setNodeId(j,j);
        model.addElement(tetra,true,0);
        // This fixture represents a saved UNBOUND candidate, whose inherited
        // environment conditions are explicitly absent before it is saved.
        model.clearBoundaryConditions(); model.clearInitialConditions(); model.clearResults();
        for(uint group=0;group<model.getNElementGroups();++group)
            while(model.getElementGroupPtr(group)->getNEnvironmentConditions())
                model.getElementGroupPtr(group)->removeEnvironmentCondition(0u);
    }
    model.setProblemTaskTree(RProblemTaskItem(withVolume ? R_PROBLEM_NONE : R_PROBLEM_STRESS));
    return model;
}
QJsonObject request(const QString &operation, const QJsonObject &parameters)
{
    return {{"apiVersion","range.host/v1"},{"requestId",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"operation",operation},{"parameters",parameters}};
}
QJsonObject propose(Session *session, uint modelId, const QString &command, const QJsonObject &arguments = {})
{
    auto *host = session->getModelCommandHost();
    const auto stamp = session->modelCommandStamp(modelId);
    return host->handle(request("command.preview",{{"target",QJsonObject{
        {"documentId",stamp.first},{"sessionEpoch",host->sessionEpoch()}}},
        {"expectedDocumentRevision",double(stamp.second)},{"command",command},
        {"commandVersion",1},{"arguments",arguments}}));
}
QJsonObject execute(ModelCommandHost *host, const QString &previewId)
{
    return host->handle(request("command.execute",{{"previewId",previewId},
        {"idempotencyKey",QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
}
QJsonObject bindingReply(QWidget &widget)
{
    return QJsonDocument::fromJson(widget.findChild<QPlainTextEdit *>("meshBindingDetails")
                                  ->toPlainText().toUtf8()).object();
}
QByteArray bindingDiagnostic(QWidget &widget)
{
    const auto *nodes = widget.findChild<QTableWidget *>("meshBindingNodes");
    const auto reply=bindingReply(widget);
    return QJsonDocument(QJsonObject{{"replyStatus",reply.value("status")},
        {"replyCode",reply.value("code")},{"replyMessage",reply.value("message")},
        {"status",widget.findChild<QLabel *>("meshBindingStatus")->text()},
        {"contextGate",nodes->property("bindingContextGate").toString()},
        {"selectedModels",QJsonArray::fromVariantList(nodes->property("bindingSelectedModels").toList())}})
            .toJson(QJsonDocument::Compact);
}
void clickButton(QPushButton *button)
{
    // Binding controls live below the node-ID table in a scroll viewport.
    // Physical Windows input must hit a visible button, just like offscreen input.
    for (QWidget *parent = button->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent))
            scroll->ensureWidgetVisible(button);
    QCoreApplication::processEvents();
    QTest::mouseClick(button,Qt::LeftButton);
}
// Never reuse source indices or assume TetGen retained the source's IDs/order.
std::array<uint,4> cornerIds(const Model &model)
{
    std::array<uint,4> ids{};
    const double xyz[4][3]{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    for (uint n=0; n<model.getNNodes(); ++n) {
        const auto &node = model.getNode(n);
        for (uint corner=0; corner<4; ++corner)
            if (std::abs(node.getX()-xyz[corner][0])<1e-12 &&
                std::abs(node.getY()-xyz[corner][1])<1e-12 &&
                std::abs(node.getZ()-xyz[corner][2])<1e-12)
                ids[corner] = model.getNodePhysicalId(n);
    }
    return ids;
}
void fillBinding(QWidget &widget, const std::array<uint,4> &ids)
{
    widget.findChild<QLineEdit *>("meshBindingYoung")->setText("1000");
    widget.findChild<QLineEdit *>("meshBindingPoisson")->setText("0.25");
    widget.findChild<QLineEdit *>("meshBindingDensity")->setText("1");
    const auto row = [&](const char *name, uint id, const QStringList &xyz) {
        clickButton(widget.findChild<QPushButton *>(QString::fromLatin1(name)+"Add"));
        auto *table = widget.findChild<QTableWidget *>(name);
        const int index = table->rowCount()-1;
        table->item(index,0)->setText(QString::number(id));
        for (int c=0; c<3; ++c) table->item(index,c+1)->setText(xyz[c]);
    };
    row("meshBindingSupports",ids[0],{"0","0","0"});
    row("meshBindingSupports",ids[1],{"","0","0"});
    row("meshBindingSupports",ids[2],{"","","0"});
    row("meshBindingForces",ids[3],{"0","0","-1"});
}
void refreshMaterials()
{
    const auto directory = Application::instance()->getApplicationSettings()->getMaterialsDir();
    for (auto *tree : Application::instance()->getMainWindow()->findChildren<MaterialManagerTree *>())
        QMetaObject::invokeMethod(tree,"onDirectoryChanged",Qt::DirectConnection,Q_ARG(QString,directory));
}
bool sameResultValues(const RModel &left,const RModel &right)
{
    if (left.getNVariables()!=right.getNVariables()) return false;
    for (uint i=0; i<left.getNVariables(); ++i) {
        const auto &a=left.getVariable(i); const auto &b=right.getVariable(i);
        if (a.getType()!=b.getType() || a.getApplyType()!=b.getApplyType()
            || a.getName()!=b.getName() || a.getUnits()!=b.getUnits()
            || a.getNVectors()!=b.getNVectors() || a.getNValues()!=b.getNValues()) return false;
        for (uint vector=0; vector<a.getNVectors(); ++vector)
            for (uint value=0; value<a.getNValues(); ++value)
                if (a.getValue(vector,value)!=b.getValue(vector,value)) return false;
    }
    return true;
}
QString materialPath()
{
    return QDir(Application::instance()->getApplicationSettings()->getMaterialsDir()).filePath(
        bindingMaterial().getID().toString(QUuid::WithoutBraces)+"."+RMaterial::getDefaultFileExtension());
}
}

class TestMeshBindingWidget : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(QStandardPaths::isTestModeEnabled());
        QVERIFY(QDir().mkpath(QFileInfo(materialPath()).absolutePath()));
        bindingMaterial().write(materialPath()); QVERIFY(QFileInfo::exists(materialPath()));
        refreshMaterials();
        for (auto *widget : Application::instance()->getMainWindow()->findChildren<MeshCommandWidget *>())
            widget->setController(nullptr);
        Application::instance()->getSession()->getModelCommandHost()->meshController()->shutdown();
        if (qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI")) {
            QCOMPARE(QApplication::platformName(),QString("windows"));
            QVERIFY(!qEnvironmentVariable("RANGE_WF14_SCREENSHOT_DIR").isEmpty());
            QVERIFY(QDir().mkpath(qEnvironmentVariable("RANGE_WF14_SCREENSHOT_DIR")));
            Application::instance()->getMainWindow()->show();
        }
    }
    void cleanup()
    {
        auto &manager = SolverManager::getInstance();
        if (manager.getNRunning() || manager.getNWaiting()) manager.stopRunningTasks();
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,15000);
        Application::instance()->getSession()->clear();
    }
    void cleanupTestCase()
    {
        QVERIFY(QFile::remove(materialPath())); refreshMaterials();
        for (auto *widget : Application::instance()->getMainWindow()->findChildren<MeshCommandWidget *>())
            widget->setController(Application::instance()->getSession()->getModelCommandHost()->meshController());
    }
    void materialFreeStaticBoundaryRoles_data()
    {
        QTest::addColumn<QString>("mutation");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("verified-static-boundary") << QString() << true;
        for (const auto *name : {"finite-thickness","axisymmetric","point-volume","foreign-point-node",
                                "duplicate-boundary-face","inverted-tetrahedron","missing-solid-material",
                                "coupled-task","dynamics","point-pressure","surface-pressure",
                                "entity-mapping","multiple-volumes"})
            QTest::newRow(name) << QString::fromLatin1(name) << false;
    }
    void materialFreeStaticBoundaryRoles()
    {
        QFETCH(QString,mutation);
        QFETCH(bool,accepted);
        // A separate analytic fixture is bound through the production service;
        // the loaded-document and worker paths below retain their actual state.
        RModel source = shellFixture(true);
        source.setProblemSetup(RProblemSetup()); source.setTimeSolver(RTimeSolver());
        for (uint g=0; g<source.getNElementGroups(); ++g)
            while (source.getElementGroupPtr(g)->getNEnvironmentConditions())
                source.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
        const auto null = QJsonValue(QJsonValue::Null);
        const QJsonObject arguments{{"unitSI",true},{"staticStress",true},
            {"material",QJsonObject{{"youngModulusPa",1000},{"poissonRatio",0.25},{"densityKgM3",1}}},
            {"constraints",QJsonArray{
                QJsonObject{{"nodePhysicalId",101},{"values",QJsonArray{0,0,0}}},
                QJsonObject{{"nodePhysicalId",111},{"values",QJsonArray{null,0,0}}},
                QJsonObject{{"nodePhysicalId",121},{"values",QJsonArray{null,null,0}}}}},
            {"loads",QJsonArray{QJsonObject{{"nodePhysicalId",131},{"forceN",QJsonArray{0,0,-1}}}}}};
        const auto bound = RMeshRebindService::preview(source,arguments);
        QVERIFY2(bound.valid,qPrintable(bound.message));
        RModel model = bound.candidate;
        if (mutation=="finite-thickness") model.getSurface(0).setThickness(0.01);
        if (mutation=="axisymmetric") model.getSurface(0).setStructuralFormulation(RElementFormulation::Axisymmetric);
        if (mutation=="point-volume") model.getPoint(0).setVolume(0.01);
        if (mutation=="foreign-point-node") {
            model.setNNodes(5); model.getNode(4).set(2,2,2);
            model.getElement(model.getPoint(0).get(0)).setNodeId(0,4);
        }
        if (mutation=="duplicate-boundary-face")
            model.getElement(model.getSurface(0).get(1)) = model.getElement(model.getSurface(0).get(0));
        if (mutation=="inverted-tetrahedron") {
            auto &tetra = model.getElement(model.getVolume(0).get(0));
            const uint first = tetra.getNodeId(0); tetra.setNodeId(0,tetra.getNodeId(1)); tetra.setNodeId(1,first);
        }
        if (mutation=="missing-solid-material") model.getVolume(0).setMaterial(RMaterial());
        if (mutation=="coupled-task")
            model.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS|R_PROBLEM_HEAT));
        if (mutation=="dynamics") model.getProblemSetup().getStructuralDynamicsSetup().setEnabled(true);
        if (mutation=="point-pressure") {
            model.getPoint(0).removeAllBoundaryConditions();
            model.getPoint(0).addBoundaryCondition(RBoundaryCondition(R_BOUNDARY_CONDITION_PRESSURE));
        }
        if (mutation=="surface-pressure")
            model.getSurface(0).addBoundaryCondition(RBoundaryCondition(R_BOUNDARY_CONDITION_PRESSURE));
        if (mutation=="entity-mapping") {
            auto data = model.getSurface(0).getData().getVariableData();
            data[R_VARIABLE_DISPLACEMENT] = R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR;
            model.getSurface(0).getData().setVariableData(data);
        }
        if (mutation=="multiple-volumes") model.setNVolumes(2);
        const auto state = RModelCommandService::stateFingerprint(model);
        QCOMPARE(RMeshRebindService::hasMaterialFreeStaticBoundaryRoles(model),accepted);
        QCOMPARE(RModelCommandService::stateFingerprint(model),state);
        QStringList warnings,errors;
        SolverSetupChecker(model).perform(warnings,errors);
        QCOMPARE(errors.isEmpty(),accepted);
        if (accepted) QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join("\n")));
    }
    void workerAcceptanceExplicitBindingHistoryAndNativeResult_data()
    {
        QTest::addColumn<QString>("publicationMutation");
        QTest::newRow("verified-artifact") << QString();
        for (const auto *name:{"render-and-reentrant-open","silent-variable-change","result-closed",
                               "workspace-changed","artifact-changed","final-event-variable-change",
                               "normalization-revision-variable-change","final-event-variable-change-and-nested-open"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void workerAcceptanceExplicitBindingHistoryAndNativeResult()
    {
        QFETCH(QString,publicationMutation);
        QTemporaryDir workspace,callbackWorkspace;
        QVERIFY(workspace.isValid());
        QVERIFY(callbackWorkspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        QJsonObject geometryControlEvidence;
        if (publicationMutation.isEmpty()) {
            if (qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI")) workspace.setAutoRemove(false);
            geometryControlPrelude(session,workspace.path(),geometryControlEvidence);
            QVERIFY2(!geometryControlEvidence.isEmpty(),"Actual GUI geometry/native control prelude did not complete");
            QCOMPARE(session->getNModels(),0u);
        }
        session->addModel(shellFixture()); session->setModelSelected(0,true);
        const auto sourceStamp = session->modelCommandStamp(0);
        const auto sourceProject = session->documentProjectContext(0);
        const auto sourceHash = RModelCommandService::stateFingerprint(session->getModel(0));
        MeshJobController controller(session);
        MeshCommandWidget widget; widget.setSession(session); widget.setController(&controller);
        widget.resize(800,600); widget.show();
        auto *generatePreview = widget.findChild<QPushButton *>("meshPreviewGenerate");
        auto *generateApply = widget.findChild<QPushButton *>("meshApproveGenerate");
        QVERIFY(generatePreview && generateApply);
        clickButton(generatePreview); QVERIFY(generateApply->isEnabled());
        widget.findChild<QLineEdit *>("meshMaxVolume")->setText("0.07");
        QVERIFY(!generateApply->isEnabled());
        clickButton(generateApply); QVERIFY(controller.jobs().isEmpty());
        widget.findChild<QLineEdit *>("meshMaxVolume")->setText("0.08");
        clickButton(generatePreview); QVERIFY(generateApply->isEnabled());
        clickButton(generateApply);
        QCOMPARE(controller.jobs().size(),1);
        const QString attempt = controller.jobs().first().toObject().value("attemptId").toString();
        QVERIFY(!attempt.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(controller.query(attempt).value("result").toObject().value("status").toString(),
                                 QString("candidate_ready"),15000);
        QCOMPARE(session->getNModels(),1u);
        auto *jobs = widget.findChild<QTableWidget *>("meshJobs");
        QTRY_COMPARE(jobs->rowCount(),1); jobs->setCurrentCell(0,0); jobs->selectRow(0);
        auto *acceptPreview = widget.findChild<QPushButton *>("meshPreviewAccept");
        auto *acceptApply = widget.findChild<QPushButton *>("meshApproveAccept");
        QTRY_VERIFY(acceptPreview->isEnabled()); QVERIFY(!acceptApply->isEnabled());
        clickButton(acceptPreview); QVERIFY(acceptApply->isEnabled());
        QCOMPARE(session->getNModels(),1u);
        clickButton(acceptApply);
        QCOMPARE(session->getNModels(),2u);
        QCOMPARE(controller.query(attempt).value("result").toObject().value("status").toString(),QString("accepted"));
        QCOMPARE(session->modelCommandStamp(0),sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceHash);
        const auto candidateStamp = session->modelCommandStamp(1);
        QVERIFY(candidateStamp.first!=sourceStamp.first);
        const auto candidateProject = session->documentProjectContext(1);
        QVERIFY(!candidateProject.value("persistentDocumentId").toString().isEmpty());
        QVERIFY(candidateProject.value("persistentDocumentId")!=sourceProject.value("persistentDocumentId"));
        QCOMPARE(session->getModel(1).getProblemTaskTree().getProblemTypeMask(),int(R_PROBLEM_NONE));
        QCOMPARE(session->getModel(1).getNVariables(),0u);
        QVERIFY(!session->getModel(1).getLoadedSourceBinding());
        const auto unboundRun = propose(session,1,"analysis.run",{{"warningsAccepted",true}});
        QCOMPARE(unboundRun.value("code").toString(),QString("SOLVER_VALIDATION_FAILED"));
        bool missingTask = false;
        for (const auto &failure : unboundRun.value("validation").toObject().value("errors").toArray())
            missingTask |= failure.toString().contains("No solver problem task");
        QVERIFY(missingTask);
        QCOMPARE(propose(session,1,"analysis.run",{{"warningsAccepted",true},{"solverParameters",QJsonObject{}}})
                 .value("code").toString(),QString("INVALID_PARAMETERS"));
        // Select through the same tree route as the user. Setting Session
        // entity flags separately leaves selected tree parents authoritative
        // and can synchronously reselect the source alongside the candidate.
        auto *window=Application::instance()->getMainWindow();
        auto *modelTree=window->findChild<ModelTree *>(); QVERIFY(modelTree);
        QTRY_VERIFY(modelTree->topLevelItemCount()>1);
        auto *candidateRow=modelTree->topLevelItem(1); QVERIFY(candidateRow);
        if(qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI")) {
            auto *modelDock=window->findChild<QDockWidget *>("modelDock"); QVERIFY(modelDock);
            modelDock->show(); modelDock->raise(); window->raise();
            modelTree->scrollToItem(candidateRow); QCoreApplication::processEvents();
            QVERIFY(!modelTree->visualItemRect(candidateRow).isEmpty());
            QTest::mouseClick(modelTree->viewport(),Qt::LeftButton,Qt::NoModifier,
                             modelTree->visualItemRect(candidateRow).center());
            widget.raise();
        } else {
            modelTree->setCurrentItem(candidateRow,0,QItemSelectionModel::ClearAndSelect);
        }
        QTRY_COMPARE(session->getSelectedModelIDs(),QList<uint>{1});
        auto *nodeList = widget.findChild<QTableWidget *>("meshBindingNodes");
        QVERIFY(nodeList);
        QCOMPARE(nodeList->editTriggers(),QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers));
        const auto nodeContextDiagnostic=[&] {
            QJsonArray selected;
            for(const auto id:session->getSelectedModelIDs()) selected.append(int(id));
            return QJsonDocument(QJsonObject{{"selectedModels",selected},
                {"lastSelectedModels",QJsonArray::fromVariantList(nodeList->property("bindingSelectedModels").toList())},
                {"gate",nodeList->property("bindingContextGate").toString()},
                {"polls",nodeList->property("bindingContextPolls").toInt()},
                {"refreshes",nodeList->property("bindingNodeRefreshes").toInt()},
                {"context",QJsonDocument::fromJson(nodeList->property("bindingContextReply").toString().toUtf8()).object()}})
                    .toJson(QJsonDocument::Compact);
        };
        QTRY_VERIFY2(nodeList->rowCount()==int(session->getModel(1).getNNodes()),nodeContextDiagnostic().constData());
        for (int row=0; row<nodeList->rowCount(); ++row)
            QCOMPARE(nodeList->item(row,0)->text().toUInt(),session->getModel(1).getNodePhysicalId(uint(row)));
        const auto ids = cornerIds(session->getModel(1));
        for (uint id : ids) {
            QVERIFY(id>0);
            for (uint old=0; old<4; ++old) QVERIFY(id!=session->getModel(0).getNodePhysicalId(old));
        }
        fillBinding(widget,ids);
        auto *preview = widget.findChild<QPushButton *>("meshBindingPreview");
        auto *apply = widget.findChild<QPushButton *>("meshBindingApply");
        QTRY_VERIFY(preview->isEnabled());
        const auto unboundHash = RModelCommandService::stateFingerprint(session->getModel(1));
        clickButton(preview);
        QCOMPARE(bindingReply(widget).value("status").toString(),QString("preview"));
        QVERIFY(apply->isEnabled());
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),unboundHash);
        widget.findChild<QLineEdit *>("meshBindingYoung")->setText("1001");
        QVERIFY(!apply->isEnabled());
        clickButton(apply);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),unboundHash);
        widget.findChild<QLineEdit *>("meshBindingYoung")->setText("1000");
        clickButton(preview); QVERIFY2(apply->isEnabled(),bindingDiagnostic(widget).constData());
        capture(widget,"01-binding-preview");
        clickButton(apply);
        QCOMPARE(bindingReply(widget).value("status").toString(),QString("accepted"));
        QCOMPARE(session->getModel(1).getProblemTaskTree().getProblemTypeMask(),int(R_PROBLEM_STRESS));
        QCOMPARE(session->getModel(1).getNPoints(),4u);
        QCOMPARE(session->getModel(1).getUndoStackSize(),1u);
        const auto boundHash = RModelCommandService::stateFingerprint(session->getModel(1));
        for (const auto &command : {QString("document.undo"),QString("document.redo")}) {
            const auto p = propose(session,1,command);
            QCOMPARE(p.value("status").toString(),QString("preview"));
            const auto id = p.value("result").toObject().value("previewId").toString();
            QVERIFY(host->approvePreview(id));
            QCOMPARE(execute(host,id).value("status").toString(),QString("accepted"));
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),
                     command=="document.undo" ? unboundHash : boundHash);
            QVERIFY(!session->getModel(1).getLoadedSourceBinding());
        }
        // A real native child uses the host's frozen project snapshot.
        const auto run = propose(session,1,"analysis.run",{{"warningsAccepted",true}});
        QVERIFY2(run.value("status")==QJsonValue("preview"),qPrintable(QJsonDocument(run).toJson()));
        const auto runId = run.value("result").toObject().value("previewId").toString();
        QCOMPARE(execute(host,runId).value("status").toString(),QString("rejected"));
        QVERIFY(host->approvePreview(runId));
        const auto submitted = execute(host,runId);
        QVERIFY2(submitted.value("status")==QJsonValue("accepted"),qPrintable(QJsonDocument(submitted).toJson()));
        const auto jobId = submitted.value("result").toObject().value("jobId").toString();
        QVERIFY(!jobId.isEmpty());
        auto &manager = SolverManager::getInstance();
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value("state").toString(),QString("succeeded"),30000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,15000);
        const auto job = manager.jobQuery(jobId);
        QVERIFY(job.value("processStarted").toBool()); QVERIFY(job.value("processExited").toBool());
        QCOMPARE(job.value("processExitCode").toInt(-1),0);
        QVERIFY(job.value("resultReadbackComplete").toBool());
        QVERIFY(job.value("resultArtifactVerified").toBool());
        // Normal GUI runs publish verified results into the still-current
        // bound source. Opening an independent result view must preserve it.
        QVERIFY(session->getModel(1).getNVariables()>0);
        const auto solvedHash = RModelCommandService::stateFingerprint(session->getModel(1));
        const Model solvedSource=session->getModel(1);
        const auto sourceBindingBefore=session->getModel(0).getLoadedSourceBinding();
        const auto boundBindingBefore=session->getModel(1).getLoadedSourceBinding();
        const auto sourceFileBefore=session->getModel(0).getFileName();
        const auto boundFileBefore=session->getModel(1).getFileName();
        const auto sourceSnapshotBefore=job.value("projectSnapshotRef");
        const auto savedSourceHashBefore=session->documentProjectContext(1).value("savedArtifactHash");
        const QString artifactPath=job.value("resultArtifactRef").toString();
        Model artifactModel; RProjectCatalog::SourceBinding artifactBinding;
        ModelIO::open(artifactModel,artifactPath,&artifactBinding);
        QVERIFY(artifactModel.getNVariables()>0);
        if (publicationMutation=="normalization-revision-variable-change")
            QVERIFY2(artifactModel.hasNativeReadNormalization(),"Actual native artifact must trigger its normalization revision notification");
        const QJsonObject openRequest=request("native.result.open",{{"jobId",jobId},
            {"expectedSHA",job.value("resultArtifactSha256")}});
        bool callbackSeen=false;
        bool callbackRoleValid=false;
        QString callbackError;
        QJsonObject reentrantReply;
        QByteArray artifactBytes;
        if (publicationMutation=="artifact-changed") {
            QFile artifactFile(artifactBinding.resolvedSourcePath);
            QVERIFY(artifactFile.open(QIODevice::ReadOnly)); artifactBytes=artifactFile.readAll();
            QVERIFY(!artifactBytes.isEmpty());
        }
        const auto publicationCallback=connect(session,&Session::modelAdded,this,[&](uint modelId) {
            callbackSeen=true;
            callbackRoleValid=session->isVerifiedArtifactPublication(modelId)
                              && !session->isVerifiedMeshPublication(modelId);
            if (publicationMutation=="render-and-reentrant-open") {
                session->getModel(modelId).setSelected(false);
                reentrantReply=host->handle(openRequest);
            } else if (publicationMutation=="silent-variable-change") {
                auto &variable=session->getModel(modelId).getVariable(0);
                variable.setValue(0,0,variable.getValue(0,0)+1);
            } else if (publicationMutation=="result-closed") {
                session->removeModel(modelId);
            } else if (publicationMutation=="workspace-changed") {
                session->clear();
                if (!session->openProject(callbackWorkspace.path(),&callbackError))
                    callbackError="Workspace callback failed: "+callbackError;
            } else if (publicationMutation=="artifact-changed") {
                QFile artifactFile(artifactBinding.resolvedSourcePath);
                if (!artifactFile.open(QIODevice::WriteOnly) || artifactFile.write("changed")!=7)
                    callbackError="Artifact mutation failed";
            }
        });
        const auto finalPublicationCallback=connect(host,&ModelCommandHost::eventPublished,this,[&](const QJsonObject &event) {
            if ((publicationMutation!="final-event-variable-change"
                 && publicationMutation!="final-event-variable-change-and-nested-open")
                || event.value("type")!="document.result_opened") return;
            const uint modelId=session->findModelByCommandIdentity(event.value("documentId").toString());
            if (modelId>=session->getNModels()) { callbackError="Final-event result identity missing"; return; }
            auto &variable=session->getModel(modelId).getVariable(0);
            variable.setValue(0,0,variable.getValue(0,0)+1);
            if (publicationMutation=="final-event-variable-change-and-nested-open")
                reentrantReply=host->handle(openRequest);
        });
        bool normalizationCallbackSeen=false;
        const auto normalizationCallback=connect(session,&Session::documentRevisionChanged,this,
            [&](const QString &documentId,quint64 revision) {
                if (publicationMutation!="normalization-revision-variable-change") return;
                const uint modelId=session->findModelByCommandIdentity(documentId);
                if (modelId>=session->getNModels() || !session->isVerifiedArtifactPublication(modelId)
                    || session->isVerifiedMeshPublication(modelId)) return;
                normalizationCallbackSeen=true;
                callbackRoleValid=true;
                if (revision!=1) callbackError="Normalization must publish precisely the expected initial revision increment";
                auto &variable=session->getModel(modelId).getVariable(0);
                // Intentionally bypass notifications to test the values digest.
                variable.setValue(0,0,variable.getValue(0,0)+1);
            });
        QStringList unexpectedResultDialogs;
        QTimer resultModalWatchdog;
        resultModalWatchdog.setInterval(1000);
        connect(&resultModalWatchdog,&QTimer::timeout,&resultModalWatchdog,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            QStringList texts;
            if (auto *message=qobject_cast<QMessageBox *>(dialog))
                texts << message->text() << message->informativeText() << message->detailedText();
            for (auto *label:dialog->findChildren<QLabel *>()) texts << label->text();
            const QString diagnostic=QString("class=%1 title=%2 text=%3")
                .arg(QString::fromLatin1(dialog->metaObject()->className()),dialog->windowTitle(),texts.join(" | "));
            unexpectedResultDialogs.append(diagnostic);
            qWarning().noquote() << "Unexpected native.result.open modal:" << diagnostic;
            // Reject and fail with evidence; never approve an unexpected import.
            dialog->reject();
        });
        resultModalWatchdog.start();
        const auto opened = host->handle(openRequest);
        resultModalWatchdog.stop();
        disconnect(publicationCallback); disconnect(finalPublicationCallback); disconnect(normalizationCallback);
        QVERIFY2(unexpectedResultDialogs.isEmpty(),qPrintable(unexpectedResultDialogs.join("\n")));
        QVERIFY2(callbackError.isEmpty(),qPrintable(callbackError));
        if (publicationMutation=="normalization-revision-variable-change") {
            QVERIFY(normalizationCallbackSeen); QVERIFY(!callbackSeen);
        } else QVERIFY(callbackSeen);
        QVERIFY(callbackRoleValid);
        if (publicationMutation=="final-event-variable-change-and-nested-open")
            QCOMPARE(reentrantReply.value("code").toString(),QString("RESULT_OPEN_IN_PROGRESS"));
        if (publicationMutation=="artifact-changed") {
            QFile artifactFile(artifactBinding.resolvedSourcePath);
            QVERIFY(artifactFile.open(QIODevice::WriteOnly));
            QCOMPARE(artifactFile.write(artifactBytes),qint64(artifactBytes.size())); artifactFile.close();
        }
        if (publicationMutation!="workspace-changed") {
            QCOMPARE(session->modelCommandStamp(0),sourceStamp);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceHash);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),solvedHash);
            QVERIFY(sameResultValues(session->getModel(1),solvedSource));
            QVERIFY(session->getModel(0).getLoadedSourceBinding()==sourceBindingBefore);
            QVERIFY(session->getModel(1).getLoadedSourceBinding()==boundBindingBefore);
            QCOMPARE(session->getModel(0).getFileName(),sourceFileBefore);
            QCOMPARE(session->getModel(1).getFileName(),boundFileBefore);
            QCOMPARE(session->documentProjectContext(1).value("savedArtifactHash"),savedSourceHashBefore);
        }
        QCOMPARE(manager.jobQuery(jobId).value("projectSnapshotRef"),sourceSnapshotBefore);
        if (!publicationMutation.isEmpty() && publicationMutation!="render-and-reentrant-open") {
            QCOMPARE(opened.value("status").toString(),QString("unknown"));
            QCOMPARE(opened.value("code").toString(),QString("RESULT_OPEN_UNKNOWN"));
            QVERIFY(opened.value("result").toObject().value("documentMayExist").toBool());
            QVERIFY(!opened.value("result").toObject().value("documentId").toString().isEmpty());
            const uint afterFailureCount=session->getNModels();
            const auto repeated=host->handle(openRequest);
            QCOMPARE(repeated.value("status").toString(),QString("unknown"));
            QCOMPARE(repeated.value("result"),opened.value("result"));
            QCOMPARE(session->getNModels(),afterFailureCount);
            return;
        }
        if (publicationMutation=="render-and-reentrant-open")
            QCOMPARE(reentrantReply.value("code").toString(),QString("RESULT_OPEN_IN_PROGRESS"));
        QVERIFY2(opened.value("status")==QJsonValue("accepted"),qPrintable(QJsonDocument(opened).toJson()));
        const auto resultId = opened.value("result").toObject().value("documentId").toString();
        const uint resultModel = session->findModelByCommandIdentity(resultId);
        QVERIFY(resultModel<session->getNModels()); QVERIFY(resultId!=candidateStamp.first);
        QVERIFY(session->getModel(resultModel).getNVariables()>0);
        QVERIFY(session->getModel(resultModel).getLoadedSourceBinding().has_value());
        QVERIFY(*session->getModel(resultModel).getLoadedSourceBinding()==artifactBinding);
        QCOMPARE(session->getModel(resultModel).getLoadedSourceBinding()->savedArtifactHash,artifactBinding.savedArtifactHash);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(resultModel)),
                 RModelCommandService::stateFingerprint(artifactModel));
        QCOMPARE(session->getModel(resultModel).getNElementGroups(),artifactModel.getNElementGroups());
        for (uint group=0; group<artifactModel.getNElementGroups(); ++group)
            QCOMPARE(session->getModel(resultModel).getElementGroupPtr(group)->getMaterial().toJson(),
                     artifactModel.getElementGroupPtr(group)->getMaterial().toJson());
        QVERIFY(sameResultValues(session->getModel(resultModel),artifactModel));
        QVERIFY(!session->isVerifiedArtifactPublication(resultModel));
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceHash);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),solvedHash);
        capture(widget,"02-bound-native-result");
        // Rendering is a separate Windows acceptance gate; offscreen rows still
        // enforce all publication, provenance, numerical and negative checks.
        if (qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI") && publicationMutation.isEmpty()) {
            displayNativeResult(resultModel,resultId,artifactModel);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceHash);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(1)),solvedHash);
        }
        if (publicationMutation.isEmpty()) {
            geometryControlEvidence.insert("meshDocument",QJsonObject{
                {"sourceDocumentId",sourceStamp.first},{"sourceHash",sourceHash},
                {"candidateDocumentId",candidateStamp.first},{"boundHash",boundHash},
                {"meshAttemptId",attempt},{"nativeJob",job},{"resultDocumentId",resultId},
                {"nodeCount",int(artifactModel.getNNodes())},
                {"resultArtifactSha256",job.value("resultArtifactSha256")},
                {"scope","Independent closed convex TRI shell; geometry beam is a separate supported document"}});
            const auto bytes=QJsonDocument(geometryControlEvidence).toJson();
            QFile proof(QDir(workspace.path()).filePath("wf14-joint-evidence.json"));
            QVERIFY(proof.open(QIODevice::WriteOnly)); QCOMPARE(proof.write(bytes),qint64(bytes.size()));
            if (qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI")) {
                QFile visibleProof(QDir(qEnvironmentVariable("RANGE_WF14_SCREENSHOT_DIR"))
                    .filePath("04-geometry-mesh-control-joint.json"));
                QVERIFY(visibleProof.open(QIODevice::WriteOnly));
                QCOMPARE(visibleProof.write(bytes),qint64(bytes.size()));
            }
        }
    }
    void loadedBindingPreservesFileIdentityAndRejectsStaleApproval()
    {
        QTemporaryDir workspace, sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        const QString path = sources.filePath("unbound.ram");
        auto fixture = shellFixture(true); ModelIO::save(fixture,path,false);
        Model loaded; ModelIO::open(loaded,path);
        QVERIFY(loaded.getLoadedSourceBinding().has_value());
        const auto binding = *loaded.getLoadedSourceBinding();
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        // Fixture setup bypasses the ordinary task-selection import dialog;
        // real generated-candidate publication above keeps all signals enabled.
        { QSignalBlocker fixtureSetup(session); session->addModel(loaded); }
        // Populate the ordinary model tree before selecting the imported
        // fixture, so later modelChanged notifications preserve user selection.
        session->setModelChanged(0);
        auto *loadedTree=Application::instance()->getMainWindow()->findChild<ModelTree *>();
        QVERIFY(loadedTree); QVERIFY(loadedTree->topLevelItem(0));
        loadedTree->setCurrentItem(loadedTree->topLevelItem(0),0,QItemSelectionModel::ClearAndSelect);
        QTRY_COMPARE(session->getSelectedModelIDs(),QList<uint>{0});
        MeshBindingWidget widget; widget.setSession(session); widget.resize(900,800); widget.show();
        fillBinding(widget,cornerIds(session->getModel(0)));
        auto *preview = widget.findChild<QPushButton *>("meshBindingPreview");
        auto *apply = widget.findChild<QPushButton *>("meshBindingApply");
        QTRY_VERIFY(preview->isEnabled());
        clickButton(preview); QVERIFY2(apply->isEnabled(),bindingDiagnostic(widget).constData());
        // An accepted GUI source revision must retire the visible approval.
        session->getModel(0).setDescription("Source edited after binding preview");
        session->setModelChanged(0);
        QVERIFY(!apply->isEnabled());
        clickButton(apply);
        QCOMPARE(session->getModel(0).getProblemTaskTree().getProblemTypeMask(),int(R_PROBLEM_NONE));
        QCOMPARE(session->getModel(0).getNPoints(),0u);
        QTRY_VERIFY(session->documentProjectContext(0).value("persistence").toObject()
                    .value("metadataSynchronized").toBool());
        const auto before = RModelCommandService::stateFingerprint(session->getModel(0));
        clickButton(preview); QVERIFY2(apply->isEnabled(),bindingDiagnostic(widget).constData());
        auto *forces = widget.findChild<QTableWidget *>("meshBindingForces");
        forces->item(0,3)->setText("-2"); QVERIFY(!apply->isEnabled());
        forces->item(0,3)->setText("-1");
        clickButton(preview); QVERIFY2(apply->isEnabled(),bindingDiagnostic(widget).constData());
        clickButton(apply);
        QCOMPARE(bindingReply(widget).value("status").toString(),QString("accepted"));
        const auto after = RModelCommandService::stateFingerprint(session->getModel(0));
        for (const auto &command : {QString(),QString("document.undo"),QString("document.redo")}) {
            if (!command.isEmpty()) {
                const auto p = propose(session,0,command);
                QCOMPARE(p.value("status").toString(),QString("preview"));
                const auto id = p.value("result").toObject().value("previewId").toString();
                QVERIFY(host->approvePreview(id));
                QCOMPARE(execute(host,id).value("status").toString(),QString("accepted"));
            }
            QCOMPARE(session->getModel(0).getFileName(),path);
            QVERIFY(session->getModel(0).getLoadedSourceBinding().has_value());
            const auto &current = *session->getModel(0).getLoadedSourceBinding();
            QCOMPARE(current.savedArtifactHash,binding.savedArtifactHash);
            QCOMPARE(current.logicalSourcePath,binding.logicalSourcePath);
            QCOMPARE(current.resolvedSourcePath,binding.resolvedSourcePath);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),command=="document.undo" ? before : after);
        }
    }
private:
    void geometryControlPrelude(Session *session,const QString &project,QJsonObject &evidence)
    {
        const QDir bin(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
        const QString suffix=".exe";
#else
        const QString suffix;
#endif
        const QString generator=bin.filePath("range-workflow-native-import-fixture"+suffix);
        QVERIFY2(QFileInfo(generator).isFile(),qPrintable(generator));
        const QString originalPath=QDir(project).filePath("wf14-original-beam.ram");
        QProcess fixture; fixture.start(generator,{"beam",originalPath});
        QVERIFY(fixture.waitForFinished(30000));
        QCOMPARE(fixture.exitStatus(),QProcess::NormalExit); QCOMPARE(fixture.exitCode(),0);
        Model original;
        try { ModelIO::open(original,originalPath); }
        catch (const RError &exception) { QFAIL(qPrintable(exception.getMessage())); }
        QCOMPARE(original.getNNodes(),2u); QCOMPARE(original.getNodePhysicalId(1),117u);
        QCOMPARE(original.getNode(1).getX(),3.3);
        session->addModel(original); session->setModelSelected(0,true);
        const Model before=session->getModel(0);
        const auto beforeStamp=session->modelCommandStamp(0);
        const auto beforeHash=RModelCommandService::stateFingerprint(before);
        GeometryCommandWidget geometry; geometry.setSession(session); geometry.resize(900,600); geometry.show();
        auto *preview=geometry.findChild<QPushButton *>("geometryPreview");
        auto *apply=geometry.findChild<QPushButton *>("geometryApproveApply");
        QVERIFY(preview && apply); QTRY_VERIFY(preview->isEnabled()); QVERIFY(!apply->isEnabled());
        geometry.findChild<QLineEdit *>("geometryNodeId")->setText("117");
        const double afterX=before.getNode(1).getX()*1.01;
        geometry.findChild<QLineEdit *>("geometryCoordinates0")->setText(QString::number(afterX,'g',17));
        geometry.findChild<QLineEdit *>("geometryCoordinates1")->setText("1.25");
        geometry.findChild<QLineEdit *>("geometryCoordinates2")->setText("-0.5");
        clickButton(preview);
        QVERIFY2(apply->isEnabled(),qPrintable(geometry.findChild<QLabel *>("geometryStatus")->text()));
        QCOMPARE(geometry.findChild<QTableWidget *>("geometryChanges")->rowCount(),1);
        QCOMPARE(session->modelCommandStamp(0),beforeStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),beforeHash);
        QCOMPARE(session->getModel(0).getUndoStackSize(),0u);
        capture(geometry,"00-geometry-preview");
        clickButton(apply);
        QCOMPARE(session->getModel(0).getNode(1).getX(),afterX);
        QCOMPARE(session->getModel(0).getUndoStackSize(),1u); QVERIFY(!apply->isEnabled());
        const auto afterStamp=session->modelCommandStamp(0);
        QCOMPARE(afterStamp.first,beforeStamp.first); QVERIFY(afterStamp.second>beforeStamp.second);
        const QString receipt=geometry.findChild<QLabel *>("geometryReceipt")->text();
        QVERIFY(receipt.contains("one_gui_undo_group"));
        // Restore only the edited coordinates on a detached copy: all physical
        // IDs, global directions, conditions, material and control inputs must match.
        RModel restored(session->getModel(0));
        restored.getNode(1).set(before.getNode(1).getX(),before.getNode(1).getY(),before.getNode(1).getZ());
        QCOMPARE(RModelCommandService::stateFingerprint(restored),beforeHash);
        const auto editedHash=RModelCommandService::stateFingerprint(session->getModel(0));
        QVERIFY(editedHash!=beforeHash);
        capture(geometry,"00-geometry-applied");
        QString error;
        const auto snapshot=session->createDocumentSnapshot(0,&error);
        QVERIFY2(!snapshot.isEmpty(),qPrintable(error));
        QCOMPARE(snapshot.value("instanceRef").toObject().value("documentRevision").toDouble(),double(afterStamp.second));
        Model frozen;
        ModelIO::open(frozen,QDir(project).filePath(snapshot.value("relativePath").toString()));
        QCOMPARE(frozen.getNodePhysicalId(1),117u); QCOMPARE(frozen.getNode(1).getX(),afterX);
        const QJsonObject input{{"snapshot",snapshot},{"originalFixture",originalPath},
            {"beforeHash",beforeHash},{"editedHash",editedHash},{"geometryReceipt",receipt},
            {"documentId",afterStamp.first},{"sessionEpoch",session->getModelCommandHost()->sessionEpoch()},
            {"beforeRevision",double(beforeStamp.second)},{"editedRevision",double(afterStamp.second)},
            {"geometryDelta",QJsonObject{{"physicalNodeId",117},
                {"before",QJsonArray{3.3,1.25,-0.5}},{"after",QJsonArray{afterX,1.25,-0.5}}}}};
        const QString inputPath=QDir(project).filePath("wf14-geometry-input.json");
        QFile inputFile(inputPath); QVERIFY(inputFile.open(QIODevice::WriteOnly));
        QVERIFY(inputFile.write(QJsonDocument(input).toJson())>0); inputFile.close();
        auto config=WorkflowProcess::defaultConfiguration(QDir(project).filePath("control-workspace"));
#ifdef RANGE_WF14_PYTHON
        config.python=QStringLiteral(RANGE_WF14_PYTHON);
        config.packageDirectory=QStringLiteral(RANGE_WF14_PACKAGE);
        config.solver=QStringLiteral(RANGE_WF14_SOLVER);
        config.native=QStringLiteral(RANGE_WF14_NATIVE);
        config.examples=QStringLiteral(RANGE_WF14_EXAMPLES);
#endif
        const auto overridePath=[](const char *name,QString &path) {
            const auto value=qEnvironmentVariable(name); if (!value.isEmpty()) path=value;
        };
        overridePath("RANGE_TEST_SCIENTIFIC_PYTHON",config.python);
        overridePath("RANGE_TEST_SCIENTIFIC_PACKAGE",config.packageDirectory);
        overridePath("RANGE_TEST_SCIENTIFIC_SOLVER",config.solver);
        overridePath("RANGE_TEST_SCIENTIFIC_NATIVE",config.native);
        overridePath("RANGE_TEST_SCIENTIFIC_EXAMPLES",config.examples);
        for (auto *path:{&config.python,&config.packageDirectory,&config.solver,&config.native,&config.examples}) {
            QVERIFY2(QFileInfo(*path).isAbsolute() && QFileInfo::exists(*path),qPrintable(*path));
            *path=QFileInfo(*path).canonicalFilePath();
        }
#ifdef RANGE_WF14_GEOMETRY_CONTROL_BOOTSTRAP
        const QString bootstrap=QStringLiteral(RANGE_WF14_GEOMETRY_CONTROL_BOOTSTRAP);
#else
        const QString bootstrap=QDir(config.packageDirectory).filePath("../tests/wf14_geometry_control_bootstrap.py");
#endif
        QVERIFY2(QFileInfo(bootstrap).isFile(),qPrintable(bootstrap));
        const QString outputPath=QDir(project).filePath("wf14-control-evidence.json");
        QProcess control;
        control.setWorkingDirectory(config.packageDirectory);
        control.start(config.python,{"-B",QFileInfo(bootstrap).canonicalFilePath(),
            "--package",config.packageDirectory,"--python",config.python,"--solver",config.solver,
            "--native",config.native,"--examples",config.examples,"--catalog",project,
            "--input",inputPath,"--output",outputPath});
        QVERIFY2(control.waitForStarted(10000),qPrintable(control.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(control.state(),QProcess::NotRunning,240000);
        const QByteArray diagnostics=control.readAllStandardError()+control.readAllStandardOutput();
        QCOMPARE(control.exitStatus(),QProcess::NormalExit);
        QVERIFY2(control.exitCode()==0,diagnostics.constData());
        QFile proof(outputPath); QVERIFY(proof.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const auto document=QJsonDocument::fromJson(proof.readAll(),&parseError);
        QCOMPARE(parseError.error,QJsonParseError::NoError); QVERIFY(document.isObject());
        evidence=document.object(); QVERIFY(!evidence.value("controlJob").toString().isEmpty());
        QCOMPARE(evidence.value("registeredSource").toObject().value("registeredModelSha256"),snapshot.value("modelSha256"));
        QCOMPARE(session->modelCommandStamp(0),afterStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),editedHash);
        QCOMPARE(session->getModel(0).getUndoStackSize(),1u);
        QVERIFY(sameResultValues(session->getModel(0),before));
        // The supported geometry and shell meshing domains are intentionally
        // separate. Keep this Session/project and remove only the beam runtime
        // document after the real source/control receipt has been persisted.
        session->removeModel(0); QCOMPARE(session->getNModels(),0u);
    }
    void displayNativeResult(uint modelId, const QString &documentId, const Model &artifact)
    {
        auto *window=Application::instance()->getMainWindow();
        auto *session=Application::instance()->getSession();
        auto *central=window->findChild<CentralTabWidget *>(); QVERIFY(central);
        auto *modelDock=window->findChild<QDockWidget *>("modelDock"); QVERIFY(modelDock);
        auto *solverDock=window->findChild<QDockWidget *>("solverDock"); QVERIFY(solverDock);
        auto *modelTabs=qobject_cast<QTabWidget *>(modelDock->widget()); QVERIFY(modelTabs);
        auto *solverTabs=qobject_cast<QTabWidget *>(solverDock->widget()); QVERIFY(solverTabs);
        auto *selector=window->findChild<ResultsVariableSelector *>(); QVERIFY(selector);
        auto *properties=window->findChild<ResultsVariableTree *>(); QVERIFY(properties);
        const auto oldGeometry=window->saveGeometry(), oldLayout=window->saveState();
        const int oldCentral=central->currentIndex(), oldModel=modelTabs->currentIndex(),
                  oldSolver=solverTabs->currentIndex();
        const QString oldStatus=window->statusBar()->currentMessage();
        const auto restore=qScopeGuard([&] {
            central->setCurrentIndex(oldCentral); modelTabs->setCurrentIndex(oldModel);
            solverTabs->setCurrentIndex(oldSolver);
            window->restoreState(oldLayout); window->restoreGeometry(oldGeometry);
            window->statusBar()->showMessage(oldStatus);
        });
        // Ephemeral test layout: keep the ordinary model and Results controls,
        // remove panels that would obscure the central scientific viewport.
        for (auto *dock:window->findChildren<QDockWidget *>())
            if (dock!=modelDock && dock!=solverDock) dock->hide();
        modelDock->show(); solverDock->show(); solverDock->raise(); modelTabs->setCurrentIndex(0);
        int resultsTab=-1;
        for (int i=0;i<solverTabs->count();++i)
            if (solverTabs->widget(i)->isAncestorOf(selector)) resultsTab=i;
        QVERIFY(resultsTab>=0); solverTabs->setCurrentIndex(resultsTab);
        central->setCurrentIndex(CentralTabWidget::Model);
        window->showNormal();
        const QRect available=window->screen()->availableGeometry();
        window->resize(QSize(800,600).boundedTo(available.size()));
        window->move(available.topLeft());
        window->resizeDocks({modelDock,solverDock},{160,260},Qt::Horizontal);
        window->raise(); window->activateWindow();

        auto *tree=window->findChild<ModelTree *>(); QVERIFY(tree);
        QTRY_VERIFY(tree->topLevelItemCount()>int(modelId));
        tree->setCurrentItem(tree->topLevelItem(int(modelId)),0,QItemSelectionModel::ClearAndSelect);
        QTRY_COMPARE(session->getSelectedModelIDs(),QList<uint>{modelId});
        QCOMPARE(session->findModelByCommandIdentity(documentId),modelId);
        SubWindow *resultWindow=nullptr;
        for (auto *sub:window->findChildren<SubWindow *>())
            if (sub->getModelID()==modelId) resultWindow=sub;
        QVERIFY(resultWindow);
        central->getMdiArea()->setActiveSubWindow(resultWindow); resultWindow->showMaximized();
        auto *gl=resultWindow->getGlWidget(); QVERIFY(gl);
        QCOMPARE(gl->getModelID(),modelId);
        QTRY_COMPARE(central->getMdiArea()->activeSubWindow(),resultWindow);

        const uint variableIndex=session->getModel(modelId).findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(variableIndex!=RConstants::eod);
        const auto &variable=session->getModel(modelId).getVariable(variableIndex);
        QCOMPARE(variable.getApplyType(),R_VARIABLE_APPLY_NODE);
        QCOMPARE(variable.getNValues(),session->getModel(modelId).getNNodes());
        QCOMPARE(variable.getNVectors(),3u); QVERIFY(!variable.getUnits().isEmpty());
        const double minimum=variable.getMinValue(), maximum=variable.getMaxValue();
        QVERIFY(std::isfinite(minimum)); QVERIFY(std::isfinite(maximum)); QVERIFY(maximum>minimum);
        for (uint vector=0;vector<variable.getNVectors();++vector)
            for (uint value=0;value<variable.getNValues();++value)
                QVERIFY(std::isfinite(variable.getValue(vector,value)));
        const int choice=selector->findData(int(R_VARIABLE_DISPLACEMENT)); QVERIFY(choice>=0);
        selector->setCurrentIndex(choice);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(selector->currentData().toInt(),int(R_VARIABLE_DISPLACEMENT));
        QVERIFY(!session->getSelectedEntityIDs().isEmpty());
        const auto check=[&](const QString &text) -> QCheckBox * {
            for (auto *box:properties->findChildren<QCheckBox *>())
                if (box->text()==text) return box;
            return nullptr;
        };
        auto *scalar=check(REntityGroupData::getVariableDisplayName(R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR));
        QVERIFY(scalar); scalar->setChecked(true); QVERIFY(scalar->isChecked());
        QPushButton *autofill=nullptr;
        for (auto *button:properties->findChildren<QPushButton *>())
            if (button->text()==ResultsVariableTree::tr("Set default min/max values")) autofill=button;
        QVERIFY(autofill); autofill->click();
        auto *range=check(ResultsVariableTree::tr("Display value range")); QVERIFY(range);
        range->setChecked(true); QVERIFY(range->isChecked());
        QCOMPARE(variable.getVariableData().getMinDisplayValue(),minimum);
        QCOMPARE(variable.getVariableData().getMaxDisplayValue(),maximum);
        QVERIFY(variable.getVariableData().getDisplayValueRange());
        const auto scales=properties->findChildren<ValueScale *>();
        QVERIFY(scales.size()>=2); QCOMPARE(scales[0]->getValue(),maximum); QCOMPARE(scales[1]->getValue(),minimum);
        bool colouredVolume=false;
        for (uint group=0;group<session->getModel(modelId).getNVolumes();++group) {
            const auto &data=session->getModel(modelId).getVolume(group).getData().getVariableData();
            const auto entry=data.find(R_VARIABLE_DISPLACEMENT);
            colouredVolume |= entry!=data.end() && (entry->second & R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR);
        }
        QVERIFY(colouredVolume);
        // GUI style choices must never replace or convert native result values.
        QVERIFY(sameResultValues(session->getModel(modelId),artifact));
        gl->resetView(-45.0f,0.0f,-135.0f); gl->update();
        QTRY_VERIFY(gl->isVisible() && gl->width()>=200 && gl->height()>=200);
        QTRY_VERIFY(gl->isValid());
        QVERIFY(gl->context()); QVERIFY(gl->context()->isValid());
        QVERIFY(window->width()<=800 && window->height()<=600);
        const QString caption=QString("Native result: %1 nodes | %2 [%3] | %4 .. %5")
            .arg(variable.getNValues()).arg(variable.getName(),variable.getUnits())
            .arg(minimum,0,'e',3).arg(maximum,0,'e',3);
        window->statusBar()->showMessage(caption);
        const QDir output(qEnvironmentVariable("RANGE_WF14_SCREENSHOT_DIR"));
        // grabFramebuffer waits for the real native OpenGL paint; no generated pixels.
        const auto framebuffer=gl->grabFramebuffer(); QVERIFY(!framebuffer.isNull());
        QVERIFY(gl->getGLModelList().getNGlVolumeLists()>0);
        QVERIFY(framebuffer.save(output.filePath("03-native-result-viewport.png")));
        QCoreApplication::processEvents();
        QVERIFY(window->grab().save(output.filePath("03-native-result-display-root.png")));
        QFile proof(output.filePath("03-native-result-display.json")); QVERIFY(proof.open(QIODevice::WriteOnly));
        const QJsonObject evidence{{"documentId",documentId},{"modelId",int(modelId)},
            {"variable",variable.getName()},{"units",variable.getUnits()},{"scope","nodes"},
            {"valueCount",int(variable.getNValues())},{"components",int(variable.getNVectors())},
            {"minimum",minimum},{"maximum",maximum},{"nativeValuesPreserved",true},
            {"openGLContextValid",gl->context()->isValid()},{"scalarVolumeDisplay",colouredVolume},
            {"caption",caption},{"logicalWidth",window->width()},{"logicalHeight",window->height()},
            {"devicePixelRatio",window->devicePixelRatioF()}};
        QVERIFY(proof.write(QJsonDocument(evidence).toJson())>0);
    }
    void capture(QWidget &widget, const QString &stem)
    {
        if (!qEnvironmentVariableIntValue("RANGE_WF14_VISIBLE_GUI")) return;
        QCoreApplication::processEvents();
        const QDir output(qEnvironmentVariable("RANGE_WF14_SCREENSHOT_DIR"));
        QVERIFY(widget.grab().save(output.filePath(stem+".png")));
        QVERIFY(Application::instance()->getMainWindow()->grab().save(output.filePath(stem+"-root.png")));
    }
};

int runMeshBindingWidgetTests(int argc, char **argv)
{
    TestMeshBindingWidget test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_mesh_binding_widget.moc"
