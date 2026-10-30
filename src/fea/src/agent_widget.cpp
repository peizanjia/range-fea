#include "agent_widget.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QSaveFile>
#include <QNetworkReply>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QThread>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextList>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QUrl>
#include <QSharedPointer>
#include <QNetworkRequest>
#include <QTextBlockFormat>
#include <QTextCharFormat>

#include <cmath>

#include "application.h"
#include "application_settings_dialog.h"
#include "application_settings.h"
#include "main_window.h"
#include "model_command_host.h"
#include "session.h"
#include <ragent_conversation_store.h>
#include <rai_agent.h>
#include <rai_tool_stream.h>

namespace
{
// Model-authored Markdown must never cause local-file or network resource reads.
class ConversationDocument final : public QTextDocument
{
public:
    using QTextDocument::QTextDocument;
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

const QString providerTypeKey = QStringLiteral("aiAgent/providerType");
const QString endpointKey = QStringLiteral("aiAgent/endpoint");
const QString modelKey = QStringLiteral("aiAgent/model");
const QString effortKey = QStringLiteral("aiAgent/effort");
const QString automaticChangesKey = QStringLiteral("aiAgent/automaticChanges");
const QString streamingResponsesKey = QStringLiteral("aiAgent/streamingResponses");
constexpr qsizetype MaxProviderResponseBytes = 1024 * 1024;
constexpr int MaxActivityCards = 64;
constexpr qsizetype MaxTrustedExperimentReviewBytes = 48 * 1024;
constexpr qsizetype MaxProviderTelemetryEventBytes = 4096;

bool boundedTelemetryValue(const QString &value, qsizetype maxBytes)
{
    if (value.isEmpty() || value.toUtf8().size() > maxBytes) return false;
    for (const QChar character : value)
    {
        const ushort codePoint = character.unicode();
        if (codePoint < 0x20 || codePoint == 0x7f) return false;
    }
    return true;
}

QString sha256Hex(const QString &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool requestContainsExactUserPrompt(const QJsonArray &messages, const QString &prompt)
{
    int exactMatches = 0;
    for (const QJsonValue &value : messages)
    {
        if (!value.isObject()) continue;
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("user")
            && message.value(QStringLiteral("content")).isString()
            && message.value(QStringLiteral("content")).toString() == prompt)
            ++exactMatches;
    }
    return exactMatches == 1;
}

QString boundedProviderIdentifier(const QJsonValue &value, qsizetype maxBytes)
{
    if (!value.isString()) return {};
    const QString text = value.toString();
    if (text.isEmpty() || text.toUtf8().size() > maxBytes) return {};
    for (const QChar character : text)
    {
        const ushort codePoint = character.unicode();
        const bool asciiAlphaNumeric = (codePoint >= 'a' && codePoint <= 'z')
            || (codePoint >= 'A' && codePoint <= 'Z')
            || (codePoint >= '0' && codePoint <= '9');
        if (!asciiAlphaNumeric && codePoint != '.' && codePoint != '_' && codePoint != ':'
            && codePoint != '/' && codePoint != '+' && codePoint != '-')
            return {};
    }
    return text;
}

QJsonObject boundedProviderUsage(const QJsonValue &value)
{
    QJsonObject usage;
    if (!value.isObject()) return usage;

    constexpr double MaxExactJsonInteger = 9007199254740991.0;
    const QJsonObject raw = value.toObject();
    const QStringList allowedFields{QStringLiteral("prompt_tokens"),
        QStringLiteral("completion_tokens"), QStringLiteral("total_tokens"),
        QStringLiteral("prompt_cache_hit_tokens"), QStringLiteral("prompt_cache_miss_tokens")};
    for (const QString &field : allowedFields)
    {
        const QJsonValue metric = raw.value(field);
        if (!metric.isDouble()) continue;
        const double count = metric.toDouble();
        if (!std::isfinite(count) || count < 0.0 || count > MaxExactJsonInteger
            || std::floor(count) != count)
            continue;
        usage.insert(field, count);
    }
    return usage;
}

QString trustedExperimentReviewText(const QJsonObject &preview, const QString &taskId, bool *complete)
{
    if (complete) *complete = false;
    if (preview.value(QStringLiteral("command")) != QJsonValue(QStringLiteral("experiment.run")))
        return {};

    const QJsonObject arguments = preview.value(QStringLiteral("arguments")).toObject();
    const QJsonObject problem = arguments.value(QStringLiteral("problemSpec")).toObject();
    const QString previewId = preview.value(QStringLiteral("previewId")).toString();
    const QString experimentId = preview.value(QStringLiteral("experimentId")).toString();
    const QString authorizationDigest = preview.value(QStringLiteral("authorizationDigest")).toString();
    const QJsonObject sourceBinding = preview.value(QStringLiteral("sourceBinding")).toObject();
    const QJsonObject target = preview.value(QStringLiteral("target")).toObject();
    const QString sourceDocumentId = target.value(QStringLiteral("documentId")).toString();
    const QString sourceRevision = preview.value(QStringLiteral("expectedDocumentRevision")).toVariant().toString();
    const QString bindingDocumentId = sourceBinding.value(QStringLiteral("documentId")).toString();
    const QString bindingRevision = sourceBinding.value(QStringLiteral("documentRevision")).toString();
    const QString sourceSessionId = sourceBinding.value(QStringLiteral("sessionId")).toString();
    const QString sourceSessionEpoch = sourceBinding.value(QStringLiteral("sessionEpoch")).toString();
    const QString sourceSnapshotSha256 = sourceBinding.value(QStringLiteral("snapshotSha256")).toString();
    static const QRegularExpression sha256Grammar(QStringLiteral("^[0-9a-f]{64}$"));
    const QString normalizedTaskId = QUuid(taskId).toString(QUuid::WithoutBraces);
    if (!preview.value(QStringLiteral("requiresHostApproval")).toBool() || problem.isEmpty()
        || previewId.isEmpty() || experimentId.isEmpty() || authorizationDigest.isEmpty()
        || sourceBinding.isEmpty() || sourceDocumentId.isEmpty() || sourceRevision.isEmpty()
        || bindingDocumentId != sourceDocumentId || bindingRevision != sourceRevision
        || normalizedTaskId.compare(taskId, Qt::CaseInsensitive) != 0
        || sourceSessionId.isEmpty() || sourceSessionEpoch.isEmpty() || sourceSessionId == sourceSessionEpoch
        || !sha256Grammar.match(sourceSnapshotSha256).hasMatch())
        return QObject::tr("Trusted experiment preview is incomplete. Host approval details are unavailable; Apply is disabled.");

    QStringList sections;
    sections << QObject::tr("Trusted mechanics experiment — Host preview")
             << QObject::tr("Agent task ID: %1").arg(taskId)
             << QObject::tr("Preview ID: %1 · proposed experiment ID: %2")
                    .arg(previewId, experimentId)
             << QObject::tr("Source document: %1 · revision %2 · source remains unchanged")
                    .arg(sourceDocumentId, sourceRevision)
             << QObject::tr("Source session: %1 · epoch %2").arg(sourceSessionId, sourceSessionEpoch)
             << QObject::tr("Source snapshot SHA-256: %1").arg(sourceSnapshotSha256)
             << QObject::tr("ProblemSpec (normalized SI):\n%1")
                    .arg(QString::fromUtf8(QJsonDocument(problem).toJson(QJsonDocument::Indented)));

    for (const auto &section : {
             qMakePair(QStringLiteral("allowedNumericalRanges"), QObject::tr("Authorized numerical ranges")),
             qMakePair(QStringLiteral("budgets"), QObject::tr("Solve / repair / model-turn budgets")),
             qMakePair(QStringLiteral("stopConditions"), QObject::tr("Stop conditions")),
             qMakePair(QStringLiteral("convergenceStudy"), QObject::tr("Convergence study"))})
    {
        const QJsonValue value = arguments.value(section.first);
        if (value.isUndefined() || value.isNull()) continue;
        const QJsonDocument document = value.isObject() ? QJsonDocument(value.toObject())
            : value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument();
        sections << section.second + QStringLiteral(":\n")
            + (document.isNull() ? value.toVariant().toString()
                                 : QString::fromUtf8(document.toJson(QJsonDocument::Indented)));
    }

    const QJsonObject diagnostics = preview.value(QStringLiteral("diagnostics")).toObject();
    if (!diagnostics.isEmpty())
        sections << QObject::tr("Host diagnostics:\n%1")
            .arg(QString::fromUtf8(QJsonDocument(diagnostics).toJson(QJsonDocument::Indented)));
    sections << QObject::tr("Authorization digest: %1").arg(authorizationDigest);

    const QString text = sections.join(QStringLiteral("\n\n"));
    if (text.toUtf8().size() > MaxTrustedExperimentReviewBytes)
        return QObject::tr("Trusted experiment preview exceeds the review display limit. Apply is disabled; reduce the ProblemSpec or requested evidence and preview again.");
    if (complete) *complete = true;
    return text;
}

QString trustedExperimentEvidenceText(const QString &experimentId, const QJsonObject &summary)
{
    QStringList lines{QObject::tr("Trusted Host evidence · experiment %1")
                          .arg(experimentId),
                      QObject::tr("Host state: %1")
                          .arg(summary.value(QStringLiteral("state")).toString(
                              QObject::tr("not reported")))};
    const QJsonObject verification = summary.value(QStringLiteral("verification")).toObject();
    if (verification.isEmpty())
    {
        const QJsonValue passed = summary.value(QStringLiteral("verificationPassed"));
        lines << QObject::tr("Host verificationPassed: %1 · metric report unavailable; individual metrics cannot be claimed.")
            .arg(passed.isBool() ? (passed.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                 : QObject::tr("not reported"));
    }
    else
    {
        lines << QObject::tr("Verification: %1 · passed=%2 · schema=%3")
            .arg(verification.value(QStringLiteral("status")).toString(QObject::tr("not reported")),
                 verification.value(QStringLiteral("passed")).isBool()
                     ? (verification.value(QStringLiteral("passed")).toBool() ? QStringLiteral("true")
                                                                              : QStringLiteral("false"))
                     : QObject::tr("not reported"),
                 verification.value(QStringLiteral("schema")).toString(QObject::tr("not reported")));
        const QJsonObject identity = verification.value(QStringLiteral("identity")).toObject();
        for (const QString &field : {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                                     QStringLiteral("inputModelSha256"), QStringLiteral("inputSnapshotSha256"),
                                     QStringLiteral("inputPackageSha256"),
                                     QStringLiteral("manifestSha256"), QStringLiteral("physicalSummarySha256"),
                                     QStringLiteral("normalizedProblemSpecSha256")})
        {
            const QString value = identity.value(field).toString();
            if (!value.isEmpty() && value.size() <= 128)
                lines << QObject::tr("%1: %2").arg(field, value);
        }
        const QJsonObject metrics = verification.value(QStringLiteral("metrics")).toObject();
        QStringList names = metrics.keys();
        names.sort();
        for (const QString &name : names.mid(0, 24))
        {
            const QJsonObject metric = metrics.value(name).toObject();
            QString line = QObject::tr("Metric %1: %2 · required=%3")
                .arg(name, metric.value(QStringLiteral("status")).toString(QObject::tr("not reported")),
                     metric.value(QStringLiteral("required")).isBool()
                         ? (metric.value(QStringLiteral("required")).toBool() ? QStringLiteral("true")
                                                                              : QStringLiteral("false"))
                         : QObject::tr("not reported"));
            for (const QString &field : {QStringLiteral("value"), QStringLiteral("limit"),
                                         QStringLiteral("tolerance"), QStringLiteral("referenceError")})
                if (metric.value(field).isDouble() && std::isfinite(metric.value(field).toDouble()))
                    line += QObject::tr(" · %1=%2").arg(field, metric.value(field).toVariant().toString());
            const QJsonObject evidence = metric.value(QStringLiteral("evidence")).toObject();
            for (const QString &field : {QStringLiteral("relativeError"), QStringLiteral("maximumRelativeError"),
                                         QStringLiteral("relativeDifference"), QStringLiteral("maximumAbsoluteErrorM"),
                                         QStringLiteral("fixedAbsoluteLimitM"), QStringLiteral("fixedLimit"),
                                         QStringLiteral("scaledMaximumAbsoluteError"), QStringLiteral("normalizedResidual"),
                                         QStringLiteral("relativeBalanceError"), QStringLiteral("relativeEnergyError"),
                                         QStringLiteral("actualHz"), QStringLiteral("referenceHz"),
                                         QStringLiteral("strainEnergyJ"), QStringLiteral("externalWorkJ")})
                if (evidence.value(field).isDouble() && std::isfinite(evidence.value(field).toDouble()))
                    line += QObject::tr(" · %1=%2").arg(field, evidence.value(field).toVariant().toString());
            lines << line.left(1000);
        }
        const QJsonObject thresholds = verification.value(QStringLiteral("thresholds")).toObject();
        if (!thresholds.isEmpty())
            lines << QObject::tr("Verifier thresholds: %1")
                .arg(QString::fromUtf8(QJsonDocument(thresholds).toJson(QJsonDocument::Compact)).left(1000));
    }

    const QJsonObject convergence = summary.value(QStringLiteral("convergence")).toObject();
    if (!convergence.isEmpty())
    {
        const auto number = [](const QJsonValue &value) {
            return value.isDouble() && std::isfinite(value.toDouble())
                ? QString::number(value.toDouble(), 'g', 15) : QObject::tr("not reported");
        };
        const auto text = [](const QJsonValue &value) {
            return value.isString() && !value.toString().isEmpty()
                ? value.toString().left(256) : QObject::tr("not reported");
        };
        const auto fields = [&number, &text](const QJsonObject &object, const QStringList &names) {
            QStringList values;
            for (const QString &name : names)
            {
                const QJsonValue value = object.value(name);
                const bool textField = name == QLatin1String("status") || name == QLatin1String("key")
                    || name == QLatin1String("quantity") || name == QLatin1String("component")
                    || name == QLatin1String("code") || name == QLatin1String("message");
                const QString display = name == QLatin1String("belowTolerance")
                    ? (value.isBool() ? (value.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                      : QObject::tr("not reported"))
                    : textField ? text(value) : number(value);
                values << name + QLatin1Char('=') + display;
            }
            return values.join(QStringLiteral(" · "));
        };
        lines << QObject::tr("Convergence: %1 · stop reason: %2 · levels: %3")
            .arg(convergence.value(QStringLiteral("status")).toString(QObject::tr("not reported")),
                 convergence.value(QStringLiteral("stopReason")).toString(QObject::tr("not reported")),
                 number(convergence.value(QStringLiteral("levelsCompleted"))));
        const QJsonArray levels = convergence.value(QStringLiteral("levels")).toArray();
        for (qsizetype index = 0; index < qMin(qsizetype(4), levels.size()); ++index)
        {
            const QJsonObject level = levels.at(index).toObject();
            const QJsonValue verificationPassed = level.value(QStringLiteral("verificationPassed"));
            const QString verificationState = verificationPassed.isBool()
                ? (verificationPassed.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                : QObject::tr("not reported");
            lines << QObject::tr("Level %1 · verified=%2 · cost=%3")
                .arg(number(level.value(QStringLiteral("level"))),
                     verificationState,
                     QString::fromUtf8(QJsonDocument(level.value(QStringLiteral("cost")).toObject())
                                           .toJson(QJsonDocument::Compact)).left(256));
            const QJsonObject change = level.value(QStringLiteral("changeFromPrevious")).toObject();
            lines << QObject::tr("Change from previous: %1").arg(fields(change,
                {QStringLiteral("status"), QStringLiteral("aggregateRelativeChange"),
                 QStringLiteral("fixedToleranceRelative"), QStringLiteral("belowTolerance")}));
            const QJsonArray changes = change.value(QStringLiteral("qoiChanges")).toArray();
            for (qsizetype i = 0; i < qMin(qsizetype(8), changes.size()); ++i)
                lines << QObject::tr("QOI change: %1").arg(fields(changes.at(i).toObject(),
                    {QStringLiteral("key"), QStringLiteral("quantity"), QStringLiteral("status"),
                     QStringLiteral("relativeChange"), QStringLiteral("absoluteChange"),
                     QStringLiteral("fineScale"), QStringLiteral("belowTolerance")}));
            lines << QObject::tr("Comparison error: %1").arg(fields(
                level.value(QStringLiteral("comparisonError")).toObject(),
                {QStringLiteral("code"), QStringLiteral("message")}));
            lines << QObject::tr("Level measurements: %1").arg(fields(level,
                {QStringLiteral("frequencyHz"), QStringLiteral("mode"), QStringLiteral("strainEnergyJ"),
                 QStringLiteral("timeStepS"), QStringLiteral("totalDurationS")}));
            const QJsonArray quantities = level.value(QStringLiteral("quantities")).toArray();
            if (quantities.isEmpty()) lines << QObject::tr("QOI: not reported");
            for (qsizetype i = 0; i < qMin(qsizetype(8), quantities.size()); ++i)
            {
                const QJsonObject qoi = quantities.at(i).toObject();
                const QJsonObject selector = qoi.value(QStringLiteral("selector")).toObject();
                QStringList coordinate;
                const QJsonArray components = qoi.value(QStringLiteral("coordinateM")).toArray();
                for (qsizetype axis = 0; axis < qMin(qsizetype(3), components.size()); ++axis)
                    coordinate << number(components.at(axis));
                lines << QObject::tr("QOI: selector=%1 · coordinateM=%2 · %3")
                    .arg(selector.isEmpty() ? QObject::tr("not reported") : QString::fromUtf8(
                             QJsonDocument(selector).toJson(QJsonDocument::Compact)).left(256),
                         coordinate.isEmpty() ? QObject::tr("not reported") : coordinate.join(QLatin1Char(',')),
                         fields(qoi, {QStringLiteral("quantity"), QStringLiteral("component"),
                             QStringLiteral("sampleCount"), QStringLiteral("peakAbsoluteM"), QStringLiteral("rmsM"),
                             QStringLiteral("firstValueM"), QStringLiteral("lastValueM"),
                             QStringLiteral("firstTimeS"), QStringLiteral("lastTimeS")}));
            }
            const QJsonObject errors = level.value(QStringLiteral("referenceErrors")).toObject();
            if (errors.isEmpty()) lines << QObject::tr("Reference errors: not reported");
            const QStringList metricNames = errors.keys();
            for (const QString &name : metricNames.mid(0, 8))
            {
                const QJsonObject metric = errors.value(name).toObject();
                lines << QObject::tr("Reference error %1: %2 · %3")
                    .arg(name.left(256), text(metric.value(QStringLiteral("status"))),
                         fields(metric.value(QStringLiteral("evidence")).toObject(),
                             {QStringLiteral("relativeError"), QStringLiteral("maximumRelativeError"),
                              QStringLiteral("relativeDifference"), QStringLiteral("maximumAbsoluteErrorM"),
                              QStringLiteral("scaledMaximumAbsoluteError")}));
            }
        }
    }
    const QJsonArray repairs = summary.value(QStringLiteral("repairs")).toArray();
    for (qsizetype index = 0; index < qMin(qsizetype(6), repairs.size()); ++index)
    {
        const QJsonObject repair = repairs.at(index).toObject();
        lines << QObject::tr("Repair event: %1 · repair=%2 · fields=%3")
            .arg(repair.value(QStringLiteral("event")).toString(),
                 repair.value(QStringLiteral("repair")).toVariant().toString(),
                 QString::fromUtf8(QJsonDocument(repair.value(QStringLiteral("changedProblemFields")).toArray())
                                       .toJson(QJsonDocument::Compact)).left(256));
    }
    const QString terminalCode = summary.value(QStringLiteral("terminalCode")).toString();
    if (!terminalCode.isEmpty()) lines << QObject::tr("Host stop code: %1").arg(terminalCode.left(128));

    constexpr qsizetype maxEvidenceBytes = 24 * 1024;
    const QString capMarker = QObject::tr("[Host evidence display capped at 24 KiB; some bounded fields are omitted from this view.]");
    const qsizetype contentLimit = maxEvidenceBytes - capMarker.toUtf8().size() - 1;
    QStringList boundedLines;
    qsizetype usedBytes = 0;
    bool truncated = false;
    for (const QString &line : lines)
    {
        const qsizetype lineBytes = line.toUtf8().size() + 1;
        if (usedBytes + lineBytes > contentLimit) { truncated = true; break; }
        boundedLines.append(line);
        usedBytes += lineBytes;
    }
    if (truncated)
        boundedLines.append(capMarker);
    return boundedLines.join(QLatin1Char('\n'));
}

AgentWidget *activeAgentWidget()
{
    Application *application = Application::instance();
    MainWindow *window = application ? application->getMainWindow() : nullptr;
    return window ? window->findChild<AgentWidget *>(QStringLiteral("agentTaskWidget")) : nullptr;
}

QSettings *agentPreferences()
{
    Application *application = Application::instance();
    return application ? application->getApplicationSettings() : nullptr;
}

bool isValidAgentEndpoint(const QString &endpoint)
{
    const QUrl url(endpoint.trimmed(), QUrl::StrictMode);
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && !url.host().isEmpty() && !url.authority().contains(QLatin1Char('@'))
        && !url.hasQuery() && !url.hasFragment();
}

bool providerSupportsStreaming(RAgentSettings::Type type)
{
    return type == RAgentSettings::Type::DeepSeek || type == RAgentSettings::Type::OpenAILegacy;
}

QString invalidEndpointMessage()
{
    return QObject::tr("Use an HTTPS endpoint without credentials, a query, or a fragment.");
}

QString optionLabel(const QJsonValue &option)
{
    if (option.isString()) return option.toString();
    const QJsonObject object = option.toObject();
    for (const QString &key : {QStringLiteral("label"), QStringLiteral("title"), QStringLiteral("text"), QStringLiteral("value")})
    {
        const QString value = object.value(key).toVariant().toString();
        if (!value.isEmpty()) return value;
    }
    return QString();
}

QString optionValue(const QJsonValue &option)
{
    if (option.isString()) return option.toString();
    const QJsonObject object = option.toObject();
    for (const QString &key : {QStringLiteral("value"), QStringLiteral("id"), QStringLiteral("label"), QStringLiteral("text")})
    {
        const QString value = object.value(key).toVariant().toString();
        if (!value.isEmpty()) return value;
    }
    return optionLabel(option);
}

QJsonObject executionReferenceFacts(const QJsonObject &reference)
{
    static const QStringList scalarFields{
        QStringLiteral("command"), QStringLiteral("operationId"), QStringLiteral("jobId"),
        QStringLiteral("sourceDocumentId"), QStringLiteral("sourceRevision"),
        QStringLiteral("acceptedModelSha256"), QStringLiteral("idempotencyKey"),
        QStringLiteral("requestSha256"), QStringLiteral("workflowProjectId"),
        QStringLiteral("experimentId"), QStringLiteral("previewId"),
        QStringLiteral("sourceSessionEpoch"), QStringLiteral("authorizationDigest"), QStringLiteral("authorizationMode"),
        QStringLiteral("trustedAuthorizationPreviewId"), QStringLiteral("trustedAuthorizationExperimentId"),
        QStringLiteral("trustedAuthorizationDigest")};
    QJsonObject facts;
    for (const QString &field : scalarFields)
        if (reference.value(field).isString()) facts.insert(field, reference.value(field));

    const QJsonObject source = reference.value(QStringLiteral("sourceBinding")).toObject();
    if (source.value(QStringLiteral("documentId")).isString())
        facts.insert(QStringLiteral("sourceDocumentId"), source.value(QStringLiteral("documentId")));
    if (source.value(QStringLiteral("sessionEpoch")).isString())
        facts.insert(QStringLiteral("sourceSessionEpoch"), source.value(QStringLiteral("sessionEpoch")));
    if (source.value(QStringLiteral("authorizationDigest")).isString())
        facts.insert(QStringLiteral("authorizationDigest"), source.value(QStringLiteral("authorizationDigest")));
    if (source.value(QStringLiteral("previewId")).isString())
        facts.insert(QStringLiteral("previewId"), source.value(QStringLiteral("previewId")));
    const QJsonValue revision = source.value(QStringLiteral("documentRevision"));
    if (revision.isString())
        facts.insert(QStringLiteral("sourceRevision"), revision);
    else if (revision.isDouble() && std::isfinite(revision.toDouble())
             && revision.toDouble() >= 0.0 && std::floor(revision.toDouble()) == revision.toDouble()
             && revision.toDouble() <= 9007199254740991.0)
        facts.insert(QStringLiteral("sourceRevision"), QString::number(quint64(revision.toDouble())));
    return facts;
}
}

QJsonObject AgentWidget::trustedExperimentAuthorizationReview(const QJsonObject &candidate,
                                                               bool *warningAcknowledgementRequired,
                                                               QString *error)
{
    if (warningAcknowledgementRequired) *warningAcknowledgementRequired = false;
    if (error) error->clear();
    const QJsonObject diagnostics = candidate.value(QStringLiteral("diagnostics")).toObject();
    const QString diagnosticStatus = diagnostics.value(QStringLiteral("status")).toString();
    const QJsonObject target = candidate.value(QStringLiteral("target")).toObject();
    const QJsonObject sourceBinding = candidate.value(QStringLiteral("sourceBinding")).toObject();
    const QString targetDocumentId = target.value(QStringLiteral("documentId")).toString();
    const QString sourceDocumentId = sourceBinding.value(QStringLiteral("documentId")).toString();
    const QString expectedRevision = candidate.value(QStringLiteral("expectedDocumentRevision")).toVariant().toString();
    const QString sourceRevision = sourceBinding.value(QStringLiteral("documentRevision")).toString();
    const QString diagnosticSourceId = diagnostics.value(QStringLiteral("sourceId")).toString();
    const QString diagnosticRevision = diagnostics.value(QStringLiteral("modelRevision")).toString();
    const auto boundedDiagnosticText = [](const QJsonValue &value, qsizetype maxBytes) {
        if (!value.isString()) return false;
        const QString text = value.toString();
        return !text.trimmed().isEmpty() && boundedTelemetryValue(text, maxBytes);
    };
    if (diagnostics.isEmpty()
        || diagnostics.value(QStringLiteral("operation")) != QJsonValue(QStringLiteral("analysis.diagnose"))
        || !diagnostics.value(QStringLiteral("schemaVersion")).isDouble()
        || diagnostics.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
        || !diagnostics.value(QStringLiteral("issues")).isArray()
        || (diagnosticStatus != QStringLiteral("ready")
            && diagnosticStatus != QStringLiteral("warning")
            && diagnosticStatus != QStringLiteral("error")))
    {
        if (error) *error = QObject::tr("Host diagnostics are missing or malformed.");
        return {};
    }
    if (!boundedDiagnosticText(target.value(QStringLiteral("documentId")), 256)
        || !boundedDiagnosticText(sourceBinding.value(QStringLiteral("documentId")), 256)
        || !boundedDiagnosticText(sourceBinding.value(QStringLiteral("documentRevision")), 64)
        || !boundedDiagnosticText(QJsonValue(expectedRevision), 64)
        || targetDocumentId != sourceDocumentId || expectedRevision != sourceRevision
        || !boundedDiagnosticText(diagnostics.value(QStringLiteral("sourceId")), 256)
        || !boundedDiagnosticText(diagnostics.value(QStringLiteral("modelRevision")), 64)
        || diagnosticSourceId != sourceDocumentId || diagnosticRevision != sourceRevision)
    {
        if (error) *error = QObject::tr("Host diagnostics do not match the reviewed source document and revision.");
        return {};
    }
    if (diagnostics.value(QStringLiteral("issues")).toArray().size() > 128)
    {
        if (error) *error = QObject::tr("Host diagnostics contain too many issues to review safely.");
        return {};
    }
    bool hasWarning = false;
    bool hasErrorIssue = false;
    QStringList blockingIssues;
    for (const QJsonValue &issueValue : diagnostics.value(QStringLiteral("issues")).toArray())
    {
        if (!issueValue.isObject())
        {
            if (error) *error = QObject::tr("Host diagnostics contain a malformed issue.");
            return {};
        }
        const QJsonObject issue = issueValue.toObject();
        const QString severity = issue.value(QStringLiteral("severity")).toString();
        if (!boundedDiagnosticText(issue.value(QStringLiteral("code")), 128)
            || !boundedDiagnosticText(issue.value(QStringLiteral("message")), 4096)
            || !issue.value(QStringLiteral("evidence")).isObject())
        {
            if (error) *error = QObject::tr("Host diagnostics contain an issue without a code, explanation, or evidence object.");
            return {};
        }
        if (severity == QStringLiteral("error"))
        {
            hasErrorIssue = true;
            blockingIssues.append(issue.value(QStringLiteral("code")).toString()
                + QStringLiteral(": ") + issue.value(QStringLiteral("message")).toString());
        }
        else if (severity == QStringLiteral("warning"))
            hasWarning = true;
        else
        {
            if (error) *error = QObject::tr("Host diagnostics contain an unknown issue severity.");
            return {};
        }
    }
    if (diagnosticStatus == QStringLiteral("ready") && (hasWarning || hasErrorIssue))
    {
        if (error) *error = QObject::tr("Host diagnostic status and issue severities are inconsistent.");
        return {};
    }
    if (diagnosticStatus == QStringLiteral("warning") && (!hasWarning || hasErrorIssue))
    {
        if (error) *error = QObject::tr("Host diagnostic status and issue severities are inconsistent.");
        return {};
    }
    if (diagnosticStatus == QStringLiteral("error") || hasErrorIssue)
    {
        if (error) *error = blockingIssues.isEmpty()
            ? QObject::tr("Host diagnostics report an error; this experiment cannot be authorized.")
            : QObject::tr("Host diagnostics block authorization: %1").arg(blockingIssues.join(QStringLiteral("; ")));
        return {};
    }
    if (hasWarning && warningAcknowledgementRequired)
        *warningAcknowledgementRequired = true;
    return QJsonObject{
        {QStringLiteral("previewId"), candidate.value(QStringLiteral("previewId"))},
        {QStringLiteral("experimentId"), candidate.value(QStringLiteral("experimentId"))},
        {QStringLiteral("authorizationDigest"), candidate.value(QStringLiteral("authorizationDigest"))},
        {QStringLiteral("target"), candidate.value(QStringLiteral("target"))},
        {QStringLiteral("expectedDocumentRevision"), candidate.value(QStringLiteral("expectedDocumentRevision"))},
        {QStringLiteral("sourceBinding"), candidate.value(QStringLiteral("sourceBinding"))},
        {QStringLiteral("arguments"), candidate.value(QStringLiteral("arguments"))},
        {QStringLiteral("diagnostics"), diagnostics}};
}

bool AgentWidget::confirmTrustedExperimentAuthorization(QWidget *parent, const QJsonObject &candidate,
                                                         QString *error)
{
    if (error) error->clear();
    bool warningAcknowledgementRequired = false;
    QString reviewError;
    const QJsonObject review = trustedExperimentAuthorizationReview(candidate,
        &warningAcknowledgementRequired, &reviewError);
    if (review.isEmpty())
    {
        if (error) *error = reviewError;
        return false;
    }
    const QByteArray reviewText = QJsonDocument(review).toJson(QJsonDocument::Indented);
    if (reviewText.size() > MaxTrustedExperimentReviewBytes)
    {
        if (error) *error = QObject::tr("Experiment authorization is too large to review.");
        return false;
    }

    QDialog reviewDialog(parent);
    reviewDialog.setObjectName(QStringLiteral("agentBoundedExperimentReview"));
    reviewDialog.setWindowTitle(QObject::tr("Authorize one bounded experiment"));
    auto *layout = new QVBoxLayout(&reviewDialog);
    layout->addWidget(new QLabel(QObject::tr("Review the exact Host-normalized problem and policy for the next task."), &reviewDialog));
    auto *details = new QPlainTextEdit(&reviewDialog);
    details->setObjectName(QStringLiteral("agentBoundedExperimentReviewDetails"));
    details->setReadOnly(true);
    details->setPlainText(QString::fromUtf8(reviewText));
    layout->addWidget(details);
    QCheckBox *warningAcknowledgement = nullptr;
    if (warningAcknowledgementRequired)
    {
        warningAcknowledgement = new QCheckBox(
            QObject::tr("I reviewed and accept the Host warning for this experiment."), &reviewDialog);
        warningAcknowledgement->setObjectName(QStringLiteral("agentBoundedExperimentWarningAcknowledgement"));
        layout->addWidget(warningAcknowledgement);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &reviewDialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &reviewDialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &reviewDialog, &QDialog::reject);
    if (warningAcknowledgement)
    {
        QPushButton *accept = buttons->button(QDialogButtonBox::Ok);
        accept->setEnabled(false);
        QObject::connect(warningAcknowledgement, &QCheckBox::toggled, accept, &QPushButton::setEnabled);
    }
    layout->addWidget(buttons);
    reviewDialog.resize(760, 560);
    if (reviewDialog.exec() != QDialog::Accepted)
    {
        if (error) *error = QObject::tr("Experiment authorization cancelled.");
        return false;
    }
    return true;
}

AgentWidget::AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent,
                         QNetworkAccessManager *networkAccessManager,
                         AgentTaskRunner::AccessPolicy policy, bool restoreLastConversation,
                         AgentTaskRunner::EvaluationArm evaluationArm,
                         AgentEvaluationSlotBinding evaluationSlotBinding)
    : QWidget(parent), accessPolicy(policy), evaluationArm(evaluationArm),
      evaluationSlotBinding(std::move(evaluationSlotBinding)),
      runner(host, workflow, this, policy, evaluationArm), defaultNetwork(this),
      network(networkAccessManager ? networkAccessManager : &defaultNetwork), commandHost(host),
      connectedWorkflow(workflow),
      providerSettings(RAgentSettings::Type::DeepSeek)
{
    setObjectName(policy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? QStringLiteral("agentReadOnlyTaskWidget") : QStringLiteral("agentTaskWidget"));
    if (QSettings *preferences = agentPreferences())
    {
        const auto requestedType = static_cast<RAgentSettings::Type>(preferences->value(providerTypeKey, int(RAgentSettings::Type::DeepSeek)).toInt());
        if (requestedType == RAgentSettings::Type::DeepSeek || requestedType == RAgentSettings::Type::OpenAILegacy)
            providerSettings.setType(requestedType);
        const RAgentSettings defaults(providerSettings.getType());
        const QString endpoint = preferences->value(endpointKey, defaults.getApiUrl()).toString();
        if (isValidAgentEndpoint(endpoint)) providerSettings.setApiUrl(endpoint.trimmed());
        else
        {
            providerSettings.setApiUrl(defaults.getApiUrl());
            preferences->setValue(endpointKey, defaults.getApiUrl());
        }
        providerSettings.setModel(preferences->value(modelKey, defaults.getModel()).toString());
        selectedEffort = preferences->value(effortKey, QStringLiteral("off")).toString();
    }
    providerSettings.setApiKey(QString());
    if (providerSettings.getType() != RAgentSettings::Type::DeepSeek)
        selectedEffort = QStringLiteral("default");

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);
    // Keep the transcript scrollable while approval and question controls stay
    // in the fixed outer layout. At high DPI, a long transcript can otherwise
    // cover the visible controls inside this scroll area's content widget.
    auto *historyScroll = new QScrollArea(this);
    historyScroll->setObjectName(QStringLiteral("agentHistoryScroll"));
    historyScroll->setWidgetResizable(true);
    historyScroll->setFrameShape(QFrame::NoFrame);
    auto *historyBody = new QWidget(historyScroll);
    historyBody->setObjectName(QStringLiteral("agentHistoryContent"));
    auto *layout = new QVBoxLayout(historyBody);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    historyScroll->setWidget(historyBody);
    rootLayout->addWidget(historyScroll, 1);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(policy == AgentTaskRunner::AccessPolicy::ReadOnly ? tr("Read-only question") : tr("Assistant"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAccessibleName(tr("Assistant conversation"));
    header->addWidget(title);
    header->addStretch(1);
    newChat = new QToolButton;
    newChat->setObjectName(QStringLiteral("agentNewChat"));
    newChat->setText(tr("New chat"));
    newChat->setToolButtonStyle(Qt::ToolButtonTextOnly);
    newChat->setAccessibleName(tr("Start a new conversation"));
    header->addWidget(newChat);
    settingsButton = new QToolButton;
    settingsButton->setObjectName(QStringLiteral("agentSettings"));
    settingsButton->setText(tr("Settings"));
    settingsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    settingsButton->setAccessibleName(tr("Open assistant settings"));
    header->addWidget(settingsButton);
    layout->addLayout(header);

    auto *historyRow = new QHBoxLayout;
    conversationChoice = new QComboBox(this);
    conversationChoice->setObjectName(QStringLiteral("agentConversationChoice"));
    conversationChoice->setAccessibleName(tr("Saved conversation"));
    conversationChoice->setMinimumWidth(0);
    historyRow->addWidget(conversationChoice, 1);
    branchConversation = new QToolButton(this);
    branchConversation->setObjectName(QStringLiteral("agentBranchConversation"));
    branchConversation->setText(tr("Branch"));
    branchConversation->setToolTip(tr("Create an independent conversation at the selected historical message. Past actions remain historical facts."));
    historyRow->addWidget(branchConversation);
    historyMenu = new QToolButton(this);
    historyMenu->setObjectName(QStringLiteral("agentConversationMenu"));
    historyMenu->setText(tr("More"));
    historyMenu->setPopupMode(QToolButton::InstantPopup);
    auto *conversationMenu = new QMenu(historyMenu);
    auto *exportAction = conversationMenu->addAction(tr("Export conversation…"));
    exportAction->setObjectName(QStringLiteral("agentExportConversation"));
    auto *deleteAction = conversationMenu->addAction(tr("Delete conversation"));
    deleteAction->setObjectName(QStringLiteral("agentDeleteConversation"));
    historyMenu->setMenu(conversationMenu);
    historyRow->addWidget(historyMenu);
    layout->addLayout(historyRow);
    auto *historyNavigation = new QHBoxLayout;
    branchMessageChoice = new QComboBox(this);
    branchMessageChoice->setObjectName(QStringLiteral("agentBranchMessage"));
    branchMessageChoice->setAccessibleName(tr("Historical message for branching"));
    branchMessageChoice->setMinimumWidth(0);
    historyNavigation->addWidget(branchMessageChoice, 1);
    historyPrevious = new QPushButton(tr("Earlier"), this);
    historyPrevious->setObjectName(QStringLiteral("agentHistoryPrevious"));
    historyNext = new QPushButton(tr("Later"), this);
    historyNext->setObjectName(QStringLiteral("agentHistoryNext"));
    historyNavigation->addWidget(historyPrevious);
    historyNavigation->addWidget(historyNext);
    layout->addLayout(historyNavigation);
    historyStatus = new QLabel(this);
    historyStatus->setObjectName(QStringLiteral("agentHistoryStatus"));
    historyStatus->setWordWrap(true);
    historyStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(historyStatus);
    connect(conversationChoice, &QComboBox::activated, this, [this](int index) {
        if (runner.isActive()) return;
        const QString id = conversationChoice->itemData(index).toString();
        if (id.isEmpty()) createConversation();
        else loadConversation(id);
    });
    connect(branchConversation, &QToolButton::clicked, this, &AgentWidget::branchFromMessage);
    connect(exportAction, &QAction::triggered, this, &AgentWidget::exportConversation);
    connect(deleteAction, &QAction::triggered, this, &AgentWidget::deleteConversation);
    connect(historyPrevious, &QPushButton::clicked, this, [this] { renderHistoryPage(qMax(0, historyOffset - 50)); });
    connect(historyNext, &QPushButton::clicked, this, [this] { renderHistoryPage(historyOffset + 50); });

    auto *hint = new QLabel(tr("Ask about the selected model or supported analysis tasks."));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    state = new QLabel(tr("Ready"));
    state->setObjectName(QStringLiteral("agentConversationStatus"));
    state->setWordWrap(false);
    state->setMinimumHeight(state->fontMetrics().height() + 4);
    state->setMinimumWidth(0);
    state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    state->setToolTip(tr("Conversation status"));
    layout->addWidget(state);

    activityScroll = new QScrollArea(this);
    activityScroll->setObjectName(QStringLiteral("agentActivityHistory"));
    activityScroll->setWidgetResizable(true);
    activityScroll->setFrameShape(QFrame::NoFrame);
    activityScroll->setMaximumHeight(150);
    activityScroll->setAccessibleName(tr("Trusted tool activity history"));
    activityCards = new QWidget(activityScroll);
    activityCards->setObjectName(QStringLiteral("agentActivityCards"));
    activityCardsLayout = new QVBoxLayout(activityCards);
    activityCardsLayout->setContentsMargins(0, 0, 0, 0);
    activityCardsLayout->setSpacing(4);
    activityCardsLayout->addStretch(1);
    activityScroll->setWidget(activityCards);
    activityScroll->hide();
    layout->addWidget(activityScroll);

    auto *eventToggle = new QToolButton;
    eventToggle->setObjectName(QStringLiteral("agentEventHistoryToggle"));
    eventToggle->setText(tr("Recent GUI document events"));
    eventToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    eventToggle->setArrowType(Qt::RightArrow);
    eventToggle->setCheckable(true);
    eventToggle->setChecked(false);
    layout->addWidget(eventToggle);
    auto *eventBody = new QWidget;
    eventBody->setObjectName(QStringLiteral("agentEventHistoryBody"));
    auto *eventLayout = new QVBoxLayout(eventBody);
    eventLayout->setContentsMargins(0, 0, 0, 0);
    auto *eventNotice = new QLabel(tr("In-memory session event history (up to 256 records); this is not a durable audit log."));
    eventNotice->setObjectName(QStringLiteral("agentEventHistoryNotice"));
    eventNotice->setWordWrap(true);
    eventLayout->addWidget(eventNotice);
    eventContextSummary = new QLabel(tr("Current document context has not been queried yet."));
    eventContextSummary->setObjectName(QStringLiteral("agentEventContextSummary"));
    eventContextSummary->setWordWrap(true);
    eventLayout->addWidget(eventContextSummary);
    eventStatus = new QLabel(tr("Waiting for host events."));
    eventStatus->setObjectName(QStringLiteral("agentEventStatus"));
    eventStatus->setWordWrap(true);
    eventLayout->addWidget(eventStatus);
    eventHistory = new QPlainTextEdit;
    eventHistory->setObjectName(QStringLiteral("agentEventHistory"));
    eventHistory->setReadOnly(true);
    eventHistory->setMaximumHeight(150);
    eventHistory->setMaximumBlockCount(256);
    eventLayout->addWidget(eventHistory);
    eventBody->hide();
    eventBody->setMaximumHeight(280);
    layout->addWidget(eventBody);
    connect(eventToggle, &QToolButton::toggled, this, [eventToggle, eventBody](bool expanded) {
        eventBody->setVisible(expanded);
        eventToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });

    transcript = new QTextBrowser;
    transcript->setDocument(new ConversationDocument(transcript));
    transcript->setObjectName(QStringLiteral("agentTranscript"));
    transcript->setReadOnly(true);
    transcript->setOpenExternalLinks(false);
    transcript->setOpenLinks(false);
    transcript->setAccessibleName(tr("Conversation transcript"));
    transcript->setMinimumWidth(0);
    layout->addWidget(transcript, 1);

    approvalCard = new QFrame;
    approvalCard->setObjectName(QStringLiteral("agentApprovalCard"));
    approvalCard->setFrameShape(QFrame::StyledPanel);
    auto *approvalLayout = new QVBoxLayout(approvalCard);
    approvalLayout->setContentsMargins(8, 6, 8, 6);
    auto *approvalTitle = new QLabel(tr("Review proposed change"));
    QFont cardFont = approvalTitle->font();
    cardFont.setBold(true);
    approvalTitle->setFont(cardFont);
    approvalLayout->addWidget(approvalTitle);
    preview = new QTextBrowser;
    preview->setObjectName(QStringLiteral("agentPreview"));
    preview->setReadOnly(true);
    preview->setMaximumHeight(110);
    approvalLayout->addWidget(preview);
    auto *approvalButtons = new QHBoxLayout;
    apply = new QPushButton(tr("Apply"));
    apply->setObjectName(QStringLiteral("agentApproveProposal"));
    apply->setAccessibleName(tr("Apply the displayed proposal"));
    decline = new QPushButton(tr("Reject"));
    decline->setObjectName(QStringLiteral("agentRejectProposal"));
    decline->setAccessibleName(tr("Reject the displayed proposal"));
    approvalButtons->addWidget(apply);
    approvalButtons->addWidget(decline);
    approvalLayout->addLayout(approvalButtons);
    approvalCard->hide();
    rootLayout->addWidget(approvalCard);

    questionCard = new QFrame;
    questionCard->setObjectName(QStringLiteral("agentQuestionCard"));
    questionCard->setFrameShape(QFrame::StyledPanel);
    auto *questionLayout = new QVBoxLayout(questionCard);
    questionLayout->setContentsMargins(8, 6, 8, 6);
    auto *questionTitle = new QLabel(tr("A question needs your answer"));
    questionTitle->setFont(cardFont);
    questionLayout->addWidget(questionTitle);
    questionText = new QLabel;
    questionText->setObjectName(QStringLiteral("agentQuestionText"));
    questionText->setWordWrap(true);
    questionText->setTextFormat(Qt::PlainText);
    questionScroll = new QScrollArea(questionCard);
    questionScroll->setWidgetResizable(true);
    questionScroll->setMaximumHeight(100);
    questionScroll->setFrameShape(QFrame::NoFrame);
    questionScroll->setWidget(questionText);
    questionLayout->addWidget(questionScroll);
    questionOptions = new QWidget(questionCard);
    questionOptionsLayout = new QGridLayout(questionOptions);
    questionOptionsLayout->setContentsMargins(0, 0, 0, 0);
    questionOptionsLayout->setHorizontalSpacing(6);
    questionOptionsLayout->setVerticalSpacing(4);
    questionLayout->addWidget(questionOptions);
    questionCard->hide();
    rootLayout->addWidget(questionCard);

    // A single toolbar adds every control's minimum width together, even when
    // this page is inactive in the dock stack. Keep the fields in a wrapping
    // form and the options on their own row so the dock can stay narrow.
    auto *modelForm = new QFormLayout;
    modelForm->setContentsMargins(0, 0, 0, 0);
    modelForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    modelForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *modelLabel = new QLabel(tr("Model"));
    model = new QComboBox;
    model->setObjectName(QStringLiteral("agentModel"));
    model->setEditable(true);
    model->setInsertPolicy(QComboBox::NoInsert);
    model->setAccessibleName(tr("Assistant model"));
    model->lineEdit()->setObjectName(QStringLiteral("agentModelInput"));
    model->lineEdit()->setClearButtonEnabled(false);
    model->setMinimumWidth(80);
    model->setMinimumContentsLength(12);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model->setEditText(providerSettings.getModel());
    model->setToolTip(model->currentText());
    connect(model, &QComboBox::currentTextChanged, model, &QWidget::setToolTip);
    modelLabel->setBuddy(model);
    modelForm->addRow(modelLabel, model);
    auto *effortLabel = new QLabel(tr("Thinking"));
    effort = new QComboBox;
    effort->setObjectName(QStringLiteral("agentThinkingEffort"));
    effort->setAccessibleName(tr("DeepSeek thinking effort"));
    updateEffortChoices();
    effortLabel->setBuddy(effort);
    modelForm->addRow(effortLabel, effort);
    auto *modelOptions = new QGridLayout;
    modelOptions->setContentsMargins(0, 0, 0, 0);
    streamResponses = new QCheckBox(tr("Stream"));
    streamResponses->setObjectName(QStringLiteral("agentStreamResponses"));
    streamResponses->setAccessibleName(tr("Show provider text as it arrives"));
    streamResponses->setToolTip(tr("When enabled, supported Chat Completions providers stream text. Complete JSON responses remain available when streaming is off."));
    if (QSettings *preferences = agentPreferences())
        streamResponses->setChecked(preferences->value(streamingResponsesKey, false).toBool());
    modelOptions->addWidget(streamResponses, 0, 0);
    authorize = new QCheckBox(tr("Auto"));
    authorize->setObjectName(QStringLiteral("agentAutomaticChanges"));
    authorize->setAccessibleName(tr("Allow supported changes to run automatically"));
    authorize->setToolTip(tr("Limits automatic changes to documents selected when the task starts and the connected reference workspace. Extension proposals still require review."));
    if (QSettings *preferences = agentPreferences())
        authorize->setChecked(preferences->value(automaticChangesKey, false).toBool());
    modelOptions->addWidget(authorize, 0, 1);
    auto *authorizeExperiment = new QToolButton(this);
    authorizeExperiment->setObjectName(QStringLiteral("agentAuthorizeBoundedExperiment"));
    authorizeExperiment->setText(tr("Authorize experiment…"));
    authorizeExperiment->setToolTip(tr("Load and review one structured experiment authorization for the next task."));
    authorizeExperiment->setVisible(policy == AgentTaskRunner::AccessPolicy::ReadWrite);
    modelOptions->addWidget(authorizeExperiment, 1, 0, 1, 2, Qt::AlignLeft);
    connect(authorizeExperiment, &QToolButton::clicked, this, [this] {
        if (runner.isActive()) return;
        runner.clearBoundedExperimentAuthorization();
        const QString path = QFileDialog::getOpenFileName(this, tr("Load experiment authorization"), {}, tr("JSON files (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        { state->setText(tr("Could not read the bounded experiment authorization.")); return; }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        { state->setText(tr("The experiment authorization is not a JSON object.")); return; }
        const QJsonObject response = runner.prepareBoundedExperimentAuthorization(document.object());
        if (response.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("preview")))
        { state->setText(tr("Host rejected the experiment authorization: %1").arg(response.value(QStringLiteral("code")).toString())); return; }
        const QJsonObject candidate = response.value(QStringLiteral("result")).toObject();
        QString reviewError;
        if (!confirmTrustedExperimentAuthorization(this, candidate, &reviewError))
        {
            runner.clearBoundedExperimentAuthorization();
            state->setText(reviewError);
            return;
        }
        if (!runner.armBoundedExperimentAuthorization(candidate.value(QStringLiteral("previewId")).toString()))
        { state->setText(tr("Experiment authorization could not be armed.")); return; }
        state->setText(tr("One bounded experiment is authorized for the next task."));
    });
    modelOptions->setColumnStretch(2, 1);
    if (policy == AgentTaskRunner::AccessPolicy::ReadOnly)
    {
        authorize->setChecked(false);
        authorize->hide();
        streamResponses->setChecked(false);
        streamResponses->hide();
        settingsButton->hide();
    }

    goal = new QPlainTextEdit;
    goal->setObjectName(QStringLiteral("agentGoal"));
    goal->setAccessibleName(tr("Message to the assistant"));
    goal->setPlaceholderText(tr("Message the assistant…"));
    goal->setMinimumHeight(64);
    goal->setMaximumHeight(92);
    goal->setTabChangesFocus(true);
    rootLayout->addWidget(goal);

    // Preserve the historic widget lookup while keeping the credential out of
    // the conversation sidebar and all QSettings values.
    key = new QLineEdit(this);
    key->setObjectName(QStringLiteral("agentApiKey"));
    key->setEchoMode(QLineEdit::Password);
    key->setAccessibleName(tr("Assistant API key"));
    key->hide();

    auto *composerButtons = new QHBoxLayout;
    send = new QPushButton(tr("Send"));
    send->setObjectName(QStringLiteral("agentSend"));
    send->setAccessibleName(tr("Send message to the assistant"));
    send->setDefault(true);
    stop = new QPushButton(tr("Stop"));
    stop->setObjectName(QStringLiteral("agentStop"));
    stop->setAccessibleName(tr("Stop assistant work"));
    composerButtons->addWidget(send, 1);
    composerButtons->addWidget(stop);
    readOnlyQuestion = new QPushButton(tr("Read-only question"), this);
    readOnlyQuestion->setObjectName(QStringLiteral("agentAskReadOnly"));
    readOnlyQuestion->setAccessibleName(tr("Ask independently without taking over the current task"));
    readOnlyQuestion->setVisible(policy == AgentTaskRunner::AccessPolicy::ReadWrite);
    composerButtons->addWidget(readOnlyQuestion);
    rootLayout->addLayout(composerButtons);
    rootLayout->addLayout(modelForm);
    rootLayout->addLayout(modelOptions);

    // Keep keyboard traversal aligned with the visible composer layout. The
    // model controls are constructed before the editor, but appear below the
    // editor and send actions on screen.
    QWidget::setTabOrder(goal, send);
    QWidget::setTabOrder(send, stop);
    QWidget::setTabOrder(stop, readOnlyQuestion);
    QWidget::setTabOrder(readOnlyQuestion, model);
    QWidget::setTabOrder(model, effort);
    QWidget::setTabOrder(effort, streamResponses);
    QWidget::setTabOrder(streamResponses, authorize);

    connect(newChat, &QToolButton::clicked, this, [this] {
        if (runner.isActive()) return;
        runner.clearConversation();
        currentConversationId.clear();
        conversationRevision.clear();
        conversationMessageCount = 0;
        createConversation();
        transcript->clear();
        for (const QString &cardKey : activityCardOrder)
        {
            if (activityCardFrames.value(cardKey)) activityCardFrames.value(cardKey)->deleteLater();
        }
        activityCardFrames.clear();
        activityCardLabels.clear();
        activityCardOrder.clear();
        activityScroll->hide();
        goal->clear();
        preview->clear();
        presentedProposalId.clear();
        presentedQuestionId.clear();
        questionText->clear();
        state->setText(tr("Ready"));
        updateAvailability();
        goal->setFocus(Qt::OtherFocusReason);
    });
    connect(settingsButton, &QToolButton::clicked, this, &AgentWidget::openSettings);
    connect(send, &QPushButton::clicked, this, &AgentWidget::submitDraft);
    connect(readOnlyQuestion, &QPushButton::clicked, this, [this] { startReadOnlyQuestion(goal->toPlainText()); });
    connect(goal, &QPlainTextEdit::textChanged, this, [this] { updateAvailability(); });
    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), goal);
    connect(sendShortcut, &QShortcut::activated, this, &AgentWidget::submitDraft);
    auto *sendShortcutEnter = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), goal);
    connect(sendShortcutEnter, &QShortcut::activated, this, &AgentWidget::submitDraft);
    connect(stop, &QPushButton::clicked, &runner, &AgentTaskRunner::cancel);
    connect(apply, &QPushButton::clicked, this, [this] {
        if (presentedProposalId.isEmpty()) return;
        runner.authorizePending(presentedProposalId, true);
        updateAvailability();
    });
    connect(decline, &QPushButton::clicked, this, [this] {
        if (presentedProposalId.isEmpty()) return;
        runner.authorizePending(presentedProposalId, false);
        updateAvailability();
    });
    connect(model, &QComboBox::currentTextChanged, this, [this](const QString &value) {
        providerSettings.setModel(value.trimmed());
        persistConversationPreferences();
    });
    connect(model->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        providerSettings.setModel(model->currentText().trimmed());
        persistConversationPreferences();
    });
    connect(effort, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        selectedEffort = effort->currentData().toString();
        persistConversationPreferences();
    });
    connect(streamResponses, &QCheckBox::toggled, this, [this] { persistConversationPreferences(); });
    connect(authorize, &QCheckBox::toggled, this, [this] { persistConversationPreferences(); });

    connect(&runner, &AgentTaskRunner::turnRequested, this, &AgentWidget::requestTurn);
    connect(&runner, &AgentTaskRunner::taskStarted, this, [this](const QString &taskId, const QString &text, bool) {
        lastProviderCaptureContext = {};
        providerCaptureToolIds.clear();
        providerCaptureToolCallNames.clear();
        providerCaptureToolNames.clear();
        recordMessage(QStringLiteral("user"), text, taskId);
        pruneConversationTaskRoutes();
        if (lastMessagePersisted && taskConversationRoutes.size() < 128)
        {
            taskConversationRoutes.insert(taskId, QJsonObject{{QStringLiteral("workspace"), storeWorkspace},
                {QStringLiteral("projectId"), storeProjectId}, {QStringLiteral("conversationId"), currentConversationId}});
            conversationTaskOrder.append(taskId);
        }
    });
    connect(&runner, &AgentTaskRunner::pendingMessagesCancelled, this, [this](const QStringList &messages) {
        for (const QString &message : messages)
        {
            recordMessage(QStringLiteral("user"), message);
            const QString explanation = tr("This queued message was cancelled before its Agent task started. No analysis or action was performed.");
            recordMessage(QStringLiteral("fact"), explanation, {},
                {{QStringLiteral("status"), QStringLiteral("cancelled_before_start")},
                 {QStringLiteral("code"), QStringLiteral("INPUT_CANCELLED_BEFORE_TASK_START")}});
            appendText(tr("Message not processed"), explanation);
        }
    });
    runner.setExecutionRecorder([this](const QString &taskId, const QString &origin, const QJsonObject &reference) {
        return recordExecutionIntent(taskId, origin, reference);
    });
    connect(&runner, &AgentTaskRunner::executionObserved, this, &AgentWidget::recordExecutionObservation);
    connect(&runner, &AgentTaskRunner::executionObservationUnknown, this,
        [this](const QString &taskId, const QString &, const QJsonObject &reference, const QString &code) {
            auto facts = executionReferenceFacts(reference);
            const bool notSubmitted = code == QStringLiteral("NOT_SUBMITTED");
            const bool rejected = notSubmitted || code.startsWith(QStringLiteral("REJECTED:"));
            facts.insert(QStringLiteral("status"), rejected ? QStringLiteral("rejected") : QStringLiteral("unknown"));
            facts.insert(QStringLiteral("code"), notSubmitted ? code : rejected ? code.mid(9).left(128)
                                                                               : QStringLiteral("RESULT_RECONCILE_REQUIRED"));
            if (appendExecutionFact(taskConversationRoutes.value(taskId), taskId,
                notSubmitted ? tr("The recorded execution was not submitted; no action was retried.")
                    : rejected ? tr("The host rejected the recorded execution. No retry was authorized.")
                               : tr("The execution outcome is unknown. Query its recorded reference before another action."), facts)
                && rejected)
                pendingExecutionReferences.remove(taskId + QStringLiteral(":")
                    + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()));
        });
    if (workflow)
        connect(workflow, &WorkflowProcess::replyReceived, this,
            [this](const QString &id, const QString &operation, const QJsonObject &result) {
                const auto pending = receiptQueries.take(id);
                if (pending.isEmpty() || operation != QStringLiteral("execution.query")) return;
                const auto reference = pending.value(QStringLiteral("reference")).toObject();
                if (result.value(QStringLiteral("projectId")) != reference.value(QStringLiteral("workflowProjectId"))
                    || result.value(QStringLiteral("requestSha256")) != reference.value(QStringLiteral("requestSha256"))) return;
                if (result.value(QStringLiteral("accepted")).toBool())
                    recordReceiptObservation(pending.value(QStringLiteral("route")).toObject(),
                        pending.value(QStringLiteral("taskId")).toString(), reference, result);
            });
    if (workflow)
        connect(workflow, &WorkflowProcess::requestFailed, this,
            [this](const QString &id, const QString &, const QString &, const QString &) { receiptQueries.remove(id); });
    connect(&runner, &AgentTaskRunner::providerCancellationRequested, this, [this](const QString &id) {
        retireProviderRequest(id);
    });
    connect(&runner, &AgentTaskRunner::toolActivity, this, &AgentWidget::onToolActivity);
    connect(&runner, &AgentTaskRunner::toolResultObserved, this, [this](const QJsonObject &facts) {
        if (!providerCaptureRecorder) return;
        QJsonObject event = lastProviderCaptureContext;
        if (event.isEmpty() || event.value("taskId") != facts.value("taskId"))
        { providerCaptureFailed = true; runner.cancel(); return; }
        for (auto it = facts.begin(); it != facts.end(); ++it) event.insert(it.key(), it.value());
        const QString localId = providerCaptureToolIds.value(facts.value("toolCallId").toString());
        if (localId.isEmpty()) { providerCaptureFailed = true; runner.cancel(); return; }
        event.insert("toolCallId", localId);
        if (!providerCaptureToolNames.contains(facts.value("toolName").toString()))
            event.insert("toolName", "unregistered");
        if (!recordProviderCapture(event)) runner.cancel();
    });
    connect(&runner, &AgentTaskRunner::experimentObserved, this,
            [this](const QString &taskId, const QString &experimentId, const QJsonObject &hostSummary) {
        if (experimentId.isEmpty()
            || hostSummary.value(QStringLiteral("experimentId")).toString() != experimentId) return;
        const QString evidence = trustedExperimentEvidenceText(experimentId, hostSummary);
        const QJsonObject verification = hostSummary.value(QStringLiteral("verification")).toObject();
        const QJsonObject facts{{QStringLiteral("experimentId"), experimentId},
            {QStringLiteral("state"), hostSummary.value(QStringLiteral("state"))},
            {QStringLiteral("verificationPassed"), verification.value(QStringLiteral("passed"))},
            {QStringLiteral("verificationStatus"), verification.value(QStringLiteral("status"))},
            {QStringLiteral("convergenceStatus"), hostSummary.value(QStringLiteral("convergence")).toObject()
                .value(QStringLiteral("status"))}};
        recordMessage(QStringLiteral("fact"), evidence, taskId, facts);
        appendText(tr("Trusted Host evidence · %1").arg(experimentId), evidence);
    });
    connect(&runner, &AgentTaskRunner::experimentEvidenceObserved, this,
            [this](const QString &taskId, const QString &toolCallId, const QString &experimentId,
                   const QJsonObject &hostSummary) {
        if (!providerCaptureRecorder) return;
        const auto failCapture = [this] {
            providerCaptureFailed = true;
            runner.cancel();
        };
        const QString localToolCallId = providerCaptureToolIds.value(toolCallId);
        const QString toolName = providerCaptureToolCallNames.value(toolCallId);
        const QString stateName = hostSummary.value(QStringLiteral("state")).toString();
        if (taskId.isEmpty() || taskId != runner.taskId() || experimentId.isEmpty()
            || hostSummary.value(QStringLiteral("experimentId")).toString() != experimentId
            || !AgentTaskRunner::isKnownExperimentLifecycleState(stateName)
            || (toolName != QStringLiteral("experiment_query")
                && toolName != QStringLiteral("experiment_wait")
                && toolName != QStringLiteral("experiment_cancel"))
            || localToolCallId.isEmpty() || lastProviderCaptureContext.isEmpty()
            || lastProviderCaptureContext.value(QStringLiteral("taskId")).toString() != taskId
            || lastProviderCaptureContext.value(QStringLiteral("conversationId")).toString().isEmpty())
        {
            failCapture();
            return;
        }

        QJsonObject observation = lastProviderCaptureContext;
        observation.insert(QStringLiteral("kind"), QStringLiteral("host_observation"));
        observation.insert(QStringLiteral("toolCallId"), localToolCallId);
        observation.insert(QStringLiteral("toolName"), toolName);
        observation.insert(QStringLiteral("experimentId"), experimentId);
        observation.insert(QStringLiteral("state"), stateName);
        const QJsonValue verificationPassed = hostSummary.value(QStringLiteral("verificationPassed"));
        if (!verificationPassed.isUndefined())
        {
            if (!verificationPassed.isBool()) { failCapture(); return; }
            observation.insert(QStringLiteral("verificationPassed"), verificationPassed);
        }
        if (!recordProviderCapture(observation)) { runner.cancel(); return; }

        // A completed, verified candidate is joined to the Host's read-only
        // native evidence snapshot. Never infer a result from Agent prose.
        if (stateName != QStringLiteral("candidate_ready") || !verificationPassed.toBool()) return;
        const QJsonObject snapshot = commandHost
            ? commandHost->evaluationEvidenceSnapshot(experimentId) : QJsonObject{};
        const QJsonObject sourceBinding = snapshot.value(QStringLiteral("sourceBinding")).toObject();
        const QJsonObject sourceRuntimeStamp = snapshot.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const QJsonObject childJob = snapshot.value(QStringLiteral("childJob")).toObject();
        const QJsonObject solveIdentity = snapshot.value(QStringLiteral("latestSolveIdentity")).toObject();
        const QJsonObject report = snapshot.value(QStringLiteral("verificationReport")).toObject();
        const QJsonObject reportIdentity = report.value(QStringLiteral("identity")).toObject();
        const QJsonObject reportMetrics = report.value(QStringLiteral("metrics")).toObject();
        const QJsonObject sourceSnapshotRef = solveIdentity.value(QStringLiteral("sourceSnapshotRef")).toObject();
        const auto boundedText = [](const QJsonValue &value, int maximum = 256) {
            return value.isString() && !value.toString().trimmed().isEmpty()
                && value.toString().size() <= maximum;
        };
        const auto sha256 = [](const QString &value) {
            static const QRegularExpression grammar(QStringLiteral("^[0-9a-f]{64}$"));
            return grammar.match(value).hasMatch();
        };
        const auto objectHash = [](const QJsonObject &value) {
            return QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        };
        const QString sourceSnapshotRefSha256 = objectHash(sourceSnapshotRef);
        const QByteArray metricsJson = QJsonDocument(reportMetrics).toJson(QJsonDocument::Compact);
        const QString metricsSha256 = QString::fromLatin1(QCryptographicHash::hash(
            metricsJson, QCryptographicHash::Sha256).toHex());
        const QString reportSha256 = snapshot.value(QStringLiteral("verificationReportSha256")).toString();
        const QJsonValue bindingRevision = sourceBinding.value(QStringLiteral("documentRevision"));
        const QString documentRevision = bindingRevision.isString() ? bindingRevision.toString() : QString();
        const bool sourceIdentityValid = boundedText(sourceBinding.value(QStringLiteral("documentId")))
                && boundedText(sourceBinding.value(QStringLiteral("documentRevision")))
                && boundedText(sourceBinding.value(QStringLiteral("sessionId")))
                && boundedText(sourceBinding.value(QStringLiteral("sessionEpoch")))
                && sha256(sourceBinding.value(QStringLiteral("snapshotSha256")).toString())
                && sourceBinding.value(QStringLiteral("documentId")) == sourceRuntimeStamp.value(QStringLiteral("documentId"))
                && documentRevision == sourceRuntimeStamp.value(QStringLiteral("revision")).toString()
                && sourceBinding.value(QStringLiteral("sessionEpoch")) == sourceRuntimeStamp.value(QStringLiteral("sessionEpoch"))
                && sourceBinding.value(QStringLiteral("sessionId")) != sourceBinding.value(QStringLiteral("sessionEpoch"));
        const bool solveIdentityValid = boundedText(solveIdentity.value(QStringLiteral("artifactId")))
                && boundedText(solveIdentity.value(QStringLiteral("jobId")))
                && sha256(solveIdentity.value(QStringLiteral("inputSnapshotSha256")).toString())
                && sha256(solveIdentity.value(QStringLiteral("inputPackageSha256")).toString())
                && !sourceSnapshotRef.isEmpty()
                && childJob.value(QStringLiteral("state")) == QJsonValue(QStringLiteral("succeeded"))
                && childJob.value(QStringLiteral("jobId")) == solveIdentity.value(QStringLiteral("jobId"))
                && childJob.value(QStringLiteral("resultArtifactId")) == solveIdentity.value(QStringLiteral("artifactId"))
                && childJob.value(QStringLiteral("resultManifestVerified")).toBool()
                && sha256(childJob.value(QStringLiteral("resultManifestSha256")).toString())
                && report.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("pass"))
                && report.value(QStringLiteral("passed")).toBool()
                && !reportMetrics.isEmpty()
                && reportIdentity.value(QStringLiteral("artifactId")) == solveIdentity.value(QStringLiteral("artifactId"))
                && reportIdentity.value(QStringLiteral("jobId")) == solveIdentity.value(QStringLiteral("jobId"))
                && reportIdentity.value(QStringLiteral("inputSnapshotSha256")) == solveIdentity.value(QStringLiteral("inputSnapshotSha256"))
                && reportIdentity.value(QStringLiteral("inputPackageSha256")) == solveIdentity.value(QStringLiteral("inputPackageSha256"))
                && reportIdentity.value(QStringLiteral("sourceSnapshotRef")) == solveIdentity.value(QStringLiteral("sourceSnapshotRef"))
                && reportIdentity.value(QStringLiteral("manifestSha256")) == childJob.value(QStringLiteral("resultManifestSha256"))
                && sha256(reportSha256) && sha256(metricsSha256)
                && reportSha256 == objectHash(report)
                && sha256(snapshot.value(QStringLiteral("authorizationDigest")).toString())
                && boundedText(snapshot.value(QStringLiteral("attemptId")));
        if (snapshot.value(QStringLiteral("schema"))
                != QJsonValue(QStringLiteral("range.trusted-experiment-evidence-snapshot/v1"))
            || snapshot.value(QStringLiteral("experimentId")).toString() != experimentId
            || !sourceIdentityValid || !solveIdentityValid)
        {
            failCapture();
            return;
        }

        QJsonObject projectedSourceBinding{{QStringLiteral("documentId"), sourceBinding.value(QStringLiteral("documentId"))},
            {QStringLiteral("documentRevision"), sourceBinding.value(QStringLiteral("documentRevision"))},
            {QStringLiteral("sessionId"), sourceBinding.value(QStringLiteral("sessionId"))},
            {QStringLiteral("sessionEpoch"), sourceBinding.value(QStringLiteral("sessionEpoch"))},
            {QStringLiteral("snapshotSha256"), sourceBinding.value(QStringLiteral("snapshotSha256"))}};
        QJsonObject projectedSolveIdentity{{QStringLiteral("artifactId"), solveIdentity.value(QStringLiteral("artifactId"))},
            {QStringLiteral("jobId"), solveIdentity.value(QStringLiteral("jobId"))},
            {QStringLiteral("inputSnapshotSha256"), solveIdentity.value(QStringLiteral("inputSnapshotSha256"))},
            {QStringLiteral("inputPackageSha256"), solveIdentity.value(QStringLiteral("inputPackageSha256"))},
            {QStringLiteral("sourceSnapshotRefSha256"), sourceSnapshotRefSha256}};
        QJsonObject projectedChildJob{{QStringLiteral("state"), childJob.value(QStringLiteral("state"))},
            {QStringLiteral("jobId"), childJob.value(QStringLiteral("jobId"))},
            {QStringLiteral("attemptId"), childJob.value(QStringLiteral("attemptId"))},
            {QStringLiteral("resultArtifactId"), childJob.value(QStringLiteral("resultArtifactId"))},
            {QStringLiteral("resultManifestSha256"), childJob.value(QStringLiteral("resultManifestSha256"))}};
        QJsonObject projectedReportIdentity{{QStringLiteral("artifactId"), reportIdentity.value(QStringLiteral("artifactId"))},
            {QStringLiteral("jobId"), reportIdentity.value(QStringLiteral("jobId"))},
            {QStringLiteral("inputSnapshotSha256"), reportIdentity.value(QStringLiteral("inputSnapshotSha256"))},
            {QStringLiteral("inputPackageSha256"), reportIdentity.value(QStringLiteral("inputPackageSha256"))},
            {QStringLiteral("manifestSha256"), reportIdentity.value(QStringLiteral("manifestSha256"))},
            {QStringLiteral("normalizedProblemSpecSha256"), reportIdentity.value(QStringLiteral("normalizedProblemSpecSha256"))},
            {QStringLiteral("physicalSummarySha256"), reportIdentity.value(QStringLiteral("physicalSummarySha256"))},
            {QStringLiteral("sourceSnapshotRefSha256"), sourceSnapshotRefSha256}};
        constexpr qsizetype maximumCapturedMetricsBytes = 32 * 1024;
        const bool metricsPayloadCaptured = !metricsJson.isEmpty()
            && metricsJson.size() <= maximumCapturedMetricsBytes;
        QJsonObject projectedReport{{QStringLiteral("sha256"), reportSha256},
            {QStringLiteral("status"), report.value(QStringLiteral("status"))},
            {QStringLiteral("passed"), report.value(QStringLiteral("passed"))},
            {QStringLiteral("identity"), projectedReportIdentity},
            {QStringLiteral("metricsSha256"), metricsSha256},
            {QStringLiteral("metricsCaptureStatus"), metricsPayloadCaptured
                ? QStringLiteral("complete") : QStringLiteral("oversized_or_unavailable")}};
        if (metricsPayloadCaptured)
            projectedReport.insert(QStringLiteral("metricsJson"), QString::fromUtf8(metricsJson));
        QJsonObject evidence = lastProviderCaptureContext;
        // Host evidence uses the closed evaluator schema; Provider identity is
        // already captured on the adjacent Provider response event.
        evidence.remove(QStringLiteral("provider"));
        evidence.insert(QStringLiteral("kind"), QStringLiteral("host_evidence"));
        evidence.insert(QStringLiteral("evidenceSchema"),
            QStringLiteral("range.trusted-agent-stage7-physical-evidence/v1"));
        evidence.insert(QStringLiteral("toolCallId"), localToolCallId);
        evidence.insert(QStringLiteral("toolName"), toolName);
        evidence.insert(QStringLiteral("experimentId"), experimentId);
        evidence.insert(QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp);
        evidence.insert(QStringLiteral("sourceBinding"), projectedSourceBinding);
        evidence.insert(QStringLiteral("authorizationDigest"), snapshot.value(QStringLiteral("authorizationDigest")));
        evidence.insert(QStringLiteral("attemptId"), snapshot.value(QStringLiteral("attemptId")));
        evidence.insert(QStringLiteral("childJob"), projectedChildJob);
        evidence.insert(QStringLiteral("solveIdentity"), projectedSolveIdentity);
        evidence.insert(QStringLiteral("verificationReport"), projectedReport);
        if (!recordProviderCapture(evidence)) runner.cancel();
    });
    connect(&runner, &AgentTaskRunner::experimentCancellationObserved, this,
            [this](const QString &taskId, const QString &experimentId,
                   const QJsonObject &sourceBinding, const QJsonObject &cancellationFact) {
        if (experimentId.isEmpty()
            || cancellationFact.value(QStringLiteral("schema"))
                != QJsonValue(QStringLiteral("range.agent-experiment-cancellation/v1"))) return;
        QJsonObject safeFact{{QStringLiteral("schema"), cancellationFact.value(QStringLiteral("schema"))},
            {QStringLiteral("experimentId"), experimentId}};
        if (sourceBinding.value(QStringLiteral("documentId")).isString())
            safeFact.insert(QStringLiteral("sourceDocumentId"), sourceBinding.value(QStringLiteral("documentId")));
        if (sourceBinding.value(QStringLiteral("sessionEpoch")).isString())
            safeFact.insert(QStringLiteral("sourceSessionEpoch"), sourceBinding.value(QStringLiteral("sessionEpoch")));
        if (sourceBinding.value(QStringLiteral("authorizationDigest")).isString())
            safeFact.insert(QStringLiteral("authorizationDigest"), sourceBinding.value(QStringLiteral("authorizationDigest")));
        const QJsonValue sourceRevision = sourceBinding.value(QStringLiteral("documentRevision"));
        if (sourceRevision.isString()) safeFact.insert(QStringLiteral("sourceRevision"), sourceRevision);
        else if (sourceRevision.isDouble() && std::isfinite(sourceRevision.toDouble())
                 && sourceRevision.toDouble() >= 0.0 && std::floor(sourceRevision.toDouble()) == sourceRevision.toDouble()
                 && sourceRevision.toDouble() <= 9007199254740991.0)
            safeFact.insert(QStringLiteral("sourceRevision"), QString::number(quint64(sourceRevision.toDouble())));
        for (const QString &key : {QStringLiteral("trigger"), QStringLiteral("status"),
                                   QStringLiteral("code"), QStringLiteral("beforeState"),
                                   QStringLiteral("afterState"), QStringLiteral("childJobStates")})
            if (cancellationFact.value(key).isString()) safeFact.insert(key, cancellationFact.value(key));
        for (const QString &key : {QStringLiteral("cancellationPersisted"),
                                   QStringLiteral("childCancellationRequestFailed"),
                                   QStringLiteral("childJobsTerminal"), QStringLiteral("childJobSetMatches"),
                                   QStringLiteral("childJobsTruncated")})
            if (cancellationFact.value(key).isBool())
                safeFact.insert(key, cancellationFact.value(key).toBool() ? QStringLiteral("true") : QStringLiteral("false"));
        for (const QString &key : {QStringLiteral("childJobCount"), QStringLiteral("terminalChildJobCount")})
            if (cancellationFact.value(key).isDouble() && std::isfinite(cancellationFact.value(key).toDouble())
                && cancellationFact.value(key).toDouble() >= 0.0
                && std::floor(cancellationFact.value(key).toDouble()) == cancellationFact.value(key).toDouble())
                safeFact.insert(key, QString::number(qint64(cancellationFact.value(key).toDouble())));
        const auto boolEvidence = [this, &cancellationFact](const QString &key) {
            const QJsonValue value = cancellationFact.value(key);
            return value.isBool() ? (value.toBool() ? tr("true") : tr("false")) : tr("unknown");
        };
        const auto countEvidence = [this, &cancellationFact](const QString &key) {
            const QJsonValue value = cancellationFact.value(key);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0.0
                || std::floor(value.toDouble()) != value.toDouble()) return tr("unknown");
            return QString::number(qint64(value.toDouble()));
        };
        const QString childStates = cancellationFact.value(QStringLiteral("childJobStates")).isString()
            ? cancellationFact.value(QStringLiteral("childJobStates")).toString().left(128) : tr("unknown");
        const QString evidence = tr("The Host checked the experiment authorization bound to document %1 at revision %2.\n"
                                    "Cancellation status: %3 (%4). Persisted=%5; child job set matches=%6; child jobs terminal=%7 (%8/%9), states=%10; child cancel request failed=%11.")
            .arg(safeFact.value(QStringLiteral("sourceDocumentId")).toString().left(128),
                 safeFact.value(QStringLiteral("sourceRevision")).toString().left(32),
                 cancellationFact.value(QStringLiteral("status")).toString().left(32),
                 cancellationFact.value(QStringLiteral("code")).toString().left(64),
                 boolEvidence(QStringLiteral("cancellationPersisted")),
                 boolEvidence(QStringLiteral("childJobSetMatches")),
                 boolEvidence(QStringLiteral("childJobsTerminal")),
                 countEvidence(QStringLiteral("terminalChildJobCount")),
                 countEvidence(QStringLiteral("childJobCount")),
                 childStates,
                 boolEvidence(QStringLiteral("childCancellationRequestFailed")));
        recordMessage(QStringLiteral("fact"), evidence, taskId, safeFact);
        appendText(tr("Trusted Host cancellation · %1").arg(experimentId), evidence);
    });
    connect(&runner, &AgentTaskRunner::stateChanged, this, [this](const QString &status, const QString &message) {
        const QMap<QString, QString> labels{{QStringLiteral("analyzing"), tr("Thinking")},
            {QStringLiteral("awaiting_input"), tr("Waiting for your answer")},
            {QStringLiteral("awaiting_authorization"), tr("Needs approval")},
            {QStringLiteral("executing"), tr("Working")}, {QStringLiteral("waiting"), tr("Waiting for calculation")},
            {QStringLiteral("completed"), tr("Completed")}, {QStringLiteral("failed"), tr("Request failed")},
            {QStringLiteral("budget_exhausted"), tr("Task limit reached")},
            {QStringLiteral("cancelled"), tr("Stopped")}, {QStringLiteral("superseded"), tr("Continuing")}};
        state->setText(labels.value(status, message.left(100)));
        state->setToolTip(message);
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::assistantMessage, this, [this](const QString &message) {
        const QString previousProjectId = storeProjectId;
        const QString previousWorkspace = storeWorkspace;
        const bool bound = bindConversationStore();
        if (!previousProjectId.isEmpty() && (!bound || previousProjectId != storeProjectId || previousWorkspace != storeWorkspace)) return;
        recordMessage(QStringLiteral("assistant"), message, runner.taskId());
        if (finalizingStreamMessage && message == finalizingStreamText)
        {
            const QString requestId = liveStreamRequestId;
            finalizingStreamMessage = false;
            finalizingStreamText.clear();
            finishStreamText(requestId, message, true);
            return;
        }
        if (!message.isEmpty()) appendText(tr("Assistant"), message, true);
    });
    connect(&runner, &AgentTaskRunner::userInputRequested, this,
            [this](const QString &id, const QString &question, const QJsonArray &options) {
        const QString previousProjectId = storeProjectId;
        const QString previousWorkspace = storeWorkspace;
        const bool bound = bindConversationStore();
        if (!previousProjectId.isEmpty() && (!bound || previousProjectId != storeProjectId || previousWorkspace != storeWorkspace)) return;
        presentedQuestionId = id;
        questionText->setText(question);
        recordMessage(QStringLiteral("assistant"), question, runner.taskId());
        appendText(tr("Assistant"), question);
        while (QLayoutItem *item = questionOptionsLayout->takeAt(0))
        {
            if (QWidget *widget = item->widget()) widget->deleteLater();
            delete item;
        }
        int index = 0;
        for (const QJsonValue &option : options)
        {
            const QString label = optionLabel(option);
            const QString value = optionValue(option);
            if (label.isEmpty() || value.isEmpty()) continue;
            auto *button = new QPushButton(label, questionOptions);
            button->setText(button->fontMetrics().elidedText(label, Qt::ElideRight, 170));
            button->setToolTip(label);
            button->setAccessibleName(tr("Answer: %1").arg(label));
            button->setMinimumWidth(0);
            button->setMaximumWidth(180);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            const QString questionId = id;
            connect(button, &QPushButton::clicked, this, [this, questionId, value] {
                answerQuestion(questionId, value, false);
            });
            questionOptionsLayout->addWidget(button, index / 2, index % 2);
            ++index;
        }
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::proposalReady, this,
            [this](const QString &origin, const QString &id, const QJsonObject &value) {
        presentedProposalId = id;
        trustedExperimentReviewComplete = true;
        const auto normalized = value.value("normalized").toObject();
        const bool hostCommandProposal = origin == QStringLiteral("document")
            || origin == QStringLiteral("experiment");
        const auto command = hostCommandProposal ? value.value("command").toString()
                                                  : normalized.value("command").toString();
        if ((origin == QStringLiteral("document") || origin == QStringLiteral("experiment"))
            && command == QStringLiteral("experiment.run"))
        {
            preview->setMinimumHeight(180);
            preview->setMaximumHeight(360);
            preview->setPlainText(trustedExperimentReviewText(value, runner.taskId(), &trustedExperimentReviewComplete));
            updateAvailability();
            return;
        }
        preview->setMinimumHeight(0);
        preview->setMaximumHeight(110);
        QStringList lines;
        const QMap<QString, QString> labels{{"analysis.modal.configure", tr("Change modal settings")},
            {"document.undo", tr("Undo last document change")}, {"document.redo", tr("Redo document change")},
            {"workflow.configure", tr("Configure reference experiment")}, {"workflow.run", tr("Run reference experiment")},
            {"workflow.resume", tr("Continue reference calculation")},
            {"extension.stage", tr("Stage extension source and tests for review")},
            {"extension.validate", tr("Execute extension tests in trusted developer mode")},
            {"extension.activate", tr("Activate validated extension version")},
            {"extension.rollback", tr("Restore retained extension version")},
            {"extension.run", tr("Run validated extension")}};
        lines << labels.value(command, command);
        const auto before = value.value("before").toObject();
        const auto after = origin == "document" ? value.value("after").toObject() : normalized.value("parameters").toObject();
        const QMap<QString, QString> fieldLabels{{"iterations", tr("Iterations")}, {"method", tr("Method")},
            {"modes", tr("Modes")}, {"tolerance", tr("Tolerance")}, {"unit", tr("Unit")},
            {"model", tr("Reference model")}, {"dt", tr("Time step (s)")}, {"steps", tr("Steps")},
            {"targetDamping", tr("Target damping ratio")}, {"mode", tr("Control comparison")},
            {"additionalSteps", tr("Additional steps")}, {"independentReference", tr("Independent numerical check")}};
        for (auto it = after.begin(); it != after.end(); ++it)
        {
            if (before.contains(it.key()) && before.value(it.key()) == it.value()) continue;
            const auto rendered = it.value().isObject() || it.value().isArray()
                ? (origin == "extension"
                    ? QString::fromUtf8((it.value().isObject() ? QJsonDocument(it.value().toObject())
                                                              : QJsonDocument(it.value().toArray())).toJson(QJsonDocument::Indented))
                    : tr("Structured data; see task description")) : it.value().toVariant().toString();
            lines << (before.contains(it.key())
                ? tr("%1: %2 → %3").arg(fieldLabels.value(it.key(), it.key()), before.value(it.key()).toVariant().toString(), rendered)
                : tr("%1: %2").arg(fieldLabels.value(it.key(), it.key()), rendered));
        }
        if (before.contains("historyAction")) lines << before.value("historyAction").toString();
        preview->setPlainText(lines.join('\n'));
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::actionCompleted, this, [this](const QString &origin, const QJsonObject &value) {
        const bool trustedExperiment = (origin == QStringLiteral("document")
            || origin == QStringLiteral("experiment"))
            && value.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("experiment.run"));
        const bool nativeTask = origin == "document" && value.value("command").toString().startsWith("analysis.")
            && (value.value("command") == QJsonValue("analysis.run") || value.value("command") == QJsonValue("analysis.recover")
                || value.value("command") == QJsonValue("analysis.cancel"));
        appendText(trustedExperiment ? tr("Trusted experiment submitted")
                     : nativeTask ? tr("Native task action accepted")
                     : origin == "document" ? tr("Document updated") : tr("Task submitted"),
                   trustedExperiment
                     ? tr("Experiment %1 was submitted as an isolated candidate. The source document remains unchanged; submission does not mean verification passed.")
                           .arg(value.value(QStringLiteral("experimentId")).toString())
                     : nativeTask ? tr("The host accepted the task action. Query its actual progress and terminal result in Document FE output.")
                     : origin == "document" ? tr("The host accepted the change. The document views now reflect its current state.")
                                        : tr("Calculation task: %1. Progress and results are available in the workflow panel.").arg(value.value("jobId").toString()));
        presentedProposalId.clear();
        trustedExperimentReviewComplete = true;
        preview->setMinimumHeight(0);
        preview->setMaximumHeight(110);
        preview->clear();
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::finished, this, [this](const QString &status, const QString &message) {
        providerCaptureTaskFinalizing = true;
        if (providerCaptureRecorder && !lastProviderCaptureContext.isEmpty())
        {
            QJsonObject event = lastProviderCaptureContext;
            event.insert("kind", "task_terminal"); event.insert("status", status);
            recordProviderCapture(event);
        }
        providerCaptureTaskFinalizing = false;
        recordMessage(QStringLiteral("fact"), tr("Assistant task ended: %1. This does not determine a solver job's result.").arg(status),
                      runner.taskId(), QJsonObject{{QStringLiteral("status"), status}});
        if (status != QStringLiteral("completed") && !message.isEmpty()) appendText(status, message);
        if (providerCaptureRecorder && providerCaptureFailed)
            state->setText(tr("Evaluation capture incomplete; this task has no complete evidence record."));
        else if (status == QStringLiteral("completed")) state->setText(tr("Completed"));
        presentedProposalId.clear();
        trustedExperimentReviewComplete = true;
        presentedQuestionId.clear();
        preview->setMinimumHeight(0);
        preview->setMaximumHeight(110);
        preview->clear();
        updateAvailability();
        // AgentTaskRunner emits finished before clearing its retirement guard.
        // Refresh again on the next event-loop turn so a terminal task does not
        // leave New chat and Send disabled indefinitely.
        QTimer::singleShot(0, this, [this] { updateAvailability(); });
    });

    eventPollTimer.setInterval(750);
    connect(&eventPollTimer, &QTimer::timeout, this, &AgentWidget::refreshHostEvents);
    eventPollTimer.start();
    refreshConversationList();
    if (restoreLastConversation)
        if (QSettings *preferences = agentPreferences())
            loadConversation(preferences->value(lastConversationPreferenceKey()).toString());
    refreshHostEvents();
    updateAvailability();
}

AgentWidget::~AgentWidget()
{
    retireProviderRequest();
}

bool AgentWidget::setProviderCaptureRecorder(ProviderCaptureRecorder recorder)
{
    if (runner.isActive() || reply || providerCaptureCallbackDepth > 0 || providerCaptureTaskFinalizing) return false;
    providerCaptureRecorder = std::move(recorder);
    providerCaptureFailed = false;
    activeProviderCaptureContext = {}; lastProviderCaptureContext = {};
    return true;
}

bool AgentWidget::recordProviderCapture(const QJsonObject &event)
{
    const ProviderCaptureRecorder recorder = providerCaptureRecorder;
    if (!recorder) return true;
    if (providerCaptureFailed) return false;
    if (QJsonDocument(event).toJson(QJsonDocument::Compact).size() > 128 * 1024)
    { providerCaptureFailed = true; return false; }
    bool accepted = false;
    ++providerCaptureCallbackDepth;
    try { accepted = recorder(event); }
    catch (...) { /* Sink errors cannot authorize dispatch or leak exception text. */ }
    --providerCaptureCallbackDepth;
    if (accepted) return true;
    providerCaptureFailed = true;
    return false;
}

QString AgentWidget::lastConversationPreferenceKey() const
{
    return QStringLiteral("aiAgent/lastConversation/") + storeProjectId
        + (accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly ? QStringLiteral("/readOnly") : QStringLiteral("/primary"));
}

void AgentWidget::pruneConversationTaskRoutes()
{
    QSet<QString> protectedTasks;
    for (const auto &task : pendingExecutionReferences) protectedTasks.insert(task);
    for (const auto &pending : receiptQueries) protectedTasks.insert(pending.value(QStringLiteral("taskId")).toString());
    if (runner.isActive()) protectedTasks.insert(runner.taskId());
    for (qsizetype index = 0; taskConversationRoutes.size() >= 96 && index < conversationTaskOrder.size();)
    {
        const QString task = conversationTaskOrder.at(index);
        if (protectedTasks.contains(task)) { ++index; continue; }
        taskConversationRoutes.remove(task);
        conversationTaskOrder.removeAt(index);
        for (auto it = observedExecutionReferences.begin(); it != observedExecutionReferences.end();)
            if (it->contains(QStringLiteral(":") + task + QStringLiteral(":"))) it = observedExecutionReferences.erase(it);
            else ++it;
    }
}

void AgentWidget::clearConversationView()
{
    runner.clearConversation();
    state->setText(tr("Ready"));
    historyStatus->clear();
    transcript->clear();
    preview->clear();
    presentedProposalId.clear(); presentedQuestionId.clear();
    questionText->clear(); approvalCard->hide(); questionCard->hide();
    branchMessageChoice->clear();
    for (const auto &key : activityCardOrder)
        if (activityCardFrames.value(key)) activityCardFrames.value(key)->deleteLater();
    activityCardFrames.clear(); activityCardLabels.clear(); activityCardOrder.clear();
    activityScroll->hide();
}

bool AgentWidget::bindConversationStore()
{
    if (rebindingStore || !commandHost) return false;
    const auto projectResponse = hostRequest(QStringLiteral("project.query"));
    const auto project = projectResponse.value(QStringLiteral("result")).toObject();
    const QString workspace = commandHost->projectWorkspace();
    const QString projectId = project.value(QStringLiteral("projectId")).toString();
    if (projectResponse.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok"))
        || workspace.isEmpty() || projectId.isEmpty())
    {
        if (conversationStore)
        {
            rebindingStore = true;
            if (runner.isActive()) runner.cancel();
            retireProviderRequest();
            clearConversationView();
            conversationStore.reset(); storeWorkspace.clear(); storeProjectId.clear();
            currentConversationId.clear(); conversationRevision.clear(); conversationMessageCount = 0;
            const QSignalBlocker blocker(conversationChoice);
            conversationChoice->clear();
            historyStatus->setText(tr("Project history is unavailable. Previous records were preserved."));
            rebindingStore = false;
        }
        return false;
    }
    if (conversationStore && workspace == storeWorkspace && projectId == storeProjectId) return true;
    rebindingStore = true;
    if (conversationStore && runner.isActive()) runner.cancel();
    retireProviderRequest();
    clearConversationView();
    conversationStore = std::make_unique<RAgentConversationStore>();
    QString error;
    const bool configured = conversationStore->configure(workspace, projectId, &error);
    storeWorkspace = configured ? workspace : QString();
    storeProjectId = configured ? projectId : QString();
    currentConversationId.clear();
    conversationRevision.clear();
    conversationMessageCount = 0;
    if (!configured)
    {
        conversationStore.reset();
        historyStatus->setText(tr("Conversation storage is unavailable. Visible messages remain in this window."));
    }
    rebindingStore = false;
    return configured;
}

void AgentWidget::refreshConversationList()
{
    if (!bindConversationStore()) return;
    QString error;
    const auto listing = conversationStore->list(0, 32, &error);
    if (!error.isEmpty())
    {
        historyStatus->setText(tr("Saved history could not be verified. Existing records were preserved."));
        return;
    }
    const QSignalBlocker blocker(conversationChoice);
    conversationChoice->clear();
    conversationChoice->addItem(tr("New conversation"), QString());
    for (const auto &value : listing.value(QStringLiteral("conversations")).toArray())
    {
        const auto summary = value.toObject();
        const QString id = summary.value(QStringLiteral("conversationId")).toString();
        conversationChoice->addItem(summary.value(QStringLiteral("title")).toString().left(100)
            + QStringLiteral(" · ") + id.left(8), id);
    }
    const int current = conversationChoice->findData(currentConversationId);
    if (current >= 0) conversationChoice->setCurrentIndex(current);
}

void AgentWidget::createConversation()
{
    if (runner.isActive() || !bindConversationStore()) return;
    QString error;
    const auto summary = conversationStore->create(accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? tr("Read-only question") : tr("Conversation"), &error);
    if (summary.isEmpty())
    {
        historyStatus->setText(tr("A new conversation could not be saved. Existing history was preserved."));
        return;
    }
    clearConversationView();
    currentConversationId = summary.value(QStringLiteral("conversationId")).toString();
    conversationRevision = summary.value(QStringLiteral("revision")).toString();
    conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), currentConversationId);
    historyOffset = 0;
    branchMessageChoice->clear();
    historyStatus->setText(tr("Saved conversation · %1 messages. Historical text grants no approval.")
        .arg(conversationMessageCount));
    refreshConversationList();
    updateAvailability();
}

void AgentWidget::recordMessage(const QString &role, const QString &text, const QString &taskId,
                                const QJsonObject &facts)
{
    lastMessagePersisted = false;
    if (restoringHistory || rebindingStore || text.isEmpty()) return;
    const QString previousProjectId = storeProjectId;
    const QString previousWorkspace = storeWorkspace;
    if (!bindConversationStore() || (!previousProjectId.isEmpty()
        && (previousProjectId != storeProjectId || previousWorkspace != storeWorkspace))) return;
    conversationStore->setSensitiveValues({getProviderSettings().getApiKey(), requestSettingsSnapshot.getApiKey()});
    if (currentConversationId.isEmpty())
    {
        QString error;
        const auto summary = conversationStore->create(accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
            ? tr("Read-only question") : tr("Conversation"), &error);
        if (summary.isEmpty()) { historyStatus->setText(tr("This conversation could not be saved.")); return; }
        currentConversationId = summary.value(QStringLiteral("conversationId")).toString();
        conversationRevision = summary.value(QStringLiteral("revision")).toString();
        conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    }
    QJsonObject entry{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                      {QStringLiteral("role"), role}, {QStringLiteral("text"), text}};
    if (!taskId.isEmpty()) entry.insert(QStringLiteral("taskId"), taskId);
    if (!facts.isEmpty()) entry.insert(QStringLiteral("facts"), facts);
    QString error;
    QJsonObject summary;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        summary = conversationStore->append(currentConversationId, conversationRevision, QJsonArray{entry}, &error);
        if (!summary.isEmpty()) break;
        const auto current = conversationStore->page(currentConversationId, 0, 1, &error);
        const QString revision = current.value(QStringLiteral("revision")).toString();
        if (current.isEmpty() || revision == conversationRevision) break;
        conversationRevision = revision;
    }
    if (summary.isEmpty())
    {
        historyStatus->setText(tr("This message remains visible but could not be persisted. Existing history was preserved."));
        return;
    }
    conversationRevision = summary.value(QStringLiteral("revision")).toString();
    conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    lastMessagePersisted = true;
    historyOffset = qMax(0, conversationMessageCount - 50);
    branchMessageChoice->addItem(role + QStringLiteral(": ") + text.left(60), entry.value(QStringLiteral("messageId")).toString());
    if (branchMessageChoice->count() > 50) branchMessageChoice->removeItem(0);
    branchMessageChoice->setCurrentIndex(branchMessageChoice->count() - 1);
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), currentConversationId);
    historyStatus->setText(tr("Saved conversation · %1 messages. Historical text grants no approval.").arg(conversationMessageCount));
    refreshConversationList();
    updateAvailability();
}

bool AgentWidget::appendExecutionFact(const QJsonObject &route, const QString &taskId,
                                     const QString &text, const QJsonObject &facts)
{
    if (route.isEmpty() || taskId.isEmpty()) return false;
    RAgentConversationStore store;
    QString error;
    if (!store.configure(route.value(QStringLiteral("workspace")).toString(),
                         route.value(QStringLiteral("projectId")).toString(), &error)) return false;
    const QString id = route.value(QStringLiteral("conversationId")).toString();
    const auto page = store.page(id, 0, 1, &error);
    if (page.isEmpty()) return false;
    const QJsonObject entry{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("taskId"), taskId}, {QStringLiteral("role"), QStringLiteral("fact")},
        {QStringLiteral("text"), text}, {QStringLiteral("facts"), facts}};
    const auto summary = store.append(id, page.value(QStringLiteral("revision")).toString(), QJsonArray{entry}, &error);
    if (summary.isEmpty()) return false;
    if (route.value(QStringLiteral("workspace")).toString() == storeWorkspace
        && route.value(QStringLiteral("projectId")).toString() == storeProjectId && id == currentConversationId)
    {
        conversationRevision = summary.value(QStringLiteral("revision")).toString();
        conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
        historyStatus->setText(tr("Execution evidence was recorded in this conversation. Historical references grant no permission."));
    }
    return true;
}

bool AgentWidget::recordExecutionIntent(const QString &taskId, const QString &, const QJsonObject &reference)
{
    auto facts = executionReferenceFacts(reference);
    facts.insert(QStringLiteral("status"), QStringLiteral("pending"));
    facts.insert(QStringLiteral("code"), QStringLiteral("OUTCOME_NOT_OBSERVED"));
    const bool recorded = appendExecutionFact(taskConversationRoutes.value(taskId), taskId,
        tr("Execution intent recorded; acceptance is not yet observed. Query this reference before retrying."), facts);
    if (recorded) pendingExecutionReferences.insert(taskId + QStringLiteral(":")
        + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()), taskId);
    return recorded;
}

void AgentWidget::recordExecutionObservation(const QString &taskId, const QString &,
                                             const QJsonObject &reference, const QJsonObject &result)
{
    recordReceiptObservation(taskConversationRoutes.value(taskId), taskId, reference, result);
}

void AgentWidget::recordReceiptObservation(const QJsonObject &route, const QString &taskId,
                                            const QJsonObject &reference, const QJsonObject &result)
{
    if (route.isEmpty()) return;
    const QString key = route.value(QStringLiteral("workspace")).toString() + QStringLiteral(":")
        + route.value(QStringLiteral("conversationId")).toString() + QStringLiteral(":")
        + taskId + QStringLiteral(":") + reference.value(QStringLiteral("idempotencyKey"))
        .toString(reference.value(QStringLiteral("requestSha256")).toString());
    if (observedExecutionReferences.contains(key)) return;
    QJsonObject facts = executionReferenceFacts(reference);
    facts.insert(QStringLiteral("status"), QStringLiteral("accepted"));
    facts.insert(QStringLiteral("code"), QStringLiteral("OK"));
    for (const auto &field : {QStringLiteral("command"), QStringLiteral("operationId"), QStringLiteral("jobId"),
                             QStringLiteral("sourceDocumentId"), QStringLiteral("sourceRevision"),
                             QStringLiteral("acceptedModelSha256")})
        if (result.contains(field) && !result.value(field).toVariant().toString().isEmpty())
            facts.insert(field, result.value(field).toVariant().toString());
    const auto snapshot = result.value(QStringLiteral("acceptedSnapshotRef")).toObject();
    if (!snapshot.value(QStringLiteral("modelSha256")).toString().isEmpty())
        facts.insert(QStringLiteral("acceptedModelSha256"), snapshot.value(QStringLiteral("modelSha256")));
    if (appendExecutionFact(route, taskId,
        tr("The host accepted the referenced action. A submitted job's numerical result must be queried separately."), facts))
    {
        observedExecutionReferences.insert(key);
        pendingExecutionReferences.remove(taskId + QStringLiteral(":")
            + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()));
        runner.resolveExecutionObservation(taskId, reference);
    }
}

void AgentWidget::reconcileHistoryFacts(const QJsonArray &messages)
{
    const QJsonObject route{{QStringLiteral("workspace"), storeWorkspace}, {QStringLiteral("projectId"), storeProjectId},
                            {QStringLiteral("conversationId"), currentConversationId}};
    QString error;
    const auto complete = conversationStore ? conversationStore->exportConversation(currentConversationId, &error) : QJsonObject{};
    QSet<QString> resolved;
    for (const auto &value : complete.value(QStringLiteral("messages")).toArray())
    {
        const auto entry = value.toObject();
        const auto facts = entry.value(QStringLiteral("facts")).toObject();
        if (facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted"))
            || facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("rejected")))
            resolved.insert(entry.value(QStringLiteral("taskId")).toString() + QStringLiteral(":")
                + facts.value(QStringLiteral("idempotencyKey")).toString(facts.value(QStringLiteral("requestSha256")).toString()));
    }
    for (const auto &value : messages)
    {
        const auto entry = value.toObject();
        const auto facts = entry.value(QStringLiteral("facts")).toObject();
        if (entry.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("fact"))
            || (facts.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("pending"))
                && facts.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("unknown")))) continue;
        const QString taskId = entry.value(QStringLiteral("taskId")).toString();
        if (taskId.isEmpty() || resolved.contains(taskId + QStringLiteral(":")
            + facts.value(QStringLiteral("idempotencyKey")).toString(facts.value(QStringLiteral("requestSha256")).toString()))) continue;
        QJsonObject reference;
        for (const auto &field : {QStringLiteral("command"), QStringLiteral("idempotencyKey"),
                                 QStringLiteral("requestSha256"), QStringLiteral("workflowProjectId")})
            if (facts.contains(field)) reference.insert(field, facts.value(field));
        if (reference.contains(QStringLiteral("idempotencyKey")))
        {
            const QJsonObject request{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("operation"), QStringLiteral("operation.query")},
                {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("idempotencyKey"), reference.value(QStringLiteral("idempotencyKey"))}}}};
            const auto reply = commandHost ? commandHost->handleReadOnly(request) : QJsonObject{};
            const auto operation = reply.value(QStringLiteral("result")).toObject();
            const auto observation = operation.value(QStringLiteral("observation")).toObject();
            if (observation.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted")))
                recordReceiptObservation(route, taskId, reference, observation.value(QStringLiteral("result")).toObject());
            else if (operation.value(QStringLiteral("phase")) == QJsonValue(QStringLiteral("observed"))
                && observation.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("rejected")))
            {
                auto rejected = reference;
                rejected.insert(QStringLiteral("status"), QStringLiteral("rejected"));
                rejected.insert(QStringLiteral("code"), observation.value(QStringLiteral("code")));
                if (appendExecutionFact(route, taskId, tr("The durable host record confirms this execution was rejected."), rejected))
                {
                    runner.resolveExecutionObservation(taskId, reference);
                    pendingExecutionReferences.remove(taskId + QStringLiteral(":") + reference.value(QStringLiteral("idempotencyKey")).toString());
                }
            }
        }
        else if (reference.contains(QStringLiteral("requestSha256")) && connectedWorkflow && connectedWorkflow->isReady()
                 && receiptQueries.size() < 16)
        {
            const QString id = connectedWorkflow->request(QStringLiteral("execution.query"),
                {{QStringLiteral("requestSha256"), reference.value(QStringLiteral("requestSha256"))}});
            if (!id.isEmpty()) receiptQueries.insert(id, QJsonObject{{QStringLiteral("taskId"), taskId},
                {QStringLiteral("reference"), reference}, {QStringLiteral("route"), route}});
        }
    }
}

bool AgentWidget::loadConversation(const QString &id, QString *error)
{
    if (id.isEmpty() || runner.isActive() || !bindConversationStore()) return false;
    QString localError;
    const auto page = conversationStore->page(id, 0, 1, &localError);
    if (page.isEmpty())
    {
        if (error) *error = localError;
        historyStatus->setText(tr("This history is unavailable or invalid; existing records were preserved."));
        return false;
    }
    clearConversationView();
    currentConversationId = id;
    conversationRevision = page.value(QStringLiteral("revision")).toString();
    conversationMessageCount = page.value(QStringLiteral("messageCount")).toInt();
    { const QSignalBlocker blocker(authorize); authorize->setChecked(false); }
    renderHistoryPage(qMax(0, conversationMessageCount - 50));
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), id);
    refreshConversationList();
    updateAvailability();
    if (error) error->clear();
    return true;
}

void AgentWidget::renderHistoryPage(int offset)
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    const auto page = conversationStore->page(currentConversationId, qMax(0, offset), 50, &error);
    if (page.isEmpty()) { historyStatus->setText(tr("Historical messages could not be verified.")); return; }
    restoringHistory = true;
    transcript->clear();
    branchMessageChoice->clear();
    QJsonArray context, historicalFacts;
    for (const auto &value : page.value(QStringLiteral("messages")).toArray())
    {
        const auto entry = value.toObject();
        const QString role = entry.value(QStringLiteral("role")).toString();
        const QString text = entry.value(QStringLiteral("text")).toString();
        QString visibleText = text;
        if (role == QStringLiteral("fact"))
        {
            const auto facts = entry.value(QStringLiteral("facts")).toObject();
            for (const auto &field : {QStringLiteral("command"), QStringLiteral("operationId"), QStringLiteral("jobId"),
                                     QStringLiteral("sourceRevision"), QStringLiteral("acceptedModelSha256"),
                                     QStringLiteral("idempotencyKey"), QStringLiteral("requestSha256")})
                if (facts.contains(field)) visibleText += QStringLiteral("\n") + field + QStringLiteral(": ") + facts.value(field).toString();
            QJsonObject historical{{QStringLiteral("type"), QStringLiteral("history_fact")},
                {QStringLiteral("text"), tr("Historical evidence; query its reference before relying on it: %1").arg(visibleText).left(2000)}};
            if (facts.contains(QStringLiteral("jobId")))
                historical.insert(QStringLiteral("artifactRef"), QJsonObject{{QStringLiteral("jobId"), facts.value(QStringLiteral("jobId"))}});
            const auto sourceRefs = entry.value(QStringLiteral("sourceRefs")).toArray();
            if (!sourceRefs.isEmpty())
            {
                const auto source = sourceRefs.first().toObject();
                historical.insert(QStringLiteral("sourceRef"), QJsonObject{{QStringLiteral("sourceId"), source.value(QStringLiteral("sourceId"))},
                    {QStringLiteral("sourceRevision"), source.value(QStringLiteral("revision"))},
                    {QStringLiteral("modelSha256"), source.value(QStringLiteral("sha256"))}});
            }
            historicalFacts.append(historical);
        }
        appendText(role == QStringLiteral("user") ? tr("Historical user")
            : role == QStringLiteral("fact") ? tr("Historical host fact") : tr("Historical assistant"),
            visibleText, role == QStringLiteral("assistant"));
        branchMessageChoice->addItem(role + QStringLiteral(": ") + text.left(60), entry.value(QStringLiteral("messageId")).toString());
        if (role == QStringLiteral("user") || role == QStringLiteral("assistant"))
            context.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), text.left(2000)}});
    }
    QJsonArray boundedHistory;
    for (const auto &fact : historicalFacts)
    {
        if (boundedHistory.size() >= 8) break;
        boundedHistory.append(fact);
    }
    const qsizetype remaining = 12 - boundedHistory.size();
    for (qsizetype index = qMax(qsizetype(0), context.size() - remaining); index < context.size(); ++index)
        boundedHistory.append(context.at(index));
    runner.restoreObservableConversation(boundedHistory);
    historyOffset = page.value(QStringLiteral("offset")).toInt();
    conversationRevision = page.value(QStringLiteral("revision")).toString();
    conversationMessageCount = page.value(QStringLiteral("messageCount")).toInt();
    restoringHistory = false;
    historyStatus->setText(tr("Historical messages %1–%2 of %3. No pending proposal or approval was restored.")
        .arg(conversationMessageCount ? historyOffset + 1 : 0)
        .arg(historyOffset + page.value(QStringLiteral("messages")).toArray().size()).arg(conversationMessageCount));
    reconcileHistoryFacts(page.value(QStringLiteral("messages")).toArray());
    updateAvailability();
}

void AgentWidget::branchFromMessage()
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    const QString messageId = branchMessageChoice->currentData().toString();
    if (messageId.isEmpty()) return;
    QString error;
    const auto summary = conversationStore->branch(currentConversationId, messageId, tr("Conversation branch"), &error);
    if (summary.isEmpty()) { historyStatus->setText(tr("The branch could not be created. Existing history was preserved.")); return; }
    loadConversation(summary.value(QStringLiteral("conversationId")).toString());
    goal->setFocus(Qt::OtherFocusReason);
}

void AgentWidget::deleteConversation()
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    if (!conversationStore->remove(currentConversationId, conversationRevision, &error))
    { historyStatus->setText(tr("This conversation could not be deleted. Existing data was preserved.")); return; }
    currentConversationId.clear(); conversationRevision.clear(); conversationMessageCount = 0;
    runner.clearConversation(); transcript->clear(); branchMessageChoice->clear();
    historyStatus->setText(tr("Conversation deleted. Its historical actions and independent branches remain unchanged."));
    refreshConversationList(); updateAvailability();
}

void AgentWidget::exportConversation()
{
    if (currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    const auto snapshot = conversationStore->exportConversation(currentConversationId, &error);
    if (snapshot.isEmpty()) { historyStatus->setText(tr("This history could not be verified for export.")); return; }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export conversation"), QString(), tr("Conversation JSON (*.json)"));
    if (path.isEmpty()) return;
    const QByteArray bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { historyStatus->setText(tr("The export could not be published.")); return; }
    historyStatus->setText(tr("Exported verified historical messages and facts; no execution permissions are included."));
}

void AgentWidget::startReadOnlyQuestion(const QString &text)
{
    if (accessPolicy != AgentTaskRunner::AccessPolicy::ReadWrite || text.trimmed().isEmpty()) return;
    if (!readOnlySidecar)
    {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("agentReadOnlyDialog"));
        dialog->setWindowTitle(tr("Read-only question · current task continues"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->resize(600, 850);
        auto *layout = new QVBoxLayout(dialog);
        auto *sidecar = new AgentWidget(commandHost, connectedWorkflow, dialog, network,
                                        AgentTaskRunner::AccessPolicy::ReadOnly);
        layout->addWidget(sidecar);
        readOnlyDialog = dialog; readOnlySidecar = sidecar;
        connect(dialog, &QDialog::finished, this, [this] { if (goal) goal->setFocus(Qt::OtherFocusReason); });
    }
    auto *sidecar = readOnlySidecar.data();
    if (!sidecar) return;
    sidecar->setProviderSettings(getProviderSettings());
    sidecar->selectedEffort = selectedEffort;
    sidecar->updateEffortChoices();
    sidecar->streamResponses->setChecked(streamResponses->isChecked());
    sidecar->streamResponses->setChecked(false);
    readOnlyDialog->show(); readOnlyDialog->raise(); readOnlyDialog->activateWindow();
    if (sidecar->runner.isActive())
    {
        sidecar->goal->setPlainText(text);
        sidecar->submitDraft();
    }
    else sidecar->startTask(text, false);
}

RAgentSettings AgentWidget::getProviderSettings() const
{
    RAgentSettings settings(providerSettings);
    settings.setModel(model ? model->currentText().trimmed() : providerSettings.getModel());
    settings.setApiKey(key ? key->text() : providerSettings.getApiKey());
    return settings;
}

void AgentWidget::setProviderSettings(const RAgentSettings &settings)
{
    if (settings.getType() != RAgentSettings::Type::DeepSeek
        && settings.getType() != RAgentSettings::Type::OpenAILegacy) return;
    if (!isValidAgentEndpoint(settings.getApiUrl()))
    {
        if (state)
        {
            state->setText(tr("Invalid endpoint"));
            state->setToolTip(invalidEndpointMessage());
        }
        return;
    }
    providerSettings = settings;
    if (key) key->setText(settings.getApiKey());
    if (model && model->currentText() != settings.getModel()) model->setEditText(settings.getModel());
    updateEffortChoices();
    persistConversationPreferences();
    updateAvailability();
}

void AgentWidget::updateEffortChoices()
{
    if (!effort) return;
    const QSignalBlocker blocker(effort);
    effort->clear();
    if (providerSettings.getType() == RAgentSettings::Type::DeepSeek)
    {
        if (selectedEffort == QStringLiteral("default")) selectedEffort = QStringLiteral("off");
        effort->addItem(tr("Off"), QStringLiteral("off"));
        effort->addItem(tr("Low"), QStringLiteral("low"));
        effort->addItem(tr("High"), QStringLiteral("high"));
        effort->addItem(tr("Max"), QStringLiteral("max"));
        if (effort->findData(selectedEffort) < 0) selectedEffort = QStringLiteral("off");
        effort->setEnabled(true);
    }
    else
    {
        selectedEffort = QStringLiteral("default");
        effort->addItem(tr("Default"), QStringLiteral("default"));
        effort->setEnabled(false);
    }
    effort->setCurrentIndex(qMax(0, effort->findData(selectedEffort)));
}

void AgentWidget::startTask(const QString &text, bool authorized)
{
    if (runner.isActive()) return;
    const QString message = text.trimmed();
    if (message.isEmpty() || key->text().trimmed().isEmpty())
    {
        state->setText(message.isEmpty() ? tr("Enter a message first.") : tr("Add an API key in assistant settings."));
        return;
    }
    initialEvaluationPrompt = message;
    evaluationSlotBindingRecorded = false;
    requestSettingsSnapshot = getProviderSettings();
    bindConversationStore();
    requestEffortSnapshot = selectedEffort;
    requestStreamingSnapshot = accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && streamResponses->isChecked()
        && providerSupportsStreaming(requestSettingsSnapshot.getType());
    appendText(tr("You"), text);
    runner.startTask(evaluationSlotBinding.enabled ? message : text,
                     accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && authorized);
}

void AgentWidget::submitDraft()
{
    const QString message = goal->toPlainText().trimmed();
    if (message.isEmpty()) return;
    if (runner.isAwaitingInput())
    {
        answerQuestion(runner.pendingQuestionId(), message, true);
        return;
    }
    if (key->text().trimmed().isEmpty())
    {
        state->setText(tr("Add an API key in assistant settings."));
        openSettings();
        return;
    }
    initialEvaluationPrompt = message;
    evaluationSlotBindingRecorded = false;
    requestSettingsSnapshot = getProviderSettings();
    bindConversationStore();
    requestEffortSnapshot = selectedEffort;
    requestStreamingSnapshot = accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && streamResponses->isChecked()
        && providerSupportsStreaming(requestSettingsSnapshot.getType());
    appendText(tr("You"), message);
    goal->clear();
    runner.submitMessage(message, accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && authorize->isChecked());
    updateAvailability();
}

void AgentWidget::answerQuestion(const QString &questionId, const QString &answer, bool clearDraft)
{
    if (questionId.isEmpty() || questionId != runner.pendingQuestionId() || answer.trimmed().isEmpty()) return;
    if (clearDraft && goal->toPlainText().trimmed() != answer.trimmed()) return;
    appendText(tr("You"), answer);
    recordMessage(QStringLiteral("user"), answer, runner.taskId());
    if (clearDraft) goal->clear();
    runner.answerQuestion(questionId, answer);
    updateAvailability();
}

void AgentWidget::appendText(const QString &title, const QString &text, bool markdown)
{
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (markdown)
    {
        ConversationDocument message;
        message.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                  | QTextDocument::MarkdownNoHTML);
        QTextCursor cursor(transcript->document());
        cursor.movePosition(QTextCursor::End);
        const auto newParagraph = [&cursor] {
            cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
            if (auto *list = cursor.currentList()) list->remove(cursor.block());
        };
        if (!transcript->document()->isEmpty()) newParagraph();
        cursor.insertHtml(QStringLiteral("<p><b>%1</b></p>").arg(title.toHtmlEscaped()));
        newParagraph();
        cursor.insertFragment(QTextDocumentFragment(&message));
        newParagraph();
        restoreTranscriptScroll(followTail, previousScroll);
        return;
    }
    QString escapedText = text.toHtmlEscaped();
    escapedText.replace(QStringLiteral("\r\n"), QStringLiteral("<br/>"));
    escapedText.replace(QLatin1Char('\r'), QLatin1String("<br/>"));
    escapedText.replace(QLatin1Char('\n'), QLatin1String("<br/>"));
    const QString roleColor = title == tr("You")
        ? transcript->palette().color(QPalette::Link).name()
        : transcript->palette().color(QPalette::Text).name();
    QTextCursor cursor(transcript->document());
    cursor.movePosition(QTextCursor::End);
    if (!transcript->document()->isEmpty())
    {
        cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
        if (auto *list = cursor.currentList()) list->remove(cursor.block());
    }
    cursor.insertHtml(QStringLiteral("<p style='margin:9px 0 3px;color:%1'><b>%2</b></p>"
                                      "<p style='margin:0 0 7px;white-space:pre-wrap'>%3</p>")
                           .arg(roleColor, title.toHtmlEscaped(), escapedText));
    restoreTranscriptScroll(followTail, previousScroll);
}

bool AgentWidget::transcriptFollowsTail() const
{
    const auto *scrollBar = transcript->verticalScrollBar();
    return scrollBar->value() >= scrollBar->maximum() - 1;
}

void AgentWidget::restoreTranscriptScroll(bool followTail, int previousValue)
{
    auto *scrollBar = transcript->verticalScrollBar();
    scrollBar->setValue(followTail ? scrollBar->maximum()
                                   : qMin(previousValue, scrollBar->maximum()));
}

void AgentWidget::updateActivityCard(const QString &taskId, const QString &toolCallId,
                                    const QString &toolName, const QString &phase,
                                    const QString &operationId, const QJsonObject &facts)
{
    if (taskId.isEmpty() || taskId.size() > 128 || toolCallId.isEmpty() || toolCallId.size() > 256
        || toolName.size() > 128 || operationId.size() > 128) return;
    const QString cardKey = taskId + QLatin1Char('/') + toolCallId;
    QPointer<QFrame> frame = activityCardFrames.value(cardKey);
    QPointer<QLabel> label = activityCardLabels.value(cardKey);
    auto *activityScrollBar = activityScroll->verticalScrollBar();
    const bool followActivityTail = activityScrollBar->value() >= activityScrollBar->maximum() - 1;
    if (!frame || !label)
    {
        frame = new QFrame(activityCards);
        frame->setObjectName(QStringLiteral("agentActivityCard"));
        frame->setFrameShape(QFrame::StyledPanel);
        auto *cardLayout = new QVBoxLayout(frame);
        cardLayout->setContentsMargins(7, 4, 7, 4);
        label = new QLabel(frame);
        label->setObjectName(QStringLiteral("agentActivityCardText"));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setAccessibleName(tr("Trusted tool activity"));
        cardLayout->addWidget(label);
        const int stretchIndex = qMax(0, activityCardsLayout->count() - 1);
        activityCardsLayout->insertWidget(stretchIndex, frame);
        activityCardFrames.insert(cardKey, frame);
        activityCardLabels.insert(cardKey, label);
        activityCardOrder.append(cardKey);
        while (activityCardOrder.size() > MaxActivityCards)
        {
            const QString oldest = activityCardOrder.takeFirst();
            if (activityCardFrames.value(oldest)) activityCardFrames.value(oldest)->deleteLater();
            activityCardFrames.remove(oldest);
            activityCardLabels.remove(oldest);
        }
    }

    const QMap<QString, QString> phaseLabels{
        {QStringLiteral("reading"), tr("Reading")},
        {QStringLiteral("preparing"), tr("Preparing")},
        {QStringLiteral("awaiting_authorization"), tr("Waiting for approval")},
        {QStringLiteral("executing"), tr("Executing")},
        {QStringLiteral("completed"), tr("Host response validated")},
        {QStringLiteral("rejected"), tr("Rejected")},
        {QStringLiteral("failed"), tr("Failed")}};
    QStringList lines{tr("Task %1 · tool %2").arg(taskId, toolCallId),
                      tr("%1 — %2").arg(toolName, phaseLabels.value(phase, tr("Working")))};
    if (!operationId.isEmpty()) lines.append(tr("Operation %1").arg(operationId));
    for (const QString &factKey : {QStringLiteral("status"), QStringLiteral("code"),
                                   QStringLiteral("proposalId"), QStringLiteral("jobId")})
    {
        const QString value = facts.value(factKey).toString().left(128);
        if (!value.isEmpty()) lines.append(tr("%1: %2").arg(factKey, value));
    }
    label->setText(lines.join(QLatin1Char('\n')));
    frame->setProperty("agentTaskId", taskId);
    frame->setProperty("agentToolCallId", toolCallId);
    frame->setProperty("agentOperationId", operationId);
    frame->setProperty("agentActivityPhase", phase);
    activityScroll->show();
    if (followActivityTail) activityScrollBar->setValue(activityScrollBar->maximum());
}

void AgentWidget::onToolActivity(const QString &taskId, const QString &toolCallId,
                                 const QString &toolName, const QString &phase,
                                 const QString &operationId, const QJsonObject &facts)
{
    updateActivityCard(taskId, toolCallId, toolName, phase, operationId, facts);
}

void AgentWidget::appendStreamText(const QString &requestId, const QString &text)
{
    if (requestId.isEmpty() || text.isEmpty()) return;
    if (!liveStreamRequestId.isEmpty() && liveStreamRequestId != requestId)
        finishStreamText(liveStreamRequestId, liveStreamText, false);

    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (liveStreamRequestId.isEmpty())
    {
        QTextCursor cursor(transcript->document());
        cursor.movePosition(QTextCursor::End);
        if (!transcript->document()->isEmpty()) cursor.insertBlock();
        liveStreamStartPosition = cursor.position();
        liveStreamTitleStartPosition = cursor.position();
        QTextCharFormat titleFormat;
        titleFormat.setFontWeight(QFont::Bold);
        cursor.insertText(tr("Assistant"), titleFormat);
        liveStreamTitleEndPosition = cursor.position();
        cursor.insertBlock();
        liveStreamContentStartPosition = cursor.position();
        liveStreamEndPosition = cursor.position();
        liveStreamRequestId = requestId;
        liveStreamText.clear();
    }

    QTextCursor cursor(transcript->document());
    cursor.setPosition(liveStreamEndPosition);
    cursor.insertText(text);
    liveStreamEndPosition = cursor.position();
    liveStreamText.append(text);
    restoreTranscriptScroll(followTail, previousScroll);
}

void AgentWidget::finishStreamText(const QString &requestId, const QString &text, bool accepted)
{
    if (requestId.isEmpty() || requestId != liveStreamRequestId) return;
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (accepted)
    {
        ConversationDocument message;
        message.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                      | QTextDocument::MarkdownNoHTML);
        QTextCursor cursor(transcript->document());
        cursor.setPosition(liveStreamContentStartPosition);
        cursor.setPosition(liveStreamEndPosition, QTextCursor::KeepAnchor);
        cursor.insertFragment(QTextDocumentFragment(&message));
    }
    else if (liveStreamTitleStartPosition >= 0 && liveStreamTitleEndPosition >= liveStreamTitleStartPosition)
    {
        QTextCursor title(transcript->document());
        title.setPosition(liveStreamTitleStartPosition);
        title.setPosition(liveStreamTitleEndPosition, QTextCursor::KeepAnchor);
        title.insertText(tr("Assistant (incomplete)"));
    }
    liveStreamRequestId.clear();
    liveStreamText.clear();
    liveStreamStartPosition = -1;
    liveStreamTitleStartPosition = -1;
    liveStreamTitleEndPosition = -1;
    liveStreamContentStartPosition = -1;
    liveStreamEndPosition = -1;
    restoreTranscriptScroll(followTail, previousScroll);
}

void AgentWidget::clearStreamText(const QString &requestId)
{
    if (requestId.isEmpty() || requestId != liveStreamRequestId) return;
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (liveStreamStartPosition >= 0 && liveStreamEndPosition >= liveStreamStartPosition)
    {
        QTextCursor cursor(transcript->document());
        cursor.setPosition(liveStreamStartPosition);
        cursor.setPosition(liveStreamEndPosition, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    liveStreamRequestId.clear();
    liveStreamText.clear();
    liveStreamStartPosition = -1;
    liveStreamTitleStartPosition = -1;
    liveStreamTitleEndPosition = -1;
    liveStreamContentStartPosition = -1;
    liveStreamEndPosition = -1;
    restoreTranscriptScroll(followTail, previousScroll);
}

bool AgentWidget::isCurrentProviderRequest(const QString &requestId, quint64 generation,
                                          QNetworkReply *expectedReply) const
{
    return !requestId.isEmpty() && requestId == activeProviderRequestId
        && generation == providerRequestGeneration && expectedReply && reply == expectedReply;
}

void AgentWidget::retireProviderRequest(const QString &requestId)
{
    if (!requestId.isEmpty() && requestId != activeProviderRequestId) return;
    const QString retiringId = activeProviderRequestId;
    const QPointer<QNetworkReply> previousReply = reply;
    if (retiringId.isEmpty() && !previousReply) return;

    // Invalidate before abort: some backends emit finished synchronously from abort().
    activeProviderRequestId.clear();
    reply.clear();
    ++providerRequestGeneration;
    if (!activeProviderCaptureContext.isEmpty())
    {
        QJsonObject event = activeProviderCaptureContext;
        activeProviderCaptureContext = {};
        event.insert("kind", "provider_interruption");
        event.insert("code", "REQUEST_RETIRED");
        recordProviderCapture(event);
    }
    if (!retiringId.isEmpty() && liveStreamRequestId == retiringId)
        finishStreamText(retiringId, liveStreamText, false);
    if (previousReply) previousReply->abort();
}

void AgentWidget::openSettings()
{
    Application *application = Application::instance();
    if (!application || !application->getApplicationSettings())
    {
        state->setText(tr("Assistant settings are unavailable."));
        return;
    }
    ApplicationSettingsDialog dialog(application->getApplicationSettings(), this, QStringLiteral("Agent"));
    dialog.exec();
}

void AgentWidget::persistConversationPreferences()
{
    if (accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly) return;
    QSettings *preferences = agentPreferences();
    if (!preferences) return;
    if (model) providerSettings.setModel(model->currentText().trimmed());
    if (!isValidAgentEndpoint(providerSettings.getApiUrl()))
    {
        if (state)
        {
            state->setText(tr("Invalid endpoint"));
            state->setToolTip(invalidEndpointMessage());
        }
        return;
    }
    preferences->setValue(providerTypeKey, int(providerSettings.getType()));
    preferences->setValue(endpointKey, providerSettings.getApiUrl());
    preferences->setValue(modelKey, providerSettings.getModel());
    preferences->setValue(effortKey, selectedEffort);
    preferences->setValue(automaticChangesKey, authorize && authorize->isChecked());
    preferences->setValue(streamingResponsesKey, streamResponses && streamResponses->isChecked());
    // API keys are deliberately excluded from persistent settings.
}

QWidget *AgentWidget::createSettingsPage(QWidget *parent)
{
    QWidget *page = new QWidget(parent);
    page->setObjectName(QStringLiteral("agentSettingsPage"));
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(page);
    provider->setObjectName(QStringLiteral("agentSettingsProvider"));
    provider->addItem(tr("DeepSeek"), int(RAgentSettings::Type::DeepSeek));
    provider->addItem(tr("OpenAI-compatible Chat Completions"), int(RAgentSettings::Type::OpenAILegacy));
    auto *endpoint = new QLineEdit(page);
    endpoint->setObjectName(QStringLiteral("agentSettingsEndpoint"));
    endpoint->setAccessibleName(tr("Assistant provider HTTPS endpoint"));
    auto *apiKey = new QLineEdit(page);
    apiKey->setObjectName(QStringLiteral("agentSettingsApiKey"));
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(tr("API key (kept in memory for this session)"));
    apiKey->setAccessibleName(tr("Assistant API key"));
    auto *loadKey = new QPushButton(tr("Load key file…"), page);
    loadKey->setObjectName(QStringLiteral("agentLoadKeyFile"));
    loadKey->setAccessibleName(tr("Load an API key from a local file"));
    auto *keyRow = new QWidget(page);
    auto *keyRowLayout = new QHBoxLayout(keyRow);
    keyRowLayout->setContentsMargins(0, 0, 0, 0);
    keyRowLayout->addWidget(apiKey, 1);
    keyRowLayout->addWidget(loadKey);
    form->addRow(tr("Provider"), provider);
    form->addRow(tr("Endpoint"), endpoint);
    auto *endpointError = new QLabel(page);
    endpointError->setObjectName(QStringLiteral("agentEndpointValidation"));
    endpointError->setWordWrap(true);
    form->addRow(QString(), endpointError);
    form->addRow(tr("API key"), keyRow);
    layout->addLayout(form);
    auto *note = new QLabel(tr("The API key stays in memory for this session and is never written to application settings."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch(1);

    QPointer<AgentWidget> agent = activeAgentWidget();
    RAgentSettings settings = agent ? agent->getProviderSettings() : RAgentSettings(RAgentSettings::Type::DeepSeek);
    QSettings *preferences = agentPreferences();
    if (!agent && preferences)
    {
        const auto type = static_cast<RAgentSettings::Type>(preferences->value(providerTypeKey, int(RAgentSettings::Type::DeepSeek)).toInt());
        if (type == RAgentSettings::Type::DeepSeek || type == RAgentSettings::Type::OpenAILegacy)
            settings.setType(type);
        const RAgentSettings defaults(settings.getType());
        const QString savedEndpoint = preferences->value(endpointKey, defaults.getApiUrl()).toString();
        if (isValidAgentEndpoint(savedEndpoint)) settings.setApiUrl(savedEndpoint.trimmed());
        else
        {
            settings.setApiUrl(defaults.getApiUrl());
            preferences->setValue(endpointKey, defaults.getApiUrl());
        }
        settings.setModel(preferences->value(modelKey, defaults.getModel()).toString());
    }
    const int providerIndex = provider->findData(int(settings.getType()));
    provider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    endpoint->setText(settings.getApiUrl());
    endpoint->setProperty("acceptedAgentEndpoint", endpoint->text());
    apiKey->setText(settings.getApiKey());

    auto applySettings = [agent, preferences, endpoint, endpointError](const RAgentSettings &updated) {
        const QString endpointValue = updated.getApiUrl().trimmed();
        if (!isValidAgentEndpoint(endpointValue))
        {
            endpointError->setText(invalidEndpointMessage());
            endpoint->setText(endpoint->property("acceptedAgentEndpoint").toString());
            return;
        }
        if (agent) agent->setProviderSettings(updated);
        if (preferences)
        {
            preferences->setValue(providerTypeKey, int(updated.getType()));
            preferences->setValue(endpointKey, endpointValue);
            preferences->setValue(modelKey, updated.getModel());
        }
        endpoint->setProperty("acceptedAgentEndpoint", endpointValue);
        endpoint->setText(endpointValue);
        endpointError->clear();
    };
    connect(provider, &QComboBox::currentIndexChanged, page, [provider, endpoint, apiKey, applySettings](int index) {
        if (index < 0) return;
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->itemData(index).toInt()));
        apiKey->clear();
        endpoint->setText(updated.getApiUrl());
        applySettings(updated);
    });
    connect(endpoint, &QLineEdit::editingFinished, page, [agent, endpoint, apiKey, provider, endpointError, applySettings] {
        const QString endpointValue = endpoint->text().trimmed();
        if (!isValidAgentEndpoint(endpointValue))
        {
            endpointError->setText(invalidEndpointMessage());
            endpoint->setText(endpoint->property("acceptedAgentEndpoint").toString());
            return;
        }
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->currentData().toInt()));
        updated.setApiUrl(endpointValue);
        updated.setModel(agent && agent->model ? agent->model->currentText().trimmed() : QString());
        updated.setApiKey(apiKey->text());
        applySettings(updated);
    });
    connect(apiKey, &QLineEdit::textChanged, page, [agent, endpoint, apiKey, provider, applySettings](const QString &value) {
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->currentData().toInt()));
        updated.setApiUrl(endpoint->text().trimmed());
        updated.setModel(agent && agent->model ? agent->model->currentText().trimmed() : QString());
        updated.setApiKey(value);
        applySettings(updated);
    });
    connect(loadKey, &QPushButton::clicked, page, [page, loadKey, apiKey, provider] {
        const QString path = QFileDialog::getOpenFileName(page, tr("Select local API key file"), QString(),
                                                          tr("Key files (*.key *.txt *.env*);;All files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        {
            loadKey->setToolTip(tr("Cannot read this key file."));
            return;
        }
        const QString contents = QString::fromUtf8(file.readAll());
        const QString variable = provider->currentData().toInt() == int(RAgentSettings::Type::DeepSeek)
            ? QStringLiteral("DEEPSEEK_API_KEY=") : QStringLiteral("OPENAI_API_KEY=");
        QString value;
        for (QString line : contents.split(QLatin1Char('\n')))
        {
            line = line.trimmed();
            if (!line.startsWith(variable)) continue;
            value = line.mid(variable.size()).trimmed();
            if (value.size() >= 2 && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                                      || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
                value = value.mid(1, value.size() - 2);
            break;
        }
        const QString rawLine = contents.trimmed();
        if (value.isEmpty() && !rawLine.contains(QLatin1Char('\n')) && !rawLine.contains(QLatin1Char('\r'))
            && !rawLine.contains(QLatin1Char('=')))
            value = rawLine;
        if (value.isEmpty() || value.size() > 4096
            || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r')))
        {
            loadKey->setToolTip(tr("No single-line key for the selected provider was found."));
            return;
        }
        apiKey->setText(value);
        loadKey->setToolTip(tr("API key loaded for this session."));
    });
    return page;
}

void AgentWidget::updateAvailability()
{
    const bool busy = runner.isActive();
    newChat->setEnabled(!busy);
    send->setText(runner.isAwaitingInput() ? tr("Answer") : (busy ? tr("Send now") : tr("Send")));
    send->setEnabled(!goal->toPlainText().trimmed().isEmpty());
    stop->setEnabled(busy);
    stop->setVisible(busy);
    goal->setEnabled(true);
    model->setEnabled(true);
    effort->setEnabled(providerSettings.getType() == RAgentSettings::Type::DeepSeek);
    streamResponses->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && providerSupportsStreaming(providerSettings.getType()));
    streamResponses->setToolTip(providerSupportsStreaming(providerSettings.getType())
        ? tr("When enabled, supported Chat Completions providers stream text. Complete JSON responses remain available when streaming is off.")
        : tr("This provider does not support streaming Chat Completions."));
    authorize->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite);
    readOnlyQuestion->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && !goal->toPlainText().trimmed().isEmpty());
    conversationChoice->setEnabled(!busy);
    branchConversation->setEnabled(!busy && !currentConversationId.isEmpty() && branchMessageChoice->count() > 0);
    branchMessageChoice->setEnabled(!busy);
    historyMenu->setEnabled(!currentConversationId.isEmpty());
    historyPrevious->setEnabled(!busy && historyOffset > 0);
    historyNext->setEnabled(!busy && historyOffset + 50 < conversationMessageCount);

    const bool pendingProposal = busy && !runner.pendingProposal().isEmpty() && !presentedProposalId.isEmpty();
    approvalCard->setVisible(pendingProposal);
    apply->setEnabled(pendingProposal && trustedExperimentReviewComplete);
    apply->setToolTip(trustedExperimentReviewComplete ? QString()
        : tr("The trusted experiment preview is too large or incomplete for safe review."));
    decline->setEnabled(pendingProposal);
    if (!pendingProposal)
    {
        presentedProposalId.clear();
        trustedExperimentReviewComplete = true;
        preview->setMinimumHeight(0);
        preview->setMaximumHeight(110);
        preview->clear();
    }
    const bool pendingQuestion = busy && runner.isAwaitingInput() && !runner.pendingQuestionId().isEmpty();
    questionCard->setVisible(pendingQuestion);
    if (!pendingQuestion) presentedQuestionId.clear();
    if (pendingQuestion && runner.pendingQuestionId() != presentedQuestionId)
    {
        questionCard->setVisible(false);
        send->setText(busy ? tr("Send now") : tr("Send"));
    }
}

QJsonObject AgentWidget::hostRequest(const QString &operation, const QJsonObject &parameters) const
{
    if (!commandHost) return {};
    const QJsonObject request{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                                {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("operation"), operation},
                                {QStringLiteral("parameters"), parameters}};
    return accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? commandHost->handleReadOnly(request) : commandHost->handle(request);
}

bool AgentWidget::queryFreshContext()
{
    const auto response = hostRequest(QStringLiteral("context.query"));
    if (response.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
    {
        contextRefreshPending = true;
        contextSummaryBody = tr("Current document context could not be queried.");
        eventContextSummary->setText(contextSummaryBody);
        return false;
    }
    const auto result = response.value(QStringLiteral("result")).toObject();
    const auto documents = result.value(QStringLiteral("documents")).toArray();
    const auto selection = result.value(QStringLiteral("selection")).toObject();
    QStringList lines;
    bool allDocumentsLive = true;
    auto appendDocument = [this, &lines, &allDocumentsLive](const QJsonObject &document) {
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        if (documentId.isEmpty()) return;
        allDocumentsLive &= document.value(QStringLiteral("stateAvailability")).toString() == QStringLiteral("live");
        const QString revision = QString::number(quint64(document.value(QStringLiteral("documentRevision")).toDouble()));
        QString line = tr("Document %1 · revision %2 · state %3")
            .arg(documentId, revision, document.value(QStringLiteral("stateAvailability")).toString());
        const QJsonObject accepted = document.value(QStringLiteral("acceptedState")).toObject();
        const QJsonObject modal = accepted.value(QStringLiteral("modal")).toObject();
        QStringList values;
        const QMap<QString, QString> labels{{QStringLiteral("iterations"), tr("iterations")},
            {QStringLiteral("modes"), tr("modes")}, {QStringLiteral("method"), tr("method")},
            {QStringLiteral("tolerance"), tr("tolerance")}, {QStringLiteral("unit"), tr("unit")}};
        for (auto it = labels.cbegin(); it != labels.cend(); ++it)
        {
            const auto value = modal.value(it.key());
            if (value.isUndefined() || value.isNull()) continue;
            values.append(QStringLiteral("%1 %2").arg(it.value(), value.toVariant().toString()));
        }
        if (!values.isEmpty()) line += tr(" · modal: %1").arg(values.join(QStringLiteral(", ")));
        lines.append(line);
    };
    constexpr qsizetype maxContextDocuments = 8;
    const qsizetype count = qMin(documents.size(), maxContextDocuments);
    for (qsizetype i = 0; i < count; ++i) appendDocument(documents.at(i).toObject());
    if (documents.size() > maxContextDocuments)
        lines.append(tr("%1 additional selected documents are omitted from this summary.").arg(documents.size() - maxContextDocuments));
    const bool live = selection.value(QStringLiteral("availability")).toString() == QStringLiteral("live")
        && !result.value(QStringLiteral("editingBlocked")).toBool()
        && (documents.isEmpty() || allDocumentsLive);
    const QString details = lines.isEmpty()
        ? tr("no selected open document is available") : lines.join(QStringLiteral("\n"));
    contextSummaryBody = live
        ? tr("Live selected-document context: %1").arg(details)
        : tr("Cached or unavailable context; document state may be stale: %1").arg(details);
    contextRefreshPending = !live;
    eventContextSummary->setText(contextSummaryBody);
    return true;
}

void AgentWidget::markContextStale()
{
    contextRefreshPending = true;
    if (contextSummaryBody.isEmpty()) contextSummaryBody = tr("No document context snapshot is available.");
    eventContextSummary->setText(tr("Prior context may be stale. %1").arg(contextSummaryBody));
}

void AgentWidget::appendHostEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const QString documentId = event.value(QStringLiteral("documentId")).toString();

    QString readableType;
    if (type == QStringLiteral("document.opened")) readableType = tr("Document opened");
    else if (type == QStringLiteral("document.closed")) readableType = tr("Document closed");
    else if (type == QStringLiteral("document.changed")) readableType = tr("Document changed");
    else if (type == QStringLiteral("document.revision_changed")) readableType = tr("Document revision changed");
    else if (type == QStringLiteral("selection.changed")) readableType = tr("Selection changed");
    else readableType = type.isEmpty() ? tr("Host event") : tr("Host event (%1)").arg(type);

    QString revision;
    const auto before = event.value(QStringLiteral("beforeRevision"));
    const auto after = event.value(QStringLiteral("afterRevision"));
    if (!before.isUndefined() || !after.isUndefined())
        revision = tr("%1 → %2").arg(before.isNull() || before.isUndefined() ? tr("new") : before.toVariant().toString(),
                                      after.isUndefined() ? tr("unknown") : after.toVariant().toString());
    else if (event.contains(QStringLiteral("documentRevision")))
        revision = event.value(QStringLiteral("documentRevision")).toVariant().toString();
    else
        revision = tr("unknown");
    const QString operationId = event.value(QStringLiteral("operationId")).toString();
    eventHistory->appendPlainText(tr("#%1 · %2 · document %3 · revision %4 · operation %5")
        .arg(QString::number(quint64(event.value(QStringLiteral("eventSeq")).toDouble())), readableType,
             documentId.isEmpty() ? tr("none") : documentId, revision,
             operationId.isEmpty() ? tr("none") : operationId));
}

void AgentWidget::refreshHostEvents()
{
    if (isVisible()) bindConversationStore();
    auto *eventBody = findChild<QWidget *>(QStringLiteral("agentEventHistoryBody"));
    if (!commandHost || QThread::currentThread() != thread() || !isVisible()
        || !eventBody || !eventBody->isVisible()) return;
    const auto capabilities = hostRequest(QStringLiteral("capabilities.query"));
    if (capabilities.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
    {
        markContextStale();
        eventStatus->setText(tr("Host event history is unavailable."));
        return;
    }
    const QString currentEpoch = capabilities.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("sessionEpoch")).toString();
    if (currentEpoch.isEmpty())
    {
        markContextStale();
        eventStatus->setText(tr("Host event history is unavailable."));
        return;
    }
    if (eventSessionEpoch != currentEpoch)
    {
        const bool reset = !eventSessionEpoch.isEmpty();
        eventSessionEpoch = currentEpoch;
        lastEventSeq = 0;
        eventHistory->clear();
        eventHistory->appendPlainText(reset ? tr("Host session changed; loading fresh document context.")
                                            : tr("Connected to the current host session."));
        if (!queryFreshContext())
            eventStatus->setText(tr("Host session changed; fresh context could not be queried yet."));
    }

    // The host retains at most 256 records. Three pages cover that entire ring;
    // a larger gap is represented by a fresh context snapshot instead.
    bool contextChanged = false;
    for (int page = 0; page < 3; ++page)
    {
        const auto response = hostRequest(QStringLiteral("events.read"),
            {{QStringLiteral("afterEventSeq"), double(lastEventSeq)}, {QStringLiteral("limit"), 100}});
        if (response.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
        {
            markContextStale();
            eventStatus->setText(tr("Host event history could not be read; it will be retried."));
            return;
        }
        const auto result = response.value(QStringLiteral("result")).toObject();
        if (result.value(QStringLiteral("resyncRequired")).toBool())
        {
            eventHistory->appendPlainText(tr("Event history gap; older in-memory events have expired."));
            if (!queryFreshContext())
            {
                eventStatus->setText(tr("Event history gap; context refresh failed and will be retried."));
                return;
            }
            lastEventSeq = quint64(result.value(QStringLiteral("latestEventSeq")).toDouble());
            eventStatus->setText(tr("Event cursor advanced through %1; context freshness is shown above.").arg(QString::number(lastEventSeq)));
            return;
        }
        const auto events = result.value(QStringLiteral("events")).toArray();
        bool epochChanged = false;
        for (const auto &value : events)
        {
            const auto event = value.toObject();
            if (event.value(QStringLiteral("sessionEpoch")).toString() != eventSessionEpoch)
            {
                epochChanged = true;
                break;
            }
            appendHostEvent(event);
            lastEventSeq = quint64(event.value(QStringLiteral("eventSeq")).toDouble());
            const QString type = event.value(QStringLiteral("type")).toString();
            contextChanged |= type.startsWith(QStringLiteral("document."))
                || type == QStringLiteral("selection.changed");
        }
        if (epochChanged)
        {
            eventHistory->appendPlainText(tr("Event stream changed; loading fresh document context."));
            if (!queryFreshContext())
            {
                eventStatus->setText(tr("Event stream changed; context refresh failed and will be retried."));
                return;
            }
            lastEventSeq = quint64(result.value(QStringLiteral("latestEventSeq")).toDouble());
            eventStatus->setText(tr("Event cursor advanced through %1; context freshness is shown above.").arg(QString::number(lastEventSeq)));
            return;
        }
        if (!result.value(QStringLiteral("hasMore")).toBool()) break;
    }
    if (contextChanged || contextRefreshPending) queryFreshContext();
    eventStatus->setText(tr("Up to date through event %1.").arg(QString::number(lastEventSeq)));
}

void AgentWidget::requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools)
{
    retireProviderRequest();
    const quint64 generation = ++providerRequestGeneration;
    activeProviderRequestId = requestId;
    const RAgentSettings settings(requestSettingsSnapshot);
    const QString conversationId = currentConversationId;
    const QString configuredProvider = RAgentSettings::typeToString(settings.getType());
    const QString configuredModel = settings.getModel().trimmed();
    const QString providerKeyValue = settings.getApiKey().trimmed();
    const QString endpointValue = settings.getApiUrl().trimmed();
    const QUrl url(endpointValue);
    if (providerCaptureRecorder && evaluationSlotBinding.enabled)
    {
        activeProviderCaptureContext = {{"requestId", requestId}, {"taskId", runner.taskId()},
            {"conversationId", conversationId}, {"provider", configuredProvider},
            {"evaluationArm", AgentTaskRunner::evaluationArmId(evaluationArm)}};
        lastProviderCaptureContext = activeProviderCaptureContext;
    }
    const auto failBeforeRequest = [this, &requestId](const QString &message) {
        retireProviderRequest(requestId);
        runner.providerFailed(requestId, message);
    };
    if (evaluationSlotBinding.enabled && !providerCaptureRecorder)
    { failBeforeRequest(tr("The v4 evaluation slot has no active capture recorder.")); return; }
    if (!isValidAgentEndpoint(endpointValue))
    { failBeforeRequest(invalidEndpointMessage()); return; }
    if (providerKeyValue.isEmpty())
    { failBeforeRequest(tr("Add an API key in assistant settings.")); return; }
    if (!network)
    { failBeforeRequest(tr("Assistant networking is unavailable.")); return; }
    const bool deepSeek = settings.getType() == RAgentSettings::Type::DeepSeek;
    const bool streamEnabled = requestStreamingSnapshot && providerSupportsStreaming(settings.getType());
    if (providerCaptureRecorder && streamEnabled)
    { failBeforeRequest(tr("Evaluation capture requires the preregistered non-streaming request mode.")); return; }
    const bool thinkingDisabled = deepSeek && requestEffortSnapshot == QStringLiteral("off");
    const QString deepSeekEffort = deepSeek && !thinkingDisabled ? requestEffortSnapshot : QString();
    const int maxTokens = deepSeek && !thinkingDisabled
        ? qMax(8192, settings.getMaxTokens()) : settings.getMaxTokens();
    QByteArray body; QString error;
    if (!RAgent::buildToolRequest(settings.getModel().trimmed(), maxTokens, 1.0, messages, tools,
                                 thinkingDisabled, &body, &error, deepSeekEffort, streamEnabled))
    { failBeforeRequest(error); return; }
    if (providerCaptureRecorder)
    {
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        providerCaptureToolNames.clear();
        for (const QJsonValue &tool : tools)
            providerCaptureToolNames.insert(tool.toObject().value("function").toObject().value("name").toString());
        providerCaptureToolNames.remove(QString());
        QJsonObject sentSettings;
        for (const QString &field : {QStringLiteral("model"), QStringLiteral("reasoning_effort"),
             QStringLiteral("temperature"), QStringLiteral("max_tokens"), QStringLiteral("stream"),
             QStringLiteral("thinking")})
            if (payload.contains(field)) sentSettings.insert(field, payload.value(field));
        activeProviderCaptureContext = {{"requestId", requestId}, {"taskId", runner.taskId()},
            {"conversationId", conversationId}, {"provider", configuredProvider},
            {"evaluationArm", AgentTaskRunner::evaluationArmId(evaluationArm)}};
        lastProviderCaptureContext = activeProviderCaptureContext;
        if (evaluationSlotBinding.enabled && !evaluationSlotBindingRecorded)
        {
            const QString submittedPromptSha256 = sha256Hex(initialEvaluationPrompt);
            const bool promptDigestMatches = !initialEvaluationPrompt.isEmpty()
                && submittedPromptSha256 == evaluationSlotBinding.promptSha256;
            const QJsonArray outgoingMessages = payload.value(QStringLiteral("messages")).toArray();
            const bool exactPromptIsInRequest = requestContainsExactUserPrompt(outgoingMessages,
                                                                               initialEvaluationPrompt);
            QJsonObject bindingEvent = activeProviderCaptureContext;
            bindingEvent.insert(QStringLiteral("runId"), evaluationSlotBinding.runId);
            bindingEvent.insert(QStringLiteral("holdoutId"), evaluationSlotBinding.holdoutId);
            bindingEvent.insert(QStringLiteral("slotId"), evaluationSlotBinding.slotId);
            bindingEvent.insert(QStringLiteral("caseId"), evaluationSlotBinding.caseId);
            bindingEvent.insert(QStringLiteral("repeat"), evaluationSlotBinding.repeat);
            bindingEvent.insert(QStringLiteral("manifestCommitmentSha256"),
                                evaluationSlotBinding.manifestCommitmentSha256);
            bindingEvent.insert(QStringLiteral("promptSha256"), evaluationSlotBinding.promptSha256);
            bindingEvent.insert(QStringLiteral("requestId"), requestId);
            if (!promptDigestMatches || !exactPromptIsInRequest)
            {
                bindingEvent.insert(QStringLiteral("kind"), QStringLiteral("slot_binding_rejected"));
                bindingEvent.insert(QStringLiteral("reasonCode"),
                    !promptDigestMatches ? QStringLiteral("INITIAL_PROMPT_DIGEST_MISMATCH")
                                         : QStringLiteral("INITIAL_PROMPT_NOT_IN_OUTGOING_REQUEST"));
                if (!recordProviderCapture(bindingEvent))
                { failBeforeRequest(tr("The v4 prompt binding failure could not be captured.")); return; }
                failBeforeRequest(tr("The v4 case prompt does not match the prepared slot; no Provider request was sent."));
                return;
            }
            bindingEvent.insert(QStringLiteral("kind"), QStringLiteral("slot_binding"));
            if (!recordProviderCapture(bindingEvent))
            { failBeforeRequest(tr("The v4 slot binding could not be durably recorded.")); return; }
            evaluationSlotBindingRecorded = true;
        }
        QJsonObject intent = activeProviderCaptureContext;
        intent.insert("kind", "dispatch_intent"); intent.insert("settings", sentSettings);
        intent.insert("requestSha256", QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex()));
        intent.insert("destinationScheme", url.scheme().toLower());
        intent.insert("destinationHost", url.host().toLower());
        intent.insert("destinationPort", url.port(url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80));
        intent.insert("endpointPathSha256", QString::fromLatin1(QCryptographicHash::hash(
            url.path(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256).toHex()));
        if (!recordProviderCapture(intent))
        { failBeforeRequest(tr("Evaluation capture could not durably record the request intent.")); return; }
        if (requestId != activeProviderRequestId || generation != providerRequestGeneration || !runner.isActive()) return;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", streamEnabled ? "text/event-stream" : "application/json");
    request.setRawHeader("Authorization", "Bearer " + providerKeyValue.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *pending = network->post(request, body);
    if (!pending)
    { failBeforeRequest(tr("The provider request could not be started.")); return; }
    reply = pending;
    pending->setReadBufferSize(64 * 1024);
    auto responseBytes = QSharedPointer<QByteArray>::create();
    auto streamParser = QSharedPointer<RAgentToolStream>::create();
    auto tooLarge = QSharedPointer<bool>::create(false);
    auto streamFailure = QSharedPointer<QString>::create();
    const auto consumeAvailable = [this, pending, requestId, generation, streamEnabled,
                                   responseBytes, streamParser, tooLarge, streamFailure] {
        const QByteArray chunk = pending->readAll();
        if (chunk.isEmpty()) return;
        if (!isCurrentProviderRequest(requestId, generation, pending)) return;
        const int statusCode = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) return; // Provider error bodies are never parsed or displayed.
        if (streamEnabled)
        {
            if (!streamFailure->isEmpty()) return;
            const bool parsed = streamParser->feed(chunk);
            appendStreamText(requestId, streamParser->takeTextDelta());
            if (!parsed)
            {
                *streamFailure = streamParser->errorMessage();
                pending->abort();
            }
            return;
        }
        if (*tooLarge) return;
        if (chunk.size() > MaxProviderResponseBytes - responseBytes->size())
        {
            *tooLarge = true;
            responseBytes->clear();
            pending->abort();
            return;
        }
        responseBytes->append(chunk);
    };
    connect(pending, &QNetworkReply::readyRead, this, [consumeAvailable] {
        consumeAvailable();
    });
    connect(pending, &QNetworkReply::finished, this,
            [this, pending, requestId, generation, streamEnabled, consumeAvailable,
             responseBytes, streamParser, tooLarge, streamFailure, conversationId,
             configuredProvider, configuredModel] {
        consumeAvailable();
        const bool wasCurrent = isCurrentProviderRequest(requestId, generation, pending);
        const auto statusCode = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = pending->error();
        if (reply == pending) reply.clear();
        pending->deleteLater();
        if (!wasCurrent) return;

        // Invalidate this request before delivering a turn; runner signals may start a new one inline.
        activeProviderRequestId.clear();
        ++providerRequestGeneration;
        const QJsonObject captureContext = activeProviderCaptureContext;
        activeProviderCaptureContext = {};
        if (networkError != QNetworkReply::NoError || statusCode != 200 || *tooLarge
            || !streamFailure->isEmpty())
        {
            if (providerCaptureRecorder)
            {
                QJsonObject event = captureContext; event.insert("kind", "provider_interruption");
                event.insert("code", "TRANSPORT_FAILED"); event.insert("httpStatus", statusCode);
                event.insert("networkCode", int(networkError)); recordProviderCapture(event);
            }
            // Provider error bodies may echo request data; never put them in logs or the transcript.
            if (streamEnabled && !streamFailure->isEmpty())
                state->setToolTip(*streamFailure);
            if (liveStreamRequestId == requestId)
                finishStreamText(requestId, liveStreamText, false);
            const QString message = !streamFailure->isEmpty()
                ? *streamFailure
                : (*tooLarge ? tr("Provider response exceeds the 1 MiB limit.")
                             : tr("Provider request failed (HTTP %1, network code %2). Check credentials, connection and quota.")
                                   .arg(statusCode).arg(int(networkError)));
            runner.providerFailed(requestId, message);
            return;
        }

        QJsonObject turn;
        QString providerModel;
        QString parseError;
        if (streamEnabled)
        {
            turn = streamParser->finish();
            if (turn.isEmpty())
            {
                if (liveStreamRequestId == requestId)
                    finishStreamText(requestId, liveStreamText, false);
                runner.providerFailed(requestId, streamParser->errorMessage().isEmpty()
                    ? tr("The provider stream ended before a complete response was received.")
                    : streamParser->errorMessage());
                return;
            }
            providerModel = streamParser->reportedModelId();
        }
        else if (!RAgent::parseToolResponse(*responseBytes, &turn, &parseError, &providerModel))
        {
            if (providerCaptureRecorder)
            {
                QJsonObject event = captureContext; event.insert("kind", "provider_interruption");
                event.insert("code", "PROVIDER_RESPONSE_INVALID");
                event.insert("responseSha256", QString::fromLatin1(QCryptographicHash::hash(*responseBytes, QCryptographicHash::Sha256).toHex()));
                recordProviderCapture(event);
            }
            runner.providerFailed(requestId, parseError); return;
        }

        if (providerCaptureRecorder)
        {
            const quint64 responseDeliveryGeneration = providerRequestGeneration;
            const QString responseTaskId = captureContext.value("taskId").toString();
            const QJsonObject raw = QJsonDocument::fromJson(*responseBytes).object();
            const QJsonObject choice = raw.value("choices").toArray().first().toObject();
            const QJsonObject assistant = turn.value("assistantMessage").toObject();
            QJsonObject event = captureContext; event.insert("kind", "provider_response");
            event.insert("responseSha256", QString::fromLatin1(QCryptographicHash::hash(*responseBytes, QCryptographicHash::Sha256).toHex()));
            const QString providerResponseId = boundedProviderIdentifier(raw.value("id"), 128);
            const QString returnedModel = boundedProviderIdentifier(raw.value("model"), 128);
            const QString finishReason = boundedProviderIdentifier(choice.value("finish_reason"), 64);
            event.insert("providerResponseIdStatus", raw.value("id").isUndefined() ? "missing"
                : providerResponseId.isEmpty() ? "invalid" : "present");
            event.insert("returnedModelStatus", raw.value("model").isUndefined() ? "missing"
                : returnedModel.isEmpty() ? "invalid" : returnedModel == configuredModel ? "matches_configured" : "different");
            if (!returnedModel.isEmpty())
                event.insert("returnedModelSha256", QString::fromLatin1(QCryptographicHash::hash(
                    returnedModel.toUtf8(), QCryptographicHash::Sha256).toHex()));
            if (raw.contains("usage")) event.insert("usage", boundedProviderUsage(raw.value("usage")));
            const QSet<QString> finishReasons{"stop", "tool_calls", "length", "content_filter", "function_call"};
            event.insert("finishReason", finishReasons.contains(finishReason) ? finishReason : QStringLiteral("unknown"));
            QJsonArray capturedCalls;
            providerCaptureToolIds.clear();
            providerCaptureToolCallNames.clear();
            for (const QJsonValue &value : assistant.value("tool_calls").toArray())
            {
                const auto call = value.toObject(); const auto function = call.value("function").toObject();
                const QString arguments = function.value("arguments").toString();
                QJsonObject capturedCall;
                const QString toolCallId = call.value("id").toString();
                const QString localId = QStringLiteral("tool-%1").arg(capturedCalls.size() + 1);
                providerCaptureToolIds.insert(toolCallId, localId);
                capturedCall.insert("toolCallId", localId);
                const QString toolName = function.value("name").toString();
                const QString capturedToolName = providerCaptureToolNames.contains(toolName)
                    ? toolName : QStringLiteral("unregistered");
                providerCaptureToolCallNames.insert(toolCallId, capturedToolName);
                capturedCall.insert("name", capturedToolName);
                capturedCall.insert("argumentsSha256", QString::fromLatin1(QCryptographicHash::hash(
                    arguments.toUtf8(), QCryptographicHash::Sha256).toHex()));
                capturedCalls.append(capturedCall);
            }
            event.insert("toolCalls", capturedCalls);
            event.insert("contentSha256", QString::fromLatin1(QCryptographicHash::hash(
                turn.value("text").toString().toUtf8(), QCryptographicHash::Sha256).toHex()));
            if (!recordProviderCapture(event))
            { runner.providerFailed(requestId, tr("Evaluation capture failed before delivering the Provider response.")); return; }
            if (!runner.isActive() || runner.taskId() != responseTaskId
                || providerRequestGeneration != responseDeliveryGeneration) return;
        }

        if (streamEnabled && liveStreamRequestId == requestId)
        {
            finalizingStreamMessage = true;
            finalizingStreamText = turn.value(QStringLiteral("text")).toString();
        }
        const QString taskId = runner.taskId();
        const bool taskWasActive = runner.isActive() && boundedTelemetryValue(taskId, 128);
        runner.receiveTurn(requestId, turn);
        if (finalizingStreamMessage && liveStreamRequestId == requestId)
        {
            finalizingStreamMessage = false;
            finalizingStreamText.clear();
            clearStreamText(requestId);
        }
        emit providerUsage(turn.value("usage").toObject());
        if (taskWasActive)
        {
            QJsonObject event{{QStringLiteral("schema"), QStringLiteral("range.provider-turn-telemetry/v2")},
                {QStringLiteral("requestId"), requestId}, {QStringLiteral("taskId"), taskId},
                {QStringLiteral("conversationId"), conversationId},
                {QStringLiteral("provider"), configuredProvider},
                {QStringLiteral("configuredModelSha256"), QString::fromLatin1(
                    QCryptographicHash::hash(configuredModel.toUtf8(), QCryptographicHash::Sha256).toHex())},
                {QStringLiteral("providerModelStatus"), providerModel.isEmpty()
                    ? QStringLiteral("unknown") : QStringLiteral("reported")},
                {QStringLiteral("usage"), turn.value(QStringLiteral("usage")).toObject()}};
            if (!providerModel.isEmpty())
                event.insert(QStringLiteral("providerModelSha256"), QString::fromLatin1(
                    QCryptographicHash::hash(providerModel.toUtf8(), QCryptographicHash::Sha256).toHex()));
            if (boundedTelemetryValue(requestId, 128) && boundedTelemetryValue(conversationId, 128)
                && boundedTelemetryValue(configuredProvider, 64) && boundedTelemetryValue(configuredModel, 256)
                && (providerModel.isEmpty() || boundedTelemetryValue(providerModel, 128))
                && QJsonDocument(event).toJson(QJsonDocument::Compact).size() <= MaxProviderTelemetryEventBytes)
                emit providerTurnTelemetry(event);
        }
    });
}
