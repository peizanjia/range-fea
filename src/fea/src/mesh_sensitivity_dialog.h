#ifndef MESH_SENSITIVITY_DIALOG_H
#define MESH_SENSITIVITY_DIALOG_H

#include <functional>

#include <QDialog>
#include <QElapsedTimer>
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
    bool isRunning() const { return adaptiveActive || pending || process.state() != QProcess::NotRunning; }

    //! Starts one bounded estimate–mark–solve loop; never accepts for the user.
    bool startAdaptive(double tolerancePercent);

public slots:
    void refresh();

signals:
    void reviewRequested(const QString &question);
    void openResultRequested(const QString &path);

private:
    void refineOnce();
    void advanceAdaptive();
    void finishAdaptive(const QString &outcome, const QString &message = {});
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
    QPair<QString,QString> openableArtifact() const;

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
    QElapsedTimer adaptiveClock;
    bool adaptiveActive = false;
    double adaptiveTolerancePercent = 5.0;
    double adaptiveMinimumEdgeM = 0.0;
    QString adaptiveStatus;
    QJsonArray adaptiveEstimates;
    QJsonArray adaptiveHistory;
    std::vector<uint> adaptiveMarks;
    QLabel *adaptiveSummary;
    QWidget *manualControls;
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
