#ifndef MESH_SENSITIVITY_DIALOG_H
#define MESH_SENSITIVITY_DIALOG_H

#include <functional>

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#include <rml_model.h>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

class MeshSensitivityDialog : public QDialog
{
    Q_OBJECT

public:
    static constexpr uint maximumElements = 200000;
    static constexpr int maximumAttempts = 3;

    MeshSensitivityDialog(const RModel &source, QString solver, QString artifactRoot,
                          QString documentId, quint64 revision, std::function<bool()> sourceCurrent,
                          bool agentAvailable, QWidget *parent = nullptr, bool baselineVerified = true);
    ~MeshSensitivityDialog() override;

    static bool hasNativeCompletion(const QString &path, const QString &hash);
    static bool matchesSolvedSnapshot(const RModel &source, const QString &path, const QString &hash);
    bool belongsTo(const QPair<QString,quint64> &stamp) const;
    bool isRunning() const { return process.state() != QProcess::NotRunning; }

public slots:
    void refresh();

signals:
    void reviewRequested(const QString &question);
    void openResultRequested(const QString &path);

private:
    void refineOnce();
    void finishRefinement(int exitCode, QProcess::ExitStatus exitStatus);
    void stopRefinement(const QString &reason);
    void recordAcceptance();
    void requestReview();
    void ensureStudyDirectory();
    void saveStudy(const QString &state = {});
    QJsonObject evidence(const QString &state = {}) const;
    QString statusCode() const;
    double maximumChange() const;
    void appendLevel(const QJsonArray &values, const QString &fileName);

    RModel current;
    std::vector<RR3Vector> positions;
    QJsonArray levels;
    QJsonArray attempts;
    QJsonObject decision;
    const QString solverPath;
    const QString rootPath;
    const QString documentId;
    const quint64 revision;
    const std::function<bool()> sourceCurrent;
    const bool agentAvailable;
    QString invalidReason;
    QString lastMessage;
    QString studyDirectory;
    QString levelDirectory;
    QString stopReason;
    QProcess process;
    QTimer deadline;
    bool pending = false;
    bool baselineVerified;
    QString baselineProof;

    QDoubleSpinBox *tolerance;
    QComboBox *refinementScope;
    QLabel *radiusLabel;
    QDoubleSpinBox *radius;
    QLabel *status;
    QTableWidget *table;
    QPushButton *refineButton;
    QPushButton *reviewButton;
    QPushButton *acceptButton;
    QPushButton *openButton;
    QPushButton *cancelButton;
};

#endif
