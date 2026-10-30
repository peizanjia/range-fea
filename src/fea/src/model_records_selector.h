#ifndef MODEL_RECORDS_SELECTOR_H
#define MODEL_RECORDS_SELECTOR_H

#include <QWidget>
#include <QAction>
#include <QSpinBox>

#include "model_records_tree.h"
#include "video_settings.h"

class ModelRecordsSelector : public QWidget
{

    Q_OBJECT

    protected:

        //! Play action.
        QAction *playAction;
        //! Record action.
        QAction *recordAction;
        //! Ignore completions and queued advances from an earlier playback.
        quint64 playbackGeneration;
        //! Loads/captures in the current record batch that have not completed.
        uint pendingRecordUpdates;
        //! Mark next record.
        bool markNextIndicator;
        //! Record screenshot indicator.
        bool recordIndicator;
        //! Model records tree.
        ModelRecordsTree *tree;
        //! Start record spin box.
        QSpinBox *startSpin;
        //! End record spin box.
        QSpinBox *endSpin;
        //! Video settings.
        VideoSettings videoSettings;

    public:

        //! Constructor.
        explicit ModelRecordsSelector(QWidget *parent = nullptr);

    protected:

        //! Create animation from screenshots.
        void createAnimation(uint modelID);

        //! Complete one loaded record after its fresh screenshot is saved.
        void finishRecordUpdate(quint64 generation);

        //! Restore controls and invalidate outstanding playback callbacks.
        void stopPlayback(const QString &error = QString());

        //! Wait for the loaded model's next rendered frame before capturing it.
        void onUpdateJobFinished(uint modelID, quint64 generation);

    protected slots:

        //! Record markeded.
        void onRecordMarked(uint modelID, const QString &recordFileName);

        //! On record has finished.
        void onRecordFinished();

        //! Update the start/end record range limits from the tree.
        void updateRecordRange();

        //! Play/Pause.
        void playToggle(bool jumpToFirst = false);

        //! Load next record.
        void loadNextRecord(bool jumpToFirst = false);

        //! On record video toggled.
        void onRecordVideo();

        //! Video recording has finished.
        void onVideoEncodeFinished(QString fileName);

        //! Video recording has failed.
        void onVideoEncodeFailed();

    signals:

        //! Signal to load next record.
        void recordFinished();
    
};

#endif // MODEL_RECORDS_SELECTOR_H
