#ifndef MODAL_SETUP_WIDGET_H
#define MODAL_SETUP_WIDGET_H

#include <QWidget>
#include <QSpinBox>

#include <rml_modal_setup.h>

class ModalSetupWidget : public QWidget
{
    Q_OBJECT

    protected:

        //! Modal setup.
        RModalSetup modalSetup;
        //! Number of iterations spin box.
        QSpinBox *spinNIterations;
        //! Number of modes to extract spin box.
        QSpinBox *spinNModesExtract;

    public:

        //! Constructor.
        explicit ModalSetupWidget(const RModalSetup &modalSetup, QWidget *parent = nullptr);

    signals:

        //! Modal setup has changed.
        void changed(const RModalSetup &modalSetup);

        //! Numeric editor draft state, with no accepted value or raw persistence.
        void draftStateChanged(const QString &field, const QString &state, const QString &text);

    private slots:

        void onModalMethodChanged(int index);

        void onNIterationsChanged(int nIterations);

        void onNModesExtractChanged(int nModes);

        void onConvergenceValueChanged(double convergenceValue);

        void onConvergenceDraftChanged(const QString &state, const QString &text);

};

#endif // MODAL_SETUP_WIDGET_H
