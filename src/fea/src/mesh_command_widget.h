#ifndef MESH_COMMAND_WIDGET_H
#define MESH_COMMAND_WIDGET_H

#include <QWidget>
#include <QJsonObject>
#include <QPointer>

class Session;
class MeshJobController;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QPlainTextEdit;
class MeshBindingWidget;

//! Trusted visible GUI boundary; previews and approval are separate actions.
class MeshCommandWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MeshCommandWidget(QWidget *parent = nullptr);
    void setSession(Session *session);
    //! Share the host-owned controller rather than taking a second workspace lease.
    void setController(MeshJobController *controller);
    MeshJobController *controller() const;

public slots:
    void previewGeneration();
    void approveGeneration();
    void previewAcceptance();
    void approveAcceptance();
    void cancelSelected();
    void refreshContext();

private:
    QPointer<Session> session;
    QPointer<MeshJobController> jobController;
    QLabel *source = nullptr, *status = nullptr;
    QLineEdit *maxVolume = nullptr, *radiusEdgeRatio = nullptr;
    QPushButton *generatePreview = nullptr, *generateApprove = nullptr;
    QPushButton *acceptPreview = nullptr, *acceptApprove = nullptr, *cancelButton = nullptr;
    QTableWidget *jobList = nullptr;
    QPlainTextEdit *details = nullptr;
    MeshBindingWidget *binding = nullptr;
    QJsonObject generationPreview, acceptancePreview;
    QString selectedAttempt() const;
    void clearApprovals();
    void showResponse(const QJsonObject &response);
};

#endif
