#include <QAction>
#include <QIcon>
#include <QVBoxLayout>
#include <QToolBar>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <rbl_job_manager.h>
#include <rbl_progress.h>

#include <rml_file_utils.h>

#include "model_records_selector.h"
#include "model_io.h"
#include "application.h"
#include "video_settings_dialog.h"
#include "video_encoder.h"
#include "gl_widget.h"

ModelRecordsSelector::ModelRecordsSelector(QWidget *parent)
    : QWidget(parent)
    , playbackGeneration(0)
    , pendingRecordUpdates(0)
    , markNextIndicator(false)
    , recordIndicator(false)
{
    QVBoxLayout *layout = new QVBoxLayout;
    layout->setSpacing(0);
    layout->setContentsMargins(0,0,0,0);
    this->setLayout(layout);

    this->tree = new ModelRecordsTree;
    layout->addWidget(this->tree);

    QObject::connect(this->tree,
                     &ModelRecordsTree::recordMarked,
                     this,
                     &ModelRecordsSelector::onRecordMarked);

    QToolBar *toolBar = new QToolBar;
    layout->addWidget(toolBar);

    QList<QAction*> actions;

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_first.svg"),"First",this));
    QObject::connect(actions.last(), &QAction::triggered, this->tree, &ModelRecordsTree::markFirst);

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_backward.svg"),"Backward",this));
    QObject::connect(actions.last(), &QAction::triggered, this->tree, &ModelRecordsTree::markPrevious);

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_play.svg"),"Play",this));
    // Start playback at the selected "From" record (jumpToFirst = true); pausing ignores it.
    QObject::connect(actions.last(), &QAction::triggered, this, [this](){ this->playToggle(true); });
    this->playAction = actions.last();

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_forward.svg"),"Forward",this));
    QObject::connect(actions.last(), &QAction::triggered, this->tree, &ModelRecordsTree::markNext);

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_last.svg"),"Last",this));
    QObject::connect(actions.last(), &QAction::triggered, this->tree, &ModelRecordsTree::markLast);

    toolBar->addActions(actions);

    QWidget* empty = new QWidget();
    empty->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    toolBar->addWidget(empty);

    actions.clear();

    actions.append(new QAction(QIcon(":/icons/media/pixmaps/range-play_record.svg"),"Record",this));
    QObject::connect(actions.last(), &QAction::triggered, this, &ModelRecordsSelector::onRecordVideo);
    this->recordAction = actions.last();

    toolBar->addActions(actions);

    QToolBar *rangeToolBar = new QToolBar;
    layout->addWidget(rangeToolBar);

    this->startSpin = new QSpinBox;
    this->startSpin->setMinimum(1);
    this->startSpin->setMaximum(1);
    this->startSpin->setPrefix(tr("From") + " ");
    this->startSpin->setToolTip(tr("First record to play / record"));
    rangeToolBar->addWidget(this->startSpin);

    this->endSpin = new QSpinBox;
    this->endSpin->setMinimum(1);
    this->endSpin->setMaximum(1);
    this->endSpin->setPrefix(tr("To") + " ");
    this->endSpin->setToolTip(tr("Last record to play / record"));
    rangeToolBar->addWidget(this->endSpin);

    // Keep the end record at or after the start record.
    QObject::connect(this->startSpin,
                     QOverload<int>::of(&QSpinBox::valueChanged),
                     this->endSpin,
                     &QSpinBox::setMinimum);

    // Highlight the records inside the from-to range as it changes.
    auto highlightRange = [this](){
        this->tree->setHighlightRange(uint(this->startSpin->value()),uint(this->endSpin->value()));
    };
    QObject::connect(this->startSpin,QOverload<int>::of(&QSpinBox::valueChanged),this,highlightRange);
    QObject::connect(this->endSpin,QOverload<int>::of(&QSpinBox::valueChanged),this,highlightRange);

    // Range set from the tree's context menu.
    QObject::connect(this->tree,
                     &ModelRecordsTree::setFromRequested,
                     this,
                     [this](uint recordNumber){ this->startSpin->setValue(int(recordNumber)); });
    QObject::connect(this->tree,
                     &ModelRecordsTree::setToRequested,
                     this,
                     [this](uint recordNumber){ this->endSpin->setValue(int(recordNumber)); });
    QObject::connect(this->tree,
                     &ModelRecordsTree::setRangeRequested,
                     this,
                     [this](uint first, uint last){
                         this->startSpin->setValue(int(first));
                         this->endSpin->setValue(int(last));
                     });

    QObject::connect(this->tree,
                     &ModelRecordsTree::recordCountChanged,
                     this,
                     &ModelRecordsSelector::updateRecordRange);
    this->updateRecordRange();
    highlightRange();

    QObject::connect(this,&ModelRecordsSelector::recordFinished,this,&ModelRecordsSelector::onRecordFinished);
}

void ModelRecordsSelector::createAnimation(uint modelID)
{
    Model &rModel = Application::instance()->getSession()->getModel(modelID);
    std::vector<double> &rTimes = rModel.getTimeSolver().getTimes();
    QList<QString> imageFileNames;

    uint firstRecord = uint(this->startSpin->value());
    uint lastRecord = uint(this->endSpin->value());

    for (uint j=0;j<rTimes.size();j++)
    {
        uint recordNumber = j+1;
        // Only encode the records that were just played/recorded, i.e. those
        // inside the selected from-to range.  Screen-shots left over from earlier
        // recordings also exist on disk and must be skipped.
        if (recordNumber < firstRecord || recordNumber > lastRecord)
        {
            continue;
        }

        QString recordFileName(RFileUtils::getFileNameWithTimeStep(rModel.getFileName(),recordNumber));
        QString imageFileName(RFileUtils::getFileNameWithTimeStep(rModel.buildScreenShotFileName(),recordNumber));
        if (RFileUtils::fileExists(recordFileName) && RFileUtils::fileExists(imageFileName))
        {
            imageFileNames.append(imageFileName);
        }
    }

    if (imageFileNames.size() <= 0)
    {
        return;
    }

    try
    {
        QQueue<QImage> imageQueue;

        uint cnt=0;
        foreach (const QString &imageFileName, imageFileNames)
        {
            RLogger::info("Adding image file \'%s\'\n",imageFileName.toUtf8().constData());

            QImage image;
            if (!image.load(imageFileName))
            {
                RLogger::error("Failed to load image file \'%s\'\n",imageFileName.toUtf8().constData());
                return;
            }

            for (uint i=0;i<this->videoSettings.getFpi();i++)
            {
                imageQueue.enqueue(image);
            }
            RProgressPrint(cnt++,imageFileNames.size());
        }

        VideoEncoder *videoEncoder = new VideoEncoder(rModel.buildAnimationFileName(),imageQueue,this->videoSettings,this);
        QObject::connect(videoEncoder,&VideoEncoder::finished,this,&ModelRecordsSelector::onVideoEncodeFinished);
        QObject::connect(videoEncoder,&VideoEncoder::finished,videoEncoder,&QObject::deleteLater);
        QObject::connect(videoEncoder,&VideoEncoder::failed,this,&ModelRecordsSelector::onVideoEncodeFailed);
        QObject::connect(videoEncoder,&VideoEncoder::failed,videoEncoder,&QObject::deleteLater);
    }
    catch (const RError &rError)
    {
        RLogger::error("Failed to create animation file:  %s\n", rError.getMessage().toUtf8().constData());
        return;
    }
}

void ModelRecordsSelector::onRecordMarked(uint modelID, const QString &recordFileName)
{
    RLogger::info("Selected record: \'%s\'\n", recordFileName.toUtf8().constData());

    RJob *updateJob = new ModelIO(MODEL_IO_UPDATE, recordFileName, &Application::instance()->getSession()->getModel(modelID));
    const quint64 generation = this->playbackGeneration;
    ++this->pendingRecordUpdates;

    QObject::connect(updateJob,
                     &RJob::finished,
                     this,
                     [this, modelID, generation]() { this->onUpdateJobFinished(modelID, generation); });
    QObject::connect(updateJob, &RJob::failed, this, [this, generation]() {
        if (generation == this->playbackGeneration)
            this->stopPlayback(tr("The selected result record could not be loaded."));
    });

    RJobManager::getInstance().submit(updateJob);
}

void ModelRecordsSelector::onRecordFinished()
{
    const quint64 generation = this->playbackGeneration;
    QTimer::singleShot(this->recordIndicator ? 0 : 1000, this, [this, generation]() {
        if (generation == this->playbackGeneration && this->markNextIndicator)
            this->loadNextRecord();
    });
}

void ModelRecordsSelector::finishRecordUpdate(quint64 generation)
{
    if (generation != this->playbackGeneration || this->pendingRecordUpdates == 0)
        return;
    --this->pendingRecordUpdates;
    if (this->pendingRecordUpdates == 0 && this->markNextIndicator)
        emit this->recordFinished();
}

void ModelRecordsSelector::stopPlayback(const QString &error)
{
    if (!error.isEmpty())
        RLogger::error("Playback/recording stopped: %s\n", error.toUtf8().constData());
    ++this->playbackGeneration;
    this->pendingRecordUpdates = 0;
    this->markNextIndicator = false;
    this->recordIndicator = false;
    this->tree->setEnabled(true);
    this->playAction->setIcon(QIcon(":/icons/media/pixmaps/range-play_play.svg"));
    this->playAction->setText("Play");
    this->recordAction->setEnabled(true);
    Application::instance()->getMainWindow()->progressAutoHideEnable();
}

void ModelRecordsSelector::onUpdateJobFinished(uint modelID, quint64 generation)
{
    if (generation != this->playbackGeneration)
        return;
    if (!this->recordIndicator)
    {
        this->finishRecordUpdate(generation);
        return;
    }

    auto *session = Application::instance()->getSession();
    GLWidget *view = nullptr;
    for (auto *candidate : Application::instance()->getMainWindow()->findChildren<GLWidget *>())
        if (candidate->getModelID() == modelID) { view = candidate; break; }
    if (!view)
    {
        this->stopPlayback(tr("No model view is available for the loaded result record."));
        return;
    }
    const auto stamp = session->modelCommandStamp(modelID);
    const QString fileName = session->getModel(modelID).buildScreenShotFileName();
    // A stale image must never make a failed capture look successful.
    if (QFileInfo::exists(fileName) && !QFile::remove(fileName))
    {
        this->stopPlayback(tr("The previous screenshot could not be replaced: %1").arg(fileName));
        return;
    }

    auto *capture = new QObject(this);
    auto *timeout = new QTimer(capture);
    timeout->setObjectName(QStringLiteral("recordFrameTimeout"));
    timeout->setSingleShot(true);
    const auto failed = [this, capture, generation](const QString &error) {
        capture->deleteLater();
        if (generation == this->playbackGeneration) this->stopPlayback(error);
    };
    QObject::connect(timeout, &QTimer::timeout, capture, [failed]() {
        failed(tr("Timed out waiting for the loaded result record to be rendered; no video was encoded."));
    });
    QObject::connect(view, &QObject::destroyed, capture, [failed]() {
        failed(tr("The model view closed before its result frame was captured."));
    });
    QObject::connect(view, &QOpenGLWidget::frameSwapped, capture,
        [this, capture, timeout, modelID, generation, stamp, fileName]() {
            timeout->stop();
            capture->deleteLater();
            if (generation != this->playbackGeneration) return;
            auto *currentSession = Application::instance()->getSession();
            if (currentSession->findModelByCommandIdentity(stamp.first) != modelID
                || currentSession->modelCommandStamp(modelID) != stamp)
            {
                this->stopPlayback(tr("The model changed before its result frame was captured."));
                return;
            }
            // This GUI-thread signal returns after the image save. Only advance
            // once a newly rendered, newly saved image can actually be read.
            currentSession->setTakeScreenShot(modelID, fileName, false);
            if (QImage(fileName).isNull())
            {
                this->stopPlayback(tr("The result screenshot could not be saved: %1").arg(fileName));
                return;
            }
            this->finishRecordUpdate(generation);
        }, Qt::SingleShotConnection);
    timeout->start(10000);
    view->update();
}

void ModelRecordsSelector::updateRecordRange()
{
    // Record numbers can be sparse (e.g. 2,10,...,102), so the spin boxes range
    // over the actual first/last record numbers rather than a 1..N count.
    uint first = this->tree->getFirstRecordNumber();
    uint last = this->tree->getLastRecordNumber();
    if (last < first || last < 1)
    {
        first = 1;
        last = 1;
    }

    // Preserve a full-range end selection so the default keeps playing through
    // to the last record as records are added.
    bool endWasFull = (this->endSpin->value() >= this->endSpin->maximum());

    this->startSpin->setRange(int(first),int(last));
    this->endSpin->setMaximum(int(last));
    this->endSpin->setMinimum(this->startSpin->value());

    if (endWasFull)
    {
        this->endSpin->setValue(int(last));
    }
}

void ModelRecordsSelector::playToggle(bool jumpToFirst)
{
    if (!this->markNextIndicator)
    {
        ++this->playbackGeneration;
        this->pendingRecordUpdates = 0;
        this->markNextIndicator = true;
        this->tree->setEnabled(false);
        Application::instance()->getMainWindow()->progressAutoHideDisable();
        this->playAction->setIcon(QIcon(":/icons/media/pixmaps/range-play_pause.svg"));
        this->playAction->setText("Pause");

        this->loadNextRecord(jumpToFirst);
    }
    else
    {
        this->stopPlayback();
    }
}

void ModelRecordsSelector::loadNextRecord(bool jumpToFirst)
{
    // Stop when there is no next record, or advancing would step past the "To"
    // record.  Checking the next record (rather than the current one) avoids
    // loading the record just beyond the end of the range.
    uint nextRecordNumber = this->tree->getNextRecordNumber();
    if (!jumpToFirst && (nextRecordNumber == 0 ||
                         nextRecordNumber > uint(this->endSpin->value()) ||
                         !this->markNextIndicator))
    {
        if (this->recordIndicator)
        {
            QList<uint> selectedModelIDs = Application::instance()->getSession()->getSelectedModelIDs();
            for (int i=0;i<selectedModelIDs.size();i++)
            {
                RProgressInitialize("Creating a video file");
                this->createAnimation(selectedModelIDs.at(i));
                RProgressFinalize();
            }
        }
        this->stopPlayback();
    }
    else
    {
        if (jumpToFirst)
        {
            this->tree->markRecord(uint(this->startSpin->value()));
        }
        else
        {
            this->tree->markNext();
        }
    }
}

void ModelRecordsSelector::onRecordVideo()
{
    uint numberOfRecords = this->tree->getRecordCountInRange(uint(this->startSpin->value()),
                                                             uint(this->endSpin->value()));
    VideoSettingsDialog videoSettingsDialog(this->videoSettings,numberOfRecords);
    if (videoSettingsDialog.exec() == QDialog::Accepted)
    {
        if (this->markNextIndicator) this->stopPlayback();
        this->videoSettings = videoSettingsDialog.getVideoSettings();
        this->recordIndicator = true;
        this->recordAction->setDisabled(true);
        this->playToggle(true);
    }
}

void ModelRecordsSelector::onVideoEncodeFinished(QString fileName)
{
    RLogger::info("Video has been successfully encoded (file: \"%s\").\n",fileName.toUtf8().constData());
}

void ModelRecordsSelector::onVideoEncodeFailed()
{
    RLogger::error("Video encoding has failed.\n");
}
