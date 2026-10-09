#include "series_workbench_widget.h"
#include "workflow_process.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

// Fixture cases verify transport; the final native case uses actual FE artifacts.
class SeriesWorkbenchWidgetTests : public QObject
{
    Q_OBJECT
    QTemporaryDir fixture;
    WorkflowProcess::Configuration config;
    QString oldOrganization, oldApplication;
    QSettings::Format oldFormat;
    template<class T> T *control(SeriesWorkbenchWidget &w, const char *name)
    { return w.findChild<T *>(QString::fromLatin1(name)); }
    void click(SeriesWorkbenchWidget &w, const char *name)
    { QTest::mouseClick(control<QPushButton>(w,name),Qt::LeftButton); }
    QJsonObject request(WorkflowProcess &p,const QString &op,const QJsonObject &params = {})
    {
        QSignalSpy replies(&p,&WorkflowProcess::replyReceived); const auto id = p.request(op,params);
        QElapsedTimer timer; timer.start();
        while (timer.elapsed()<10000) {
            for (const auto &r : replies) if (r[0].toString()==id) return r[2].toJsonObject();
            QTest::qWait(10);
        }
        return {};
    }
    void start(WorkflowProcess &p,SeriesWorkbenchWidget &w)
    {
        w.resize(380,700); w.show(); p.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(p.isReady(),10000);
        QTRY_COMPARE_WITH_TIMEOUT(control<QComboBox>(w,"seriesJobChoice")->count(),2,10000);
        QTRY_COMPARE_WITH_TIMEOUT(control<QComboBox>(w,"seriesChannelChoice")->count(),4,10000);
    }
    void oneSource(SeriesWorkbenchWidget &w)
    { click(w,"seriesAddNative"); w.queryPlot(); QTRY_VERIFY(!w.displayedDto().isEmpty()); }
    QJsonArray calls(WorkflowProcess &p,const QString &op)
    {
        QJsonArray filtered;
        for (const auto &c : request(p,"fixture.query").value("calls").toArray())
            if (c.toObject().value("operation")==op) filtered.append(c);
        return filtered;
    }
private slots:
    void initTestCase()
    {
        QVERIFY(fixture.isValid());
        config = WorkflowProcess::defaultConfiguration(fixture.path()+"/workspace");
#ifdef RANGE_SERIES_PYTHON
        config.python = QString::fromUtf8(RANGE_SERIES_PYTHON);
#endif
        QVERIFY(!config.python.isEmpty()); config.solver = config.python; config.native = config.python;
        config.examples = fixture.path(); config.packageDirectory = fixture.path()+"/package";
        QVERIFY(QDir().mkpath(config.workspace));
        QVERIFY(QDir().mkpath(config.packageDirectory+"/range_workflow"));
        QFile init(config.packageDirectory+"/range_workflow/__init__.py"); QVERIFY(init.open(QIODevice::WriteOnly)); init.close();
#ifdef RANGE_SERIES_WIDGET_BOOTSTRAP
        QVERIFY(QFile::copy(QString::fromUtf8(RANGE_SERIES_WIDGET_BOOTSTRAP),config.packageDirectory+"/range_workflow/__main__.py"));
#else
        QFAIL("RANGE_SERIES_WIDGET_BOOTSTRAP must identify the isolated fixture");
#endif
        oldOrganization = QCoreApplication::organizationName(); oldApplication = QCoreApplication::applicationName(); oldFormat = QSettings::defaultFormat();
        QCoreApplication::setOrganizationName("range-series-widget-fixture"); QCoreApplication::setApplicationName("isolated-wf10");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,fixture.path()+"/settings");
    }
    void cleanupTestCase()
    {
        QSettings::setDefaultFormat(oldFormat); QCoreApplication::setOrganizationName(oldOrganization); QCoreApplication::setApplicationName(oldApplication);
    }
    void fourSourceBoundedTypedPlot()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w);
        auto *channels = control<QComboBox>(w,"seriesChannelChoice");
        click(w,"seriesAddNative"); click(w,"seriesAddReference");
        channels->setCurrentIndex(1); click(w,"seriesAddNative"); channels->setCurrentIndex(2); click(w,"seriesAddNative");
        QCOMPARE(control<QListWidget>(w,"seriesSourceSelection")->count(),4);
        channels->setCurrentIndex(3); click(w,"seriesAddNative"); QCOMPARE(control<QListWidget>(w,"seriesSourceSelection")->count(),4);
        w.queryPlot(); QTRY_COMPARE(w.plotWidget()->dataset().traces.size(),4);
        for (const auto &t : w.plotWidget()->dataset().traces) {
            QVERIFY(t.points.size() <= 1000); QCOMPARE(t.originalOrdinals.last(),quint64(999)); QCOMPARE(t.unit,QString("m/s"));
        }
        const auto plots = calls(p,"result.plot.query"); QVERIFY(!plots.isEmpty());
        QCOMPARE(plots.last().toObject().value("parameters").toObject().value("options").toObject().value("limit").toInt(),1000);
        QVERIFY(control<QLabel>(w,"seriesDisplayedSources")->text().contains("independent-reference"));
        QVERIFY(w.minimumSizeHint().width() < 600);
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void completeRawExportChecksCountHashAndChunks()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w); oneSource(w);
        const auto path = fixture.path()+"/whole.csv"; w.exportToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(w,"seriesStatus")->text().contains("Exported 2000"),10000);
        QFile csv(path); QVERIFY(csv.open(QIODevice::ReadOnly)); const auto data=csv.readAll();
        QCOMPARE(data.count('\n'),2001); QVERIFY(data.size()>65536);
        const auto exportCalls=calls(p,"result.raw.export"); QCOMPARE(exportCalls.size(),1);
        const auto params=exportCalls[0].toObject().value("parameters").toObject();
        QCOMPARE(params.keys(),QStringList({"range","sources"})); QCOMPARE(params.value("range").toString(),QString("complete"));
        QCOMPARE(params.value("sources"),w.displayedDto().value("export").toObject().value("sources"));
        const auto chunks=calls(p,"result.raw.export.chunk"); QVERIFY(chunks.size()>1);
        for (const auto &c : chunks) QVERIFY(c.toObject().value("parameters").toObject().value("length").toInt()<=65536);
        QVERIFY(!QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex().isEmpty());
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void heldOldDisplayExportsOldPinsThenAdoptsLatest()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w); oneSource(w);
        const auto old=w.displayedDto(); const auto oldPoints=w.plotWidget()->dataset().traces[0].points;
        auto *hold=w.findChild<QCheckBox *>("seriesPlotHold"); QVERIFY(hold); hold->setChecked(true);
        request(p,"fixture.revision"); w.queryPlot(); QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("pending"));
        QCOMPARE(w.displayedDto(),old); QCOMPARE(w.plotWidget()->dataset().traces[0].points,oldPoints);
        const auto path=fixture.path()+"/held.csv"; w.exportToFile(path);
        QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("Exported 2000"));
        const auto exported=calls(p,"result.raw.export").last().toObject().value("parameters").toObject().value("sources");
        QCOMPARE(exported,old.value("export").toObject().value("sources"));
        w.plotWidget()->setDisplayHeld(false);
        QVERIFY(!hold->isChecked()); QVERIFY(w.displayedDto()!=old); QVERIFY(w.plotWidget()->dataset().traces[0].points!=oldPoints);
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void staleWorkspaceAndSelectionRepliesAreRejected()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w); oneSource(w);
        const auto original=w.displayedDto(); request(p,"fixture.defer"); w.queryPlot();
        QTRY_COMPARE(request(p,"fixture.query").value("deferred").toArray().size(),1);
        QSignalSpy replies(&p,&WorkflowProcess::replyReceived);
        control<QComboBox>(w,"seriesJobChoice")->setCurrentIndex(1); request(p,"fixture.release");
        QCOMPARE(w.displayedDto(),original); QList<QVariant> late;
        for (const auto &r : replies) if (r[1].toString()=="result.plot.query") late=r;
        QVERIFY(!late.isEmpty());
        p.stop(); QTRY_VERIFY(p.isStopped());
        auto other=config; other.workspace=fixture.path()+"/other-workspace"; QVERIFY(QDir().mkpath(other.workspace));
        p.start(other); QTRY_VERIFY(p.isReady()); QTRY_VERIFY(w.displayedDto().isEmpty());
        p.replyReceived(late[0].toString(),late[1].toString(),late[2].toJsonObject());
        QVERIFY(w.displayedDto().isEmpty());
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void badDtoUnitsReferenceAndChunkPreservePriorState()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w);
        click(w,"seriesAddReference"); w.queryPlot(); QTRY_VERIFY(!w.displayedDto().isEmpty()); const auto old=w.displayedDto();
        for (const auto &kind : {"unit","reference","ordinal","unknown"}) {
            request(p,"fixture.bad",{{"kind",kind}}); w.queryPlot();
            QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("Invalid bounded")); QCOMPARE(w.displayedDto(),old);
        }
        const auto path=fixture.path()+"/preserved.csv"; QFile prior(path); QVERIFY(prior.open(QIODevice::WriteOnly)); prior.write("original-file"); prior.close();
        request(p,"fixture.bad",{{"kind","chunk"}}); w.exportToFile(path);
        QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("SHA-256 mismatch"));
        QVERIFY(prior.open(QIODevice::ReadOnly)); QCOMPARE(prior.readAll(),QByteArray("original-file"));
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void layoutsRestoreStableReferencesAndDiagnoseStalePins()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w);
        auto *channels=control<QComboBox>(w,"seriesChannelChoice"); channels->setCurrentIndex(1); oneSource(w);
        const auto selectorBefore=control<QListWidget>(w,"seriesSourceSelection")->item(0)->data(Qt::UserRole).toJsonObject();
        w.saveLayout(); QVERIFY(control<QLabel>(w,"seriesStatus")->text().contains("Layout saved"));
        request(p,"fixture.reverse"); w.refresh();
        QTRY_VERIFY2(channels->itemData(0).toJsonObject().value("channelId").toString() == "node:51000:velocity:Z",
            qPrintable(control<QLabel>(w,"seriesStatus")->text()));
        QCOMPARE(channels->currentData().toJsonObject(),selectorBefore);
        SeriesWorkbenchWidget reopened(&p); reopened.show();
        QTRY_COMPARE(control<QComboBox>(reopened,"seriesChannelChoice")->count(),4);
        reopened.restoreLayout(); QTRY_VERIFY(!reopened.displayedDto().isEmpty());
        QCOMPARE(control<QListWidget>(reopened,"seriesSourceSelection")->item(0)->data(Qt::UserRole).toJsonObject(),selectorBefore);
        request(p,"fixture.revision"); reopened.restoreLayout();
        QTRY_VERIFY(control<QLabel>(reopened,"seriesStatus")->text().contains("SOURCE_CHANGED"));
        QCOMPARE(control<QListWidget>(reopened,"seriesSourceSelection")->item(0)->data(Qt::UserRole).toJsonObject(),selectorBefore);
        p.stop(); QTRY_VERIFY(p.isStopped());
    }
    void heldSpectrumLayoutRestoresDisplayedMathAndKeepsFutureControls()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w);
        auto *view=control<QComboBox>(w,"seriesViewChoice");
        auto *window=control<QComboBox>(w,"seriesWindowChoice");
        auto *mean=control<QCheckBox>(w,"seriesRemoveMean");
        view->setCurrentIndex(view->findData("amplitude"));
        window->setCurrentIndex(window->findData("hann")); mean->setChecked(true);
        oneSource(w);
        const auto amplitude=w.displayedDto(); const auto points=w.plotWidget()->dataset().traces[0].points;
        const SeriesPlotWidget::VisibleRange range{.1,5.,10.,500.};
        QVERIFY(w.plotWidget()->setVisibleRange(range));
        QVERIFY(w.plotWidget()->setCursor({"trace-1",100}));
        w.plotWidget()->setDisplayHeld(true);
        view->setCurrentIndex(view->findData("psd"));
        window->setCurrentIndex(window->findData("rectangular")); mean->setChecked(false);
        w.queryPlot(); QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("pending"));
        QCOMPARE(w.displayedDto(),amplitude); QCOMPARE(w.plotWidget()->dataset().traces[0].points,points);
        w.saveLayout(); QVERIFY(control<QLabel>(w,"seriesStatus")->text().contains("Layout saved"));

        SeriesWorkbenchWidget reopened(&p); reopened.show();
        QTRY_COMPARE(control<QComboBox>(reopened,"seriesChannelChoice")->count(),4);
        reopened.restoreLayout(); QTRY_VERIFY(!reopened.displayedDto().isEmpty());
        QCOMPARE(reopened.displayedDto(),amplitude);
        QCOMPARE(reopened.plotWidget()->dataset().traces[0].points,points);
        QCOMPARE(reopened.plotWidget()->dataset().yUnit,QString("m/s"));
        QVERIFY(reopened.plotWidget()->isDisplayHeld());
        const auto restoredRange=reopened.plotWidget()->visibleRange();
        QVERIFY(std::abs(restoredRange.xMinimum-range.xMinimum)<1e-10);
        QVERIFY(std::abs(restoredRange.xMaximum-range.xMaximum)<1e-10);
        QVERIFY(std::abs(restoredRange.yMinimum-range.yMinimum)<1e-10);
        QVERIFY(std::abs(restoredRange.yMaximum-range.yMaximum)<1e-10);
        QVERIFY(reopened.plotWidget()->cursor().has_value());
        QCOMPARE(reopened.plotWidget()->cursor()->traceId,QString("trace-1"));
        QCOMPARE(reopened.plotWidget()->cursor()->ordinal,quint64(100));
        QCOMPARE(control<QComboBox>(reopened,"seriesViewChoice")->currentData().toString(),QString("psd"));
        QCOMPARE(control<QComboBox>(reopened,"seriesWindowChoice")->currentData().toString(),QString("rectangular"));
        QVERIFY(!control<QCheckBox>(reopened,"seriesRemoveMean")->isChecked());
        const auto restoreCall=calls(p,"result.plot.query").last().toObject().value("parameters").toObject();
        QCOMPARE(restoreCall.value("view").toString(),QString("amplitude"));
        QCOMPARE(restoreCall.value("options").toObject().value("window").toString(),QString("hann"));
        QCOMPARE(restoreCall.value("options").toObject().value("removeMean"),QJsonValue(true));
        QCOMPARE(restoreCall.value("sources"),amplitude.value("export").toObject().value("sources"));

        reopened.queryPlot(); QTRY_VERIFY(control<QLabel>(reopened,"seriesStatus")->text().contains("pending"));
        QCOMPARE(reopened.displayedDto(),amplitude);
        reopened.plotWidget()->setDisplayHeld(false);
        QCOMPARE(reopened.displayedDto().value("view").toString(),QString("psd"));
        const auto trace=reopened.displayedDto().value("traces").toArray()[0].toObject();
        QCOMPARE(trace.value("window").toString(),QString("rectangular"));
        QCOMPARE(trace.value("removeMean"),QJsonValue(false));
        QVERIFY(reopened.plotWidget()->dataset().traces[0].points!=points);
        p.stop(); QTRY_VERIFY(p.isStopped());
    }

    void workViewAllowsOnlyExactActiveJobScalarTransformation()
    {
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); start(p,w); oneSource(w);
        auto *view = control<QComboBox>(w,"seriesViewChoice");
        view->setCurrentIndex(view->findData("work")); w.queryPlot();
        QTRY_COMPARE(w.displayedDto().value("view").toString(), QString("work"));
        const auto selector = w.displayedDto().value("traces").toArray().first().toObject()
            .value("sources").toArray().first().toObject().value("selector").toObject();
        QCOMPARE(selector.value("channelId").toString(), QString("control:work"));
        QCOMPARE(selector.value("mode").toString(), QString("active"));
        const auto old = w.displayedDto();
        for (const auto &bad : {QString("work-job"), QString("work-mode"), QString("work-artifact")})
        {
            request(p,"fixture.bad",{{"kind",bad}}); w.queryPlot();
            QTRY_VERIFY(control<QLabel>(w,"seriesStatus")->text().contains("identity mismatch"));
            QCOMPARE(w.displayedDto(), old);
        }
        p.stop(); QTRY_VERIFY(p.isStopped());
    }

    void actualTwoJobNativeCurvesReferenceAndCompleteCsvThroughGui()
    {
#if defined(RANGE_SERIES_PACKAGE) && defined(RANGE_SERIES_SOLVER) && defined(RANGE_SERIES_NATIVE) && defined(RANGE_SERIES_EXAMPLES)
        QTemporaryDir actualFixture; QVERIFY(actualFixture.isValid());
        auto actual = WorkflowProcess::defaultConfiguration(actualFixture.path()+"/workspace");
        actual.python=QString::fromUtf8(RANGE_SERIES_PYTHON); actual.packageDirectory=QString::fromUtf8(RANGE_SERIES_PACKAGE);
        actual.solver=QString::fromUtf8(RANGE_SERIES_SOLVER); actual.native=QString::fromUtf8(RANGE_SERIES_NATIVE);
        actual.examples=QString::fromUtf8(RANGE_SERIES_EXAMPLES);
        QProcess bootstrap; auto env=QProcessEnvironment::systemEnvironment(); env.insert("PYTHONPATH",actual.packageDirectory);
        env.insert("PYTHONNOUSERSITE","1"); bootstrap.setProcessEnvironment(env);
        bootstrap.start(actual.python,{QDir(actual.packageDirectory).absoluteFilePath("../tests/task_widget_bootstrap.py"),
            "--root",actualFixture.path(),"--solver",actual.solver,"--native",actual.native,"--examples",actual.examples,"--python",actual.python});
        QVERIFY(bootstrap.waitForStarted(10000)); QVERIFY(bootstrap.waitForFinished(120000));
        QVERIFY2(bootstrap.exitCode()==0,bootstrap.readAllStandardError().constData());
        QFile manifest(actualFixture.path()+"/fixture.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto source=QJsonDocument::fromJson(manifest.readAll()).object();
        WorkflowProcess p; SeriesWorkbenchWidget w(&p); w.resize(1200,950); w.show(); p.start(actual);
        QTRY_VERIFY_WITH_TIMEOUT(p.isReady(),30000);
        auto execute=[&](const QString &command,const QJsonObject &arguments) {
            const auto project=request(p,"project.query");
            const auto preview=request(p,"command.preview",{{"command",command},{"parameters",arguments},{"expectedRevision",project.value("revision")}});
            const auto approval=request(p,"host.approve",{{"previewId",preview.value("previewId")},{"previewHash",preview.value("previewHash")}});
            const auto submitted=request(p,"command.execute",{{"previewId",preview.value("previewId")},{"approvalToken",approval.value("approvalToken")}});
            const auto id=submitted.value("jobId").toString(); QJsonObject job; QElapsedTimer clock; clock.start();
            do { job=request(p,"jobs.query",{{"jobId",id}}).value("job").toObject();
                if (job.value("state")!="queued" && job.value("state")!="running") break; QTest::qWait(20);
            } while(clock.elapsed()<90000);
            return job;
        };
        QStringList nativeJobs;
        for(const double damping:{.05,.1}) {
            const auto configured=execute("workflow.configure",{{"dt",.005},{"steps",20},{"targetDamping",damping},{"sourceId",source.value("sourceId")}});
            QCOMPARE(configured.value("state").toString(),QString("succeeded"));
            const auto job=execute("workflow.run",{{"mode","both"},{"independentReference",true}});
            QCOMPARE(job.value("state").toString(),QString("succeeded")); nativeJobs<<job.value("jobId").toString();
        }
        w.refresh(); auto *jobChoice=control<QComboBox>(w,"seriesJobChoice");
        auto *modeChoice=control<QComboBox>(w,"seriesModeChoice"); auto *channelChoice=control<QComboBox>(w,"seriesChannelChoice");
        QTRY_COMPARE_WITH_TIMEOUT(jobChoice->count(),2,30000);
        QString velocityChannel;
        for(const auto &id:nativeJobs) for(const auto &mode:{QString("active"),QString("passive")}) {
            jobChoice->setCurrentIndex(jobChoice->findData(id)); modeChoice->setCurrentIndex(modeChoice->findData(mode));
            QTRY_VERIFY_WITH_TIMEOUT(channelChoice->count()>0,30000);
            int found=-1;
            for(int i=0;i<channelChoice->count();++i) {
                const auto row=channelChoice->itemData(i,Qt::UserRole+1).toJsonObject();
                const auto selector=row.value("selector").toObject();
                if(selector.value("jobId")!=id || selector.value("mode")!=mode) continue;
                const auto channel=selector.value("channelId").toString();
                if((velocityChannel.isEmpty() && row.contains("reference")) || channel==velocityChannel) {found=i; velocityChannel=channel;break;}
            }
            QTRY_VERIFY_WITH_TIMEOUT(found>=0,30000);
            channelChoice->setCurrentIndex(found); click(w,"seriesAddNative");
        }
        QCOMPARE(control<QListWidget>(w,"seriesSourceSelection")->count(),4);
        w.queryPlot(); QTRY_COMPARE_WITH_TIMEOUT(w.plotWidget()->dataset().traces.size(),4,30000);
        for(const auto &trace:w.displayedDto().value("traces").toArray()) QCOMPARE(trace.toObject().value("rawSampleCount").toInt(),20);
        const QString output=actualFixture.path()+"/four-native-complete.csv"; w.exportToFile(output);
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(w,"seriesStatus")->text().contains("Exported 80"),30000);
        QFile csv(output); QVERIFY(csv.open(QIODevice::ReadOnly)); const auto bytes=csv.readAll();
        QCOMPARE(bytes.split('\n').size(),82); // Header, 80 real raw rows, newline.
        const QString evidence=qEnvironmentVariable("RANGE_SERIES_GUI_EVIDENCE_DIRECTORY");
        if(!evidence.isEmpty()) { QVERIFY(QDir().mkpath(evidence)); QVERIFY(w.grab().save(evidence+"/four-native-curves.png")); }
        p.stop(); QTRY_VERIFY(p.isStopped());
#else
        QFAIL("CMake must provide actual native workbench paths");
#endif
    }
};
int runSeriesWorkbenchWidgetTests(int argc,char **argv)
{ SeriesWorkbenchWidgetTests tests; return QTest::qExec(&tests,argc,argv); }
#include "tst_series_workbench_widget.moc"
