#include <QtTest>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QStandardPaths>
#include <QSaveFile>
#include <QScopeGuard>
#include <rmesh_job_service.h>
#include <rmodel_command_service.h>
#include "application.h"
#include "mesh_command_widget.h"
#include "mesh_job_controller.h"
#include "material_manager_tree.h"
#include "model_command_host.h"
#include "model_io.h"
#include "session.h"

namespace
{
const RMaterial &fixtureMaterial()
{
    static const RMaterial material = [] {
        auto value = RMaterial::generateDefault();
        value.setName("Uniform fixture material"); return value;
    }();
    return material;
}
Model tetraSurface()
{
    Model model;
    model.setName("Mesh GUI tetrahedron");
    model.setNNodes(4); model.setNSurfaces(1);
    model.getNode(0).set(0,0,0); model.getNode(1).set(1,0,0);
    model.getNode(2).set(0,1,0); model.getNode(3).set(0,0,1);
    model.getSurface(0).setName("outer");
    model.getSurface(0).setMaterial(fixtureMaterial());
    const uint faces[4][3]{{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (uint i=0;i<4;++i)
    {
        model.setNodePhysicalId(i,101+10*i);
        RElement element(R_ELEMENT_TRI1);
        for (uint j=0;j<3;++j) element.setNodeId(j,faces[i][j]);
        model.addElement(element,true,0); model.setElementPhysicalId(i,501+10*i);
    }
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    return model;
}
QJsonObject options()
{ return {{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",0.08},{"radiusEdgeRatio",1.6}}; }
QString status(MeshJobController &controller,const QString &attempt)
{ return controller.query(attempt).value("result").toObject().value("status").toString(); }
QJsonObject registry(const QString &workspace)
{
    QFile file(QDir(workspace).filePath("mesh-jobs/registry.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeRegistry(const QString &workspace,const QJsonObject &object)
{
    QSaveFile file(QDir(workspace).filePath("mesh-jobs/registry.json"));
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
void refreshMaterialDatabase()
{
    const QString directory = Application::instance()->getApplicationSettings()->getMaterialsDir();
    for (auto *tree : Application::instance()->getMainWindow()->findChildren<MaterialManagerTree *>())
        QMetaObject::invokeMethod(tree,"onDirectoryChanged",Qt::DirectConnection,Q_ARG(QString,directory));
}
}

class TestMeshJobController : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(QStandardPaths::isTestModeEnabled());
        const QString directory = Application::instance()->getApplicationSettings()->getMaterialsDir();
        QVERIFY(directory.contains("test-runtime"));
        QVERIFY(QDir().mkpath(directory));
        fixtureMaterial().write(materialPath());
        QVERIFY(QFileInfo::exists(materialPath())); refreshMaterialDatabase();
        // Each test owns its controller's recovery lifecycle. The real main
        // window remains active, but must not race for that same workspace lease.
        for (auto *widget : Application::instance()->getMainWindow()->findChildren<MeshCommandWidget *>())
            widget->setController(nullptr);
        Application::instance()->getSession()->getModelCommandHost()->meshController()->shutdown();
    }
    void cleanupTestCase()
    {
        QVERIFY(QFile::remove(materialPath())); refreshMaterialDatabase();
        for (auto *widget : Application::instance()->getMainWindow()->findChildren<MeshCommandWidget *>())
            widget->setController(Application::instance()->getSession()->getModelCommandHost()->meshController());
    }
    void cleanup() { Application::instance()->getSession()->clear(); }

    void previewAndQueuedCancellationLeaveSourceUntouched()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        const auto before = session->modelCommandStamp(0);
        const QString hash = RModelCommandService::stateFingerprint(session->getModel(0));
        MeshJobController controller(session);
        QVERIFY(!controller.approveGenerate("caller-manufactured-approval").value("valid").toBool());
        auto bad = options(); bad.insert("callback","anything");
        QVERIFY(!controller.previewGenerate(0,bad).value("valid").toBool());
        const auto preview = controller.previewGenerate(0,options());
        QVERIFY2(preview.value("valid").toBool(),qPrintable(QJsonDocument(preview).toJson()));
        QCOMPARE(session->getNModels(),1u); QCOMPARE(session->modelCommandStamp(0),before);
        QCOMPARE(controller.jobs().size(),0);
        const auto result = preview.value("result").toObject();
        const QString id = result.value("attemptId").toString();
        const QString directory = result.value("request").toObject().value("jobDirectory").toString();
        QVERIFY(!QFileInfo::exists(directory));
        QVERIFY(controller.approveGenerate(result.value("previewId").toString()).value("valid").toBool());
        QCOMPARE(status(controller,id),QString("queued"));
        QVERIFY(controller.cancel(id).value("valid").toBool());
        QCoreApplication::processEvents();
        QCOMPARE(status(controller,id),QString("cancelled"));
        QVERIFY(!QFileInfo::exists(QDir(directory).filePath("intent.json")));
        QCOMPARE(session->getNModels(),1u); QCOMPARE(session->modelCommandStamp(0),before);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),hash);
        QCOMPARE(session->getModel(0).getUndoStackSize(),0u);
    }

    void actualWorkerCandidateRequiresSeparateAcceptance()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        const auto sourceStamp = session->modelCommandStamp(0);
        const QString sourceHash = RModelCommandService::stateFingerprint(session->getModel(0));
        MeshJobController controller(session);
        const auto p = controller.previewGenerate(0,options());
        QVERIFY2(p.value("valid").toBool(),qPrintable(QJsonDocument(p).toJson()));
        const auto preview = p.value("result").toObject();
        const QString id = preview.value("attemptId").toString();
        QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(status(controller,id),QString("candidate_ready"),15000);
        QCOMPARE(session->getNModels(),1u);
        QVERIFY(!controller.approveAccept("unpreviewed").value("valid").toBool());
        const auto accept = controller.previewAccept(id);
        QVERIFY2(accept.value("valid").toBool(),qPrintable(QJsonDocument(accept).toJson()));
        QCOMPARE(session->getNModels(),1u);
        // Removing a database entry must not strip approved candidate material
        // or open another import approval during its verified publication.
        QString unexpectedModal;
        QTimer modalWatchdog;
        modalWatchdog.setInterval(100);
        connect(&modalWatchdog,&QTimer::timeout,this,[&] {
            auto *modal = QApplication::activeModalWidget();
            if (!modal || !unexpectedModal.isEmpty()) return;
            unexpectedModal = QStringLiteral("Unexpected modal during mesh acceptance: %1 (%2)")
                .arg(modal->windowTitle(), QString::fromLatin1(modal->metaObject()->className()));
            if (auto *message = qobject_cast<QMessageBox *>(modal))
                unexpectedModal += QStringLiteral(": ") + message->text();
            qWarning().noquote() << unexpectedModal;
            if (auto *dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
            else modal->close();
        });
        modalWatchdog.start();
        qInfo("mesh acceptance: removing fixture material");
        QVERIFY(QFile::remove(materialPath()));
        qInfo("mesh acceptance: refreshing material database");
        refreshMaterialDatabase();
        auto restoreMaterial = qScopeGuard([] { fixtureMaterial().write(materialPath()); refreshMaterialDatabase(); });
        qInfo("mesh acceptance: approving verified candidate");
        const auto approved = controller.approveAccept(accept.value("result").toObject().value("previewId").toString());
        qInfo("mesh acceptance: approval returned");
        QVERIFY2(unexpectedModal.isEmpty(),qPrintable(unexpectedModal));
        QVERIFY2(approved.value("valid").toBool(),qPrintable(QJsonDocument(approved).toJson()));
        QCOMPARE(session->getNModels(),2u); QCOMPARE(session->modelCommandStamp(0),sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceHash);
        QVERIFY(session->modelCommandStamp(1).first != sourceStamp.first);
        QCOMPARE(session->getModel(1).getProblemTaskTree().getProblemTypeMask(),RProblemTypeMask(R_PROBLEM_NONE));
        QVERIFY(session->getModel(1).getNVolumes() == 1);
        QVERIFY(session->getModel(1).getNVariables() == 0);
        QVERIFY(!session->getModel(1).getLoadedSourceBinding());
        QCOMPARE(session->getModel(1).getSurface(0).getMaterial().toJson(),fixtureMaterial().toJson());
        QCOMPARE(session->getModel(1).getVolume(0).getMaterial().toJson(),fixtureMaterial().toJson());
        QCOMPARE(status(controller,id),QString("accepted"));
        QVERIFY(!controller.previewAccept(id).value("valid").toBool());
        QCOMPARE(session->getNModels(),2u);
    }

    void abandonedAndPartialCapturesStayWithinPersistentStorageQuota()
    {
        QTemporaryDir workspace;
        auto *session=Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        MeshJobController controller(session);
        const auto first=controller.previewGenerate(0,options());
        QVERIFY2(first.value("valid").toBool(),qPrintable(QJsonDocument(first).toJson()));
        const QString captured=first.value("result").toObject().value("request").toObject().value("sourceSnapshotPath").toString();
        const auto bytes=RMeshJobService::fileSha256(captured);
        controller.invalidatePreviews();
        QDir captures(QDir(workspace.path()).filePath("mesh-jobs/captures"));
        for(int i=1;i<64;++i) QVERIFY(captures.mkdir(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QCOMPARE(captures.entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),64);
        const auto rejected=controller.previewGenerate(0,options());
        QVERIFY(!rejected.value("valid").toBool());
        QCOMPARE(rejected.value("code").toString(),QString("capture_storage_full"));
        QCOMPARE(captures.entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),64);
        QCOMPARE(RMeshJobService::fileSha256(captured),bytes);
        QVERIFY(controller.jobs().isEmpty());
        controller.shutdown();
        MeshJobController reopened(session);
        QCOMPARE(reopened.previewGenerate(0,options()).value("code").toString(),QString("capture_storage_full"));
        QCOMPARE(RMeshJobService::fileSha256(captured),bytes);
    }

    void staleClosedSourceAndCorruptArtifactCannotBeAccepted()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        MeshJobController controller(session);
        const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
        QVERIFY(!preview.isEmpty());
        const QString id = preview.value("attemptId").toString();
        QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(status(controller,id),QString("candidate_ready"),15000);
        const auto acceptedPreview = controller.previewAccept(id);
        QVERIFY(acceptedPreview.value("valid").toBool());
        session->removeModel(0);
        const auto stale = controller.query(id).value("result").toObject();
        QCOMPARE(stale.value("status").toString(),QString("candidate_ready"));
        QVERIFY(!stale.value("sourceCurrent").toBool());
        QVERIFY(!controller.approveAccept(acceptedPreview.value("result").toObject().value("previewId").toString()).value("valid").toBool());
        QCOMPARE(session->getNModels(),0u);
        const QString artifact = stale.value("descriptor").toObject().value("candidatePath").toString();
        QFile file(artifact); QVERIFY(file.open(QIODevice::Append)); QVERIFY(file.write("corrupt") == 7); file.close();
        QCOMPARE(status(controller,id),QString("error"));
        QVERIFY(!controller.previewAccept(id).value("valid").toBool());
        QCOMPARE(session->getNModels(),0u);
    }

    void fullSourceBytesGuardAndVisibleDraftInvalidation()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        MeshJobController controller(session);
        MeshCommandWidget widget; widget.setSession(session); widget.setController(&controller);
        QVERIFY(widget.findChild<QPlainTextEdit *>("meshDetails")->isReadOnly());
        auto *approve = widget.findChild<QPushButton *>("meshApproveGenerate");
        QVERIFY(!approve->isEnabled());
        widget.findChild<QLineEdit *>("meshMaxVolume")->setText("NaN");
        widget.previewGeneration(); QVERIFY(!approve->isEnabled());
        widget.findChild<QLineEdit *>("meshMaxVolume")->setText("0.08");
        widget.previewGeneration(); QVERIFY(approve->isEnabled());
        widget.findChild<QLineEdit *>("meshRadiusEdgeRatio")->setText("1.7");
        QVERIFY(!approve->isEnabled());
        widget.approveGeneration(); QCOMPARE(controller.jobs().size(),0);
        widget.previewGeneration(); QVERIFY(approve->isEnabled());
        session->getModel(0).setDescription("Accepted external edit");
        session->setModelChanged(0);
        // Both widget approval and controller preview invalidation stay direct.
        QVERIFY(!approve->isEnabled());
        widget.approveGeneration(); QCOMPARE(controller.jobs().size(),0);
        const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
        QVERIFY(!preview.isEmpty());
        // No revision notification: only the full native-byte comparison can
        // catch this unsupported direct mutation before worker launch.
        session->getModel(0).setDescription("Unannounced source mutation");
        const auto approved = controller.approveGenerate(preview.value("previewId").toString());
        QVERIFY(!approved.value("valid").toBool());
        QCOMPARE(controller.jobs().size(),0);
    }

    void runningCancelAndOwnerCloseNeverReplay()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        QString id, requestHash;
        {
            MeshJobController controller(session);
            const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
            QVERIFY(!preview.isEmpty()); id = preview.value("attemptId").toString();
            QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
            QCoreApplication::processEvents();
            QCOMPARE(status(controller,id),QString("running"));
            QVERIFY(controller.cancel(id).value("valid").toBool());
            QCOMPARE(status(controller,id),QString("cancelling"));
            QTRY_COMPARE_WITH_TIMEOUT(status(controller,id),QString("cancelled"),5000);
            QCOMPARE(controller.query(id).value("result").toObject().value("phase").toString(),QString("child_tree_exited"));
            const auto next = controller.previewGenerate(0,options()).value("result").toObject();
            QVERIFY(!next.isEmpty()); id = next.value("attemptId").toString();
            const auto started = controller.approveGenerate(next.value("previewId").toString());
            QVERIFY(started.value("valid").toBool()); QCoreApplication::processEvents();
            requestHash = started.value("result").toObject().value("descriptor").toObject().value("requestSha256").toString();
            controller.shutdown();
            const auto records = registry(workspace.path()).value("jobs").toArray();
            bool found = false;
            for (const auto &value : records)
                if (value.toObject().value("attemptId") == QJsonValue(id))
                { found = true; QCOMPARE(value.toObject().value("status").toString(),QString("interrupted")); }
            QVERIFY(found);
        }
        MeshJobController restarted(session);
        const auto current = restarted.query(id).value("result").toObject();
        QVERIFY(current.value("status") == QJsonValue("interrupted") || current.value("status") == QJsonValue("candidate_ready"));
        QCOMPARE(current.value("pid").toDouble(),0.0);
        QCOMPARE(current.value("descriptor").toObject().value("requestSha256").toString(),requestHash);
        QCoreApplication::processEvents();
        QVERIFY(status(restarted,id) != "running"); QCOMPARE(session->getNModels(),1u);
    }

    void failedRegistryCancellationIsStickyAcrossRestart()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        const QString path = QDir(workspace.path()).filePath("mesh-jobs/registry.json");
        const QString backup = path+".fault-backup";
        QString id;
        {
            MeshJobController controller(session);
            const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
            QVERIFY(!preview.isEmpty()); id = preview.value("attemptId").toString();
            QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
            QVERIFY(QFile::rename(path,backup)); QVERIFY(QDir().mkdir(path));
            const auto cancelled = controller.cancel(id);
            QVERIFY(!cancelled.value("valid").toBool());
            QCOMPARE(cancelled.value("code").toString(),QString("cancel_registry_unknown"));
            QCOMPARE(status(controller,id),QString("cancelled"));
            const QString decision = QDir(workspace.path()).filePath("mesh-jobs/captures/"+id+"/owner-decision.json");
            QVERIFY(QFileInfo::exists(decision));
            QCoreApplication::processEvents();
            QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath("mesh-jobs/attempts/"+id+"/intent.json")));
        }
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup,path));
        MeshJobController restarted(session);
        QCOMPARE(status(restarted,id),QString("cancelled"));
        QVERIFY(!restarted.previewAccept(id).value("valid").toBool());
        QCOMPARE(session->getNModels(),1u);
    }

    void completedReceiptCannotUndoDurableTimeoutOrCancellation()
    {
        QTemporaryDir workspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        QString id;
        {
            MeshJobController controller(session);
            const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
            QVERIFY(!preview.isEmpty()); id = preview.value("attemptId").toString();
            QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
            QTRY_COMPARE_WITH_TIMEOUT(status(controller,id),QString("candidate_ready"),15000);
        }
        auto records = registry(workspace.path());
        auto jobs = records.value("jobs").toArray(); QVERIFY(jobs.size() == 1);
        auto entry = jobs[0].toObject();
        // Simulates the deadline winning after a complete receipt, including
        // the legacy error/code representation from the reviewed implementation.
        entry.insert("status","error"); entry.insert("code","worker_timeout");
        jobs[0] = entry; records.insert("jobs",jobs); QVERIFY(writeRegistry(workspace.path(),records));
        {
            MeshJobController restarted(session);
            QCOMPARE(status(restarted,id),QString("timed_out"));
            QVERIFY(!restarted.previewAccept(id).value("valid").toBool());
        }
        records = registry(workspace.path()); jobs = records.value("jobs").toArray(); entry = jobs[0].toObject();
        entry.insert("status","interrupted"); entry.insert("terminalDecision","cancel"); entry.remove("code");
        jobs[0] = entry; records.insert("jobs",jobs); QVERIFY(writeRegistry(workspace.path(),records));
        MeshJobController cancelled(session);
        QCOMPARE(status(cancelled,id),QString("cancelled"));
        QVERIFY(!cancelled.previewAccept(id).value("valid").toBool());
        QCOMPARE(session->getNModels(),1u);
    }

    void synchronousPublicationCallbacksCannotProduceAcceptanceSuccess_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("modify-candidate-material") << QString("material");
        QTest::newRow("remove-candidate") << QString("remove");
        QTest::newRow("switch-workspace") << QString("workspace");
    }
    void synchronousPublicationCallbacksCannotProduceAcceptanceSuccess()
    {
        QFETCH(QString,action);
        QTemporaryDir workspace,otherWorkspace;
        auto *session = Application::instance()->getSession(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(tetraSurface()); session->setModelSelected(0,true);
        const QString sourceDocument = session->modelCommandStamp(0).first;
        MeshJobController controller(session);
        const auto preview = controller.previewGenerate(0,options()).value("result").toObject();
        QVERIFY(!preview.isEmpty()); const QString id = preview.value("attemptId").toString();
        QVERIFY(controller.approveGenerate(preview.value("previewId").toString()).value("valid").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(status(controller,id),QString("candidate_ready"),15000);
        const auto acceptance = controller.previewAccept(id).value("result").toObject(); QVERIFY(!acceptance.isEmpty());
        const auto callback = connect(session,&Session::modelAdded,&controller,[&](uint modelId) {
            if (session->modelCommandStamp(modelId).first == sourceDocument) return;
            if (action == "material") session->getModel(modelId).getSurface(0).setMaterial(RMaterial());
            else if (action == "remove") session->removeModel(modelId);
            else
            {
                session->clear(); QString switchError;
                QVERIFY2(session->openProject(otherWorkspace.path(),&switchError),qPrintable(switchError));
                controller.jobs(); // Force controller registry switching in this callback.
            }
        },Qt::DirectConnection);
        const auto result = controller.approveAccept(acceptance.value("previewId").toString());
        disconnect(callback);
        QVERIFY(!result.value("valid").toBool());
        QCOMPARE(result.value("code").toString(),QString("acceptance_unknown"));
        QVERIFY(!result.value("result").toObject().value("createdDocumentId").toString().isEmpty());
        if (action == "workspace")
        {
            QVERIFY(controller.jobs().isEmpty());
            const auto oldJobs = registry(workspace.path()).value("jobs").toArray(); QVERIFY(oldJobs.size() == 1);
            QVERIFY(oldJobs[0].toObject().value("status") == QJsonValue("accepting_unknown")
                || oldJobs[0].toObject().value("status") == QJsonValue("acceptance_unknown"));
        }
        else
        {
            QCOMPARE(status(controller,id),QString("acceptance_unknown"));
            QVERIFY(!controller.previewAccept(id).value("valid").toBool());
            QVERIFY(session->findModelByCommandIdentity(sourceDocument) < session->getNModels());
        }
    }

private:
    static QString materialPath()
    {
        return QDir(Application::instance()->getApplicationSettings()->getMaterialsDir())
            .filePath(fixtureMaterial().getID().toString(QUuid::WithoutBraces)+"."+RMaterial::getDefaultFileExtension());
    }
};

int runMeshJobControllerTests(int argc,char **argv)
{
    TestMeshJobController test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_mesh_job_controller.moc"
