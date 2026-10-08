#include <cmath>
#include <cfloat>
#include <QLocale>
#include <QSignalBlocker>

#include "value_line_edit.h"

ValueLineEdit::ValueLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
    this->setDoubleValidator();
    QObject::connect(this,&QLineEdit::textChanged,this,&ValueLineEdit::onTextChaged);
}

ValueLineEdit::ValueLineEdit(double min, double max, QWidget *parent)
    : QLineEdit(parent)
{
    this->setValidator(new QDoubleValidator(min,max,1000,this));
    QObject::connect(this,&QLineEdit::textChanged,this,&ValueLineEdit::onTextChaged);
}

ValueLineEdit::ValueLineEdit(int min, int max, QWidget *parent)
    : QLineEdit(parent)
{
    this->setValidator(new QIntValidator(min,max,this));
    QObject::connect(this,&QLineEdit::textChanged,this,&ValueLineEdit::onTextChaged);
}

void ValueLineEdit::setDoubleValidator()
{
    this->setValidator(new QDoubleValidator(-DBL_MAX,DBL_MAX,1000,this));
    this->paintBackground();
}

void ValueLineEdit::setIntValidator()
{
    this->setValidator(new QIntValidator(INT_MIN,INT_MAX,this));
    this->paintBackground();
}

void ValueLineEdit::setRange(double min, double max)
{
    this->setValidator(new QDoubleValidator(min,max,1000,this));
    this->paintBackground();
}

void ValueLineEdit::setRange(int min, int max)
{
    this->setValidator(new QIntValidator(min,max,this));
    this->paintBackground();
}

double ValueLineEdit::getMinimum() const
{
    return qobject_cast<const QDoubleValidator *>(this->validator())->bottom();
}

double ValueLineEdit::getMaximum() const
{
    return qobject_cast<const QDoubleValidator *>(this->validator())->top();
}

double ValueLineEdit::getValue() const
{
    return QLocale().toDouble(this->text());
}

void ValueLineEdit::setValue(double value)
{
    this->setText(QLocale().toString(value));
}

void ValueLineEdit::setValue(int value)
{
    this->setText(QLocale().toString(value));
}

void ValueLineEdit::setValue(uint value)
{
    this->setText(QLocale().toString(value));
}

void ValueLineEdit::restoreDraftText(const QString &text)
{
    const QSignalBlocker blocker(this);
    this->setText(text);
    this->paintBackground();
}

QValidator::State ValueLineEdit::getValidatorState() const
{
    if (!this->validator()) return QValidator::Acceptable;
    QString content = this->text();
    int position = content.size();
    return this->validator()->validate(content, position);
}

void ValueLineEdit::paintBackground()
{
    switch (this->getValidatorState())
    {
        case QValidator::Invalid:
        {
            this->setStyleSheet("QLineEdit{background: red;}");
            break;
        }
        case QValidator::Intermediate:
        {
            this->setStyleSheet("QLineEdit{background: rgb(255, 200, 200);}");
            break;
        }
        case QValidator::Acceptable:
        default:
        {
            this->setStyleSheet("");
            break;
        }
    }
}

void ValueLineEdit::onTextChaged(QString text)
{
    this->paintBackground();
    const QValidator::State state = this->getValidatorState();
    if (state == QValidator::Invalid)
    {
        emit this->draftStateChanged(QStringLiteral("invalid"), text);
        return;
    }
    if (state == QValidator::Intermediate)
    {
        emit this->draftStateChanged(QStringLiteral("intermediate"), text);
        return;
    }
    bool ok = false;
    const double value = QLocale().toDouble(text, &ok);
    if (!ok || !std::isfinite(value))
    {
        emit this->draftStateChanged(QStringLiteral("invalid"), text);
        return;
    }
    emit this->draftStateChanged(QStringLiteral("none"), {});
    emit this->valueChanged(value);
}
