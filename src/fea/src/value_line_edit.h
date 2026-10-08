#ifndef VALUE_LINE_EDIT_H
#define VALUE_LINE_EDIT_H

#include <QLineEdit>
#include <QDoubleValidator>

class ValueLineEdit : public QLineEdit
{
    Q_OBJECT

    public:

        //! Constructor.
        explicit ValueLineEdit(QWidget *parent = nullptr);

        //! Constructor.
        explicit ValueLineEdit(double min, double max, QWidget *parent = nullptr);

        //! Constructor.
        explicit ValueLineEdit(int min, int max, QWidget *parent = nullptr);

        //! Set double validator.
        void setDoubleValidator();

        //! Set int validator.
        void setIntValidator();

        //! Set range.
        void setRange(double min, double max);

        //! Set range.
        void setRange(int min, int max);

        //! Return minimum value.
        double getMinimum() const;

        //! Return maximum value.
        double getMaximum() const;

        //! Return value.
        double getValue() const;

        //! Set new value.
        void setValue(double value);

        //! Set new value.
        void setValue(int value);

        //! Set new value.
        void setValue(uint value);

        //! Restore an in-progress edit without emitting an accepted value.
        void restoreDraftText(const QString &text);

        //! Return validator state.
        QValidator::State getValidatorState() const;

    private:

        //! Paint background based on entered value.
        void paintBackground();
        
    signals:

        //! Value changed signal.
        void valueChanged(double value);

        //! Text validity changed; the text is UI-local draft data.
        void draftStateChanged(const QString &state, const QString &text);


    private slots:

        //! Catch signal textChanged
        void onTextChaged(QString text);

};

#endif /* VALUE_LINE_EDIT_H */
