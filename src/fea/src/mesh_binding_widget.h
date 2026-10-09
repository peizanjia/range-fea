#ifndef MESH_BINDING_WIDGET_H
#define MESH_BINDING_WIDGET_H
#include <QWidget>
#include <QPointer>
#include <QJsonObject>
class Session;
class QLineEdit;
class QTableWidget;
class QLabel;
class QPushButton;
class QPlainTextEdit;

class MeshBindingWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MeshBindingWidget(QWidget *parent = nullptr);
    void setSession(Session *value);
private:
    QPointer<Session> session;
    QLineEdit *young, *poisson, *density;
    QTableWidget *supports, *forces, *nodes;
    QLabel *status;
    QPushButton *previewButton, *applyButton;
    QPlainTextEdit *details;
    QJsonObject previewDocument;
    QString previewId, previewWorkspace;
    QJsonObject displayedDocument;
    QJsonObject document() const;
    void invalidate();
    void preview();
    void apply();
    void refreshNodes();
};
#endif
