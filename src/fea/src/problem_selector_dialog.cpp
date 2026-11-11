#include <QVBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDialogButtonBox>

#include <rbl_logger.h>

#include "problem_selector_dialog.h"

ProblemSelectorDialog::ProblemSelectorDialog(QWidget *parent) :
    QDialog(parent)
{
    R_LOG_TRACE_IN;
    this->setWindowTitle(tr("Problem type selector"));
    this->resize(560, 420);
    this->setSizeGripEnabled(true);

    QIcon cancelIcon(":/icons/action/pixmaps/range-cancel.svg");
    QIcon okIcon(":/icons/action/pixmaps/range-ok.svg");

    QVBoxLayout *mainLayout = new QVBoxLayout;
    this->setLayout (mainLayout);

    QLabel *messageLabel = new QLabel(tr("Check the analysis types to add"));
    mainLayout->addWidget(messageLabel);

    this->problemTree = new ProblemSelectorTree(this);
    this->problemTree->setAccessibleName(tr("Analysis types"));
    mainLayout->addWidget(this->problemTree, 1);

    auto *descriptionLabel = new QLabel(tr("&Description"), this);
    auto *description = new QPlainTextEdit(this);
    description->setObjectName(QStringLiteral("problemTypeDescription"));
    description->setAccessibleName(tr("Description of the highlighted analysis type"));
    description->setReadOnly(true);
    description->setTabChangesFocus(true);
    description->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    description->setMaximumHeight(6 * description->fontMetrics().lineSpacing() + 2 * description->frameWidth());
    descriptionLabel->setBuddy(description);
    mainLayout->addWidget(descriptionLabel);
    mainLayout->addWidget(description);

    connect(this->problemTree, &QTreeWidget::currentItemChanged, description, [this, description] {
        description->setPlainText(this->problemTree->currentDescription());
    });
    this->problemTree->setCurrentItem(this->problemTree->topLevelItem(0));
    this->problemTree->setFocus(Qt::OtherFocusReason);

    QDialogButtonBox *buttonBox = new QDialogButtonBox;
    mainLayout->addWidget(buttonBox);

    QPushButton *cancelButton = new QPushButton(cancelIcon, tr("Cancel"));
    buttonBox->addButton(cancelButton,QDialogButtonBox::RejectRole);

    this->okButton = new QPushButton(okIcon, tr("OK"));
    this->okButton->setObjectName(QStringLiteral("confirmProblemTypes"));
    this->okButton->setDisabled(true);
    buttonBox->addButton(this->okButton,QDialogButtonBox::AcceptRole);

    QObject::connect(buttonBox,&QDialogButtonBox::rejected,this,&QDialog::reject);
    QObject::connect(buttonBox,&QDialogButtonBox::accepted,this,&QDialog::accept);

    QObject::connect(this->problemTree,&ProblemSelectorTree::changed,this,&ProblemSelectorDialog::onProblemTreeChanged);
    R_LOG_TRACE_OUT;
}

RProblemTypeMask ProblemSelectorDialog::exec()
{
    R_LOG_TRACE_IN;
    if (QDialog::exec() == QDialog::Accepted)
    {
        R_LOG_TRACE_RETURN(this->problemTree->findProblemTypeMask());
    }

    R_LOG_TRACE_RETURN(R_PROBLEM_NONE);
}

void ProblemSelectorDialog::onProblemTreeChanged()
{
    R_LOG_TRACE_IN;
    this->okButton->setEnabled(this->problemTree->checkProblemIsChecked());
    R_LOG_TRACE_OUT;
}
