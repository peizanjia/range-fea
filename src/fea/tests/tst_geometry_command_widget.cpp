#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include "application.h"
#include "geometry_command_widget.h"
#include "model_io.h"
#include "session.h"

namespace
{
Model geometryFixture()
{
    Model model;
    model.setName("Geometry widget fixture");
    model.setNNodes(2);
    model.setNode(0, RNode(0, 0, 0));
    model.setNode(1, RNode(1, 0, 0));
    model.setNodePhysicalId(0, 41);
    model.setNodePhysicalId(1, 73);
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0); element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    return model;
}
void draftNode(GeometryCommandWidget &widget, const QString &x)
{
    widget.findChild<QLineEdit *>("geometryNodeId")->setText("73");
    widget.findChild<QLineEdit *>("geometryCoordinates0")->setText(x);
}
}

class TestGeometryCommandWidget : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { Application::instance()->getSession()->clear(); }

    void draftAndPreviewRequireSeparateApproval()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(geometryFixture());
        session->setModelSelected(0, true);
        const auto stamp = session->modelCommandStamp(0);
        GeometryCommandWidget widget;
        widget.setSession(session);
        auto *apply = widget.findChild<QPushButton *>("geometryApproveApply");
        QVERIFY(!apply->isEnabled());
        draftNode(widget, "not-a-number");
        widget.preview();
        QVERIFY(!apply->isEnabled());
        QCOMPARE(session->modelCommandStamp(0), stamp);
        draftNode(widget, "2");
        widget.preview();
        QVERIFY(apply->isEnabled());
        QCOMPARE(widget.findChild<QTableWidget *>("geometryChanges")->rowCount(), 1);
        QCOMPARE(session->getModel(0).getNode(1).getX(), 1.0);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
        widget.approveAndApply();
        QCOMPARE(session->getModel(0).getNode(1).getX(), 2.0);
        QVERIFY(session->modelCommandStamp(0).second > stamp.second);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 1u);
        QVERIFY(!apply->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("geometryReceipt")->text().contains("one_gui_undo_group"));
    }

    void draftSelectionAndWorkspaceInvalidatePreview()
    {
        static QTemporaryDir workspace;
        static QTemporaryDir nextWorkspace;
        QVERIFY(workspace.isValid() && nextWorkspace.isValid());
        auto *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(geometryFixture());
        session->addModel(geometryFixture());
        session->setModelSelected(0, true);
        session->setModelSelected(1, false);
        GeometryCommandWidget widget;
        widget.setSession(session);
        draftNode(widget, "2");
        auto *apply = widget.findChild<QPushButton *>("geometryApproveApply");
        widget.preview(); QVERIFY(apply->isEnabled());
        draftNode(widget, "3"); QVERIFY(!apply->isEnabled());
        widget.approveAndApply();
        QCOMPARE(session->getModel(0).getNode(1).getX(), 1.0);
        widget.preview(); QVERIFY(apply->isEnabled());
        session->setModelSelected(0, false);
        session->setModelSelected(1, true);
        QVERIFY(!apply->isEnabled());
        widget.approveAndApply();
        QCOMPARE(session->getModel(1).getNode(1).getX(), 1.0);
        widget.preview(); QVERIFY(apply->isEnabled());
        session->clear();
        QVERIFY2(session->openProject(nextWorkspace.path(), &error), qPrintable(error));
        session->addModel(geometryFixture());
        session->setModelSelected(0, true);
        widget.refreshContext();
        QVERIFY(!apply->isEnabled());
        widget.approveAndApply();
        QCOMPARE(session->getModel(0).getNode(1).getX(), 1.0);
    }

    void rigidTransformAndExternalSourceGuard()
    {
        static QTemporaryDir workspace;
        static QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model source = geometryFixture();
        const QString path = sources.filePath("geometry-source.ram");
        ModelIO::save(source, path, false);
        Model loaded; ModelIO::open(loaded, path);
        session->addModel(loaded); session->setModelSelected(0, true);
        GeometryCommandWidget widget;
        widget.setSession(session);
        widget.findChild<QComboBox *>("geometryCommand")->setCurrentIndex(1);
        widget.findChild<QLineEdit *>("geometryAxis2")->setText("0");
        widget.preview();
        auto *apply = widget.findChild<QPushButton *>("geometryApproveApply");
        QVERIFY(!apply->isEnabled());
        widget.findChild<QLineEdit *>("geometryAxis2")->setText("1");
        widget.findChild<QLineEdit *>("geometryTranslation1")->setText("2");
        widget.preview(); QVERIFY(apply->isEnabled());
        QCOMPARE(widget.findChild<QTableWidget *>("geometryChanges")->rowCount(), 2);
        const auto stamp = session->modelCommandStamp(0);
        QFile external(path); QVERIFY(external.open(QIODevice::Append));
        QVERIFY(external.write("\n") == 1); external.close();
        widget.refreshContext();
        QVERIFY(!apply->isEnabled());
        widget.approveAndApply();
        QCOMPARE(session->getModel(0).getNode(0).getY(), 0.0);
        QCOMPARE(session->modelCommandStamp(0), stamp);
    }
};

int runGeometryCommandWidgetTests(int argc, char **argv)
{
    TestGeometryCommandWidget test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_geometry_command_widget.moc"
