#ifndef GEOMETRY_COMMAND_WIDGET_H
#define GEOMETRY_COMMAND_WIDGET_H

#include <QWidget>
#include <QPointer>
#include <QJsonObject>
#include <array>

class Session;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTableWidget;

//! Explicit WF14 geometry entry: draft, host preview, then trusted user approval.
class GeometryCommandWidget : public QWidget
{
    Q_OBJECT
public:
    explicit GeometryCommandWidget(QWidget *parent = nullptr);
    void setSession(Session *session);

public slots:
    void preview();
    void approveAndApply();
    void refreshContext();

private:
    QPointer<Session> session;
    QComboBox *command = nullptr;
    QLineEdit *nodeId = nullptr;
    std::array<QLineEdit *, 3> coordinates{}, center{}, axis{}, translation{};
    QLineEdit *angle = nullptr;
    QLabel *source = nullptr;
    QLabel *status = nullptr;
    QLabel *receipt = nullptr;
    QPushButton *previewButton = nullptr;
    QPushButton *applyButton = nullptr;
    QTableWidget *changes = nullptr;
    QJsonObject approvedPreview;
    QJsonObject previewDocument;
    QString previewWorkspace;
    bool contextRefreshPending = false;
    quint64 contextRefreshGeneration = 0;
    void queueContextRefresh();
    QJsonObject currentDocument() const;
    QJsonObject arguments(QString *error) const;
    bool previewIsCurrent() const;
    void invalidatePreview(const QString &reason);
};

#endif
