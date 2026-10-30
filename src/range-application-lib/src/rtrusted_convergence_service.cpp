#include <rtrusted_convergence_service.h>

#include <rtrusted_mechanics_service.h>

#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
using Service = RTrustedConvergenceService;

QJsonObject quantity(double value, const QString &unit)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("unit"), unit}};
}

QJsonArray vector(double x, double y, double z, const QString &unit)
{
    return {quantity(x, unit), quantity(y, unit), quantity(z, unit)};
}

QJsonObject genericTransientProblemSpec()
{
    const QJsonObject fixedEnd{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}};
    const QJsonObject allNodes{{QStringLiteral("kind"), QStringLiteral("allNodes")}};
    const QJsonObject freeEnd{{QStringLiteral("kind"), QStringLiteral("freeEnd")}};
    return {{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("modelType"), QStringLiteral("axialRod")},
        {QStringLiteral("name"), QStringLiteral("generic estimator rod")},
        {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), quantity(1.0, QStringLiteral("m"))},
            {QStringLiteral("segments"), 4}}},
        {QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
            {QStringLiteral("source"), QStringLiteral("estimator fixture")},
            {QStringLiteral("youngModulus"), quantity(210.0e9, QStringLiteral("Pa"))},
            {QStringLiteral("poissonRatio"), 0.3},
            {QStringLiteral("density"), quantity(7850.0, QStringLiteral("kg/m3"))}}},
        {QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), quantity(1.0e-4, QStringLiteral("m2"))}}},
        {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("linearTransient")},
            {QStringLiteral("timeStep"), quantity(0.01, QStringLiteral("s"))},
            {QStringLiteral("steps"), 1},
            {QStringLiteral("damping"), QJsonObject{
                {QStringLiteral("rayleighAlpha"), quantity(0.0, QStringLiteral("1/s"))},
                {QStringLiteral("rayleighBeta"), quantity(0.0, QStringLiteral("s"))}}}}},
        {QStringLiteral("supports"), QJsonArray{
            QJsonObject{{QStringLiteral("selector"), fixedEnd},
                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
            QJsonObject{{QStringLiteral("selector"), allNodes},
                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
        {QStringLiteral("loads"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
            {QStringLiteral("selector"), freeEnd},
            {QStringLiteral("components"), vector(1000.0, 0.0, 0.0, QStringLiteral("N"))}}}},
        {QStringLiteral("initialConditions"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("displacement")},
            {QStringLiteral("selector"), allNodes},
            {QStringLiteral("components"), vector(0.0, 0.0, 0.0, QStringLiteral("m"))}}}},
        {QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), freeEnd},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("ux")}}}},
        {QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}}}};
}

Service::Outcome success(const QJsonObject &data = {})
{
    return {true, {}, {}, data};
}

Service::Outcome failure(const QString &code, const QString &message, const QJsonObject &data = {})
{
    return {false, code, message, data};
}

bool hasOnlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
    for (const QString &key : object.keys())
        if (!allowed.contains(key)) return false;
    return true;
}

bool nearlyEqual(double a, double b, double relative = 1.0e-12, double absolute = 1.0e-14)
{
    return std::abs(a - b) <= std::max(absolute, relative * std::max(std::abs(a), std::abs(b)));
}

bool timeEqual(double a, double b)
{
    return std::abs(a - b) <= std::max(1.0e-12, 1.0e-10 * std::max(std::abs(a), std::abs(b)));
}

QJsonObject withoutDerivedDuration(const QJsonObject &spec)
{
    QJsonObject result = spec;
    QJsonObject analysis = result.value(QStringLiteral("analysis")).toObject();
    analysis.remove(QStringLiteral("duration"));
    result.insert(QStringLiteral("analysis"), analysis);
    return result;
}

bool supportedAnalysis(const QString &kind)
{
    return kind == QLatin1String("static") || kind == QLatin1String("modal")
        || kind == QLatin1String("linearTransient");
}

qsizetype estimateMaximumStaticConvergenceMeasurementBytes(const QJsonObject &normalized)
{
    const QJsonValue maximumNumber(std::numeric_limits<double>::max());
    const QJsonArray maximumCoordinate{maximumNumber, maximumNumber, maximumNumber};
    QJsonArray displacements;
    int displacementCount = 0;
    for (const QJsonValue &value : normalized.value(QStringLiteral("observables")).toArray()) {
        const QJsonObject observable = value.toObject();
        if (observable.value(QStringLiteral("quantity")).toString() != QLatin1String("displacement"))
            continue;
        ++displacementCount;
        displacements.append(QJsonObject{
            {QStringLiteral("selector"), observable.value(QStringLiteral("selector"))},
            {QStringLiteral("component"), observable.value(QStringLiteral("component"))},
            {QStringLiteral("coordinateM"), maximumCoordinate},
            {QStringLiteral("values"), QJsonArray{maximumNumber}}});
    }
    const QJsonObject maximumMeasurement{
        {QStringLiteral("analysisKind"), QStringLiteral("static")},
        {QStringLiteral("expectedDisplacementCount"), displacementCount},
        {QStringLiteral("displacements"), displacements},
        {QStringLiteral("strainEnergyJ"), maximumNumber}};
    return RTrustedRecordSerialization::compact(maximumMeasurement).size();
}

qint64 dofsAtSegments(const QJsonObject &normalized, qint64 segments)
{
    if (segments < 1) return std::numeric_limits<qint64>::max();
    const qint64 nodes = segments + 1;
    const qint64 dofsPerNode = normalized.value(QStringLiteral("modelType")).toString()
            == QLatin1String("cantileverBeam") ? 6 : 1;
    if (nodes > std::numeric_limits<qint64>::max() / dofsPerNode)
        return std::numeric_limits<qint64>::max();
    return nodes * dofsPerNode;
}

bool isAllZeroInitialCondition(const QJsonObject &condition)
{
    const QJsonObject selector = condition.value(QStringLiteral("selector")).toObject();
    const QString selectorKind = selector.value(QStringLiteral("kind")).toString();
    if (selectorKind != QLatin1String("allNodes") && selectorKind != QLatin1String("fixedEnd")
        && selectorKind != QLatin1String("freeEnd") && selectorKind != QLatin1String("position"))
        return false;
    const QJsonArray components = condition.value(QStringLiteral("components")).toArray();
    if (components.isEmpty()) return false;
    for (const QJsonValue &component : components)
        if (!component.isDouble() || !std::isfinite(component.toDouble()) || component.toDouble() != 0.0)
            return false;
    return true;
}

QString canonicalObject(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

struct Displacement
{
    QString key;
    QString selectorCanonical;
    QString component;
    QVector<double> coordinate;
    QVector<double> values;
    QVector<double> times;
};

struct Measurement
{
    QString analysisKind;
    QMap<QString, Displacement> displacements;
    bool hasEnergy = false;
    double energy = 0.0;
    bool hasFrequency = false;
    double frequency = 0.0;
    bool hasMode = false;
    int mode = 0;
    int expectedDisplacementCount = -1;
};

bool readFiniteArray(const QJsonValue &value, QVector<double> *output, int requiredSize = -1)
{
    if (!output || !value.isArray()) return false;
    const QJsonArray array = value.toArray();
    if ((requiredSize >= 0 && array.size() != requiredSize) || array.isEmpty()) return false;
    QVector<double> parsed;
    parsed.reserve(array.size());
    for (const QJsonValue &entry : array) {
        if (!entry.isDouble() || !std::isfinite(entry.toDouble())) return false;
        parsed.append(entry.toDouble());
    }
    *output = std::move(parsed);
    return true;
}

bool parseMeasurement(const QJsonObject &raw, Measurement *measurement, QString *reason)
{
    if (!measurement || !hasOnlyKeys(raw, {QStringLiteral("analysisKind"), QStringLiteral("displacements"),
                                            QStringLiteral("strainEnergyJ"), QStringLiteral("frequencyHz"),
                                            QStringLiteral("mode"), QStringLiteral("expectedDisplacementCount")})) {
        if (reason) *reason = QStringLiteral("Measurement object contains unsupported fields.");
        return false;
    }
    const QString analysisKind = raw.value(QStringLiteral("analysisKind")).toString();
    if (!supportedAnalysis(analysisKind) || !raw.value(QStringLiteral("displacements")).isArray()) {
        if (reason) *reason = QStringLiteral("Measurement must identify a supported analysisKind and a displacements array.");
        return false;
    }

    Measurement parsed;
    parsed.analysisKind = analysisKind;
    const QJsonValue expectedDisplacementCount = raw.value(QStringLiteral("expectedDisplacementCount"));
    if (!expectedDisplacementCount.isDouble()
        || std::floor(expectedDisplacementCount.toDouble()) != expectedDisplacementCount.toDouble()
        || expectedDisplacementCount.toInt(-1) < 0) {
        if (reason) *reason = QStringLiteral("expectedDisplacementCount must be a nonnegative integer.");
        return false;
    }
    parsed.expectedDisplacementCount = expectedDisplacementCount.toInt();
    const QJsonArray displacements = raw.value(QStringLiteral("displacements")).toArray();
    for (const QJsonValue &value : displacements) {
        if (!value.isObject()) {
            if (reason) *reason = QStringLiteral("Each displacement measurement must be an object.");
            return false;
        }
        const QJsonObject item = value.toObject();
        if (!hasOnlyKeys(item, {QStringLiteral("selector"), QStringLiteral("component"),
                                QStringLiteral("coordinateM"), QStringLiteral("values"), QStringLiteral("timesS")})) {
            if (reason) *reason = QStringLiteral("Displacement measurement contains unsupported fields.");
            return false;
        }
        const QJsonObject selector = item.value(QStringLiteral("selector")).toObject();
        const QString component = item.value(QStringLiteral("component")).toString();
        static const QSet<QString> components = {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                                   QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")};
        QVector<double> coordinate, valuesVector, times;
        if (selector.isEmpty()
            || QJsonDocument(selector).toJson(QJsonDocument::Compact).size()
                > Service::MaximumSelectorIdentityBytes
            || !components.contains(component)
            || !readFiniteArray(item.value(QStringLiteral("coordinateM")), &coordinate, 3)
            || !readFiniteArray(item.value(QStringLiteral("values")), &valuesVector)) {
            if (reason) *reason = QStringLiteral("Displacement selector (maximum 256 compact bytes), component, coordinateM, or values are invalid.");
            return false;
        }
        if (analysisKind == QLatin1String("linearTransient")) {
            if (!item.contains(QStringLiteral("timesS"))
                || !readFiniteArray(item.value(QStringLiteral("timesS")), &times)
                || times.size() != valuesVector.size() || times.size() < 2) {
                if (reason) *reason = QStringLiteral("Transient displacement requires matching finite timesS and values arrays with at least two samples.");
                return false;
            }
            for (int i = 1; i < times.size(); ++i) {
                if (times.at(i) <= times.at(i - 1)) {
                    if (reason) *reason = QStringLiteral("Transient measurement timesS must increase strictly.");
                    return false;
                }
            }
        } else if (item.contains(QStringLiteral("timesS")) || valuesVector.size() != 1) {
            if (reason) *reason = QStringLiteral("Static and modal displacement measurements require one value and no timesS.");
            return false;
        }

        const QString selectorKey = canonicalObject(selector);
        const QString key = selectorKey + QLatin1Char('|') + component;
        if (parsed.displacements.contains(key)) {
            if (reason) *reason = QStringLiteral("Duplicate displacement selector/component measurement.");
            return false;
        }
        parsed.displacements.insert(key, Displacement{key, selectorKey, component,
                                                       std::move(coordinate), std::move(valuesVector), std::move(times)});
    }

    if (raw.contains(QStringLiteral("strainEnergyJ"))) {
        const QJsonValue energy = raw.value(QStringLiteral("strainEnergyJ"));
        if (!energy.isDouble() || !std::isfinite(energy.toDouble()) || energy.toDouble() < 0.0
            || analysisKind != QLatin1String("static")) {
            if (reason) *reason = QStringLiteral("strainEnergyJ must be a finite nonnegative static-analysis value.");
            return false;
        }
        parsed.hasEnergy = true;
        parsed.energy = energy.toDouble();
    }
    if (raw.contains(QStringLiteral("frequencyHz"))) {
        const QJsonValue frequency = raw.value(QStringLiteral("frequencyHz"));
        if (!frequency.isDouble() || !std::isfinite(frequency.toDouble()) || frequency.toDouble() <= 0.0
            || analysisKind != QLatin1String("modal")) {
            if (reason) *reason = QStringLiteral("frequencyHz must be a finite positive modal-analysis value.");
            return false;
        }
        parsed.hasFrequency = true;
        parsed.frequency = frequency.toDouble();
    }
    if (raw.contains(QStringLiteral("mode"))) {
        const QJsonValue mode = raw.value(QStringLiteral("mode"));
        if (!mode.isDouble() || std::floor(mode.toDouble()) != mode.toDouble()
            || mode.toInt() < 1 || analysisKind != QLatin1String("modal")) {
            if (reason) *reason = QStringLiteral("mode must be a positive one-based integer for modal analysis.");
            return false;
        }
        parsed.hasMode = true;
        parsed.mode = mode.toInt();
    }
    if (analysisKind == QLatin1String("linearTransient") && parsed.displacements.size() != 1) {
        if (reason) *reason = QStringLiteral("Transient comparison requires exactly one selected displacement history.");
        return false;
    }
    if (parsed.displacements.size() != parsed.expectedDisplacementCount
        || (analysisKind == QLatin1String("static") && parsed.expectedDisplacementCount < 1)) {
        if (reason) *reason = QStringLiteral("Every declared displacement QOI must be present in the convergence measurement.");
        return false;
    }
    if (analysisKind == QLatin1String("static") && !parsed.hasEnergy) {
        if (reason) *reason = QStringLiteral("Static measurement must provide strainEnergyJ; missing metrics are not treated as zero.");
        return false;
    }
    if (analysisKind == QLatin1String("modal") && (!parsed.hasFrequency || !parsed.hasMode)) {
        if (reason) *reason = QStringLiteral("Modal measurement must provide frequencyHz and mode; missing metrics are not treated as zero.");
        return false;
    }
    *measurement = std::move(parsed);
    return true;
}

bool sameCoordinates(const QVector<double> &a, const QVector<double> &b)
{
    if (a.size() != 3 || b.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!nearlyEqual(a.at(i), b.at(i), 1.0e-10, 1.0e-12)) return false;
    return true;
}

QJsonObject comparisonEntry(const QString &key, const QString &quantity,
                             double absoluteChange, double fineScale)
{
    bool defined = std::isfinite(absoluteChange) && std::isfinite(fineScale) && fineScale >= 0.0;
    double change = 0.0;
    if (defined && fineScale == 0.0) defined = absoluteChange == 0.0;
    else if (defined) {
        change = absoluteChange / fineScale;
        defined = std::isfinite(change);
    }
    QJsonObject result{{QStringLiteral("key"), key}, {QStringLiteral("quantity"), quantity},
                       {QStringLiteral("status"), defined ? QStringLiteral("defined") : QStringLiteral("undefined")},
                       {QStringLiteral("belowTolerance"), defined && change < Service::FixedRelativeTolerance}};
    if (defined) result.insert(QStringLiteral("relativeChange"), change);
    else result.insert(QStringLiteral("relativeChange"), QJsonValue(QJsonValue::Null));
    result.insert(QStringLiteral("absoluteChange"), std::isfinite(absoluteChange)
                  ? QJsonValue(absoluteChange) : QJsonValue(QJsonValue::Null));
    result.insert(QStringLiteral("fineScale"), std::isfinite(fineScale)
                  ? QJsonValue(fineScale) : QJsonValue(QJsonValue::Null));
    return result;
}

} // namespace

qsizetype RTrustedConvergenceService::estimateMaximumTransientRecordBytes(
    int frames, int displacementQois)
{
    const auto validation = RTrustedMechanicsService::validate(genericTransientProblemSpec());
    if (!validation.valid) return std::numeric_limits<qsizetype>::max();
    return estimateMaximumTransientRecordBytes(genericTransientProblemSpec(), frames, displacementQois);
}

qsizetype RTrustedConvergenceService::estimateMaximumTransientRecordBytes(
    const QJsonObject &problemSpec, int frames, int displacementQois)
{
    return RTrustedRecordSerialization::maximumTransientRecordBytes(problemSpec, frames, displacementQois);
}

RTrustedConvergenceService::Outcome RTrustedConvergenceService::validateStudy(
    const QJsonObject &problemSpec, const QJsonObject &study)
{
    if (QJsonDocument(problemSpec).toJson(QJsonDocument::Compact).size() > 64 * 1024)
        return failure(QStringLiteral("STUDY_PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("A convergence ProblemSpec input must be at most 64 KiB."));
    if (study.contains(QStringLiteral("tolerance")) || study.contains(QStringLiteral("toleranceRelative")))
        return failure(QStringLiteral("STUDY_TOLERANCE_FIXED"),
                       QStringLiteral("Convergence tolerance is fixed at 1%; callers cannot override it."));
    if (!hasOnlyKeys(study, {QStringLiteral("kind"), QStringLiteral("maximumLevels")}) || study.size() != 2)
        return failure(QStringLiteral("STUDY_SPEC_INVALID"),
                       QStringLiteral("Study must contain exactly kind and maximumLevels."));
    const QString kind = study.value(QStringLiteral("kind")).toString();
    if (kind != QLatin1String("mesh") && kind != QLatin1String("timeStep"))
        return failure(QStringLiteral("STUDY_KIND_INVALID"), QStringLiteral("Study kind must be mesh or timeStep."));
    const QJsonValue levelsValue = study.value(QStringLiteral("maximumLevels"));
    if (!levelsValue.isDouble() || std::floor(levelsValue.toDouble()) != levelsValue.toDouble()
        || levelsValue.toInt(-1) < 3 || levelsValue.toInt(-1) > MaximumLevels)
        return failure(QStringLiteral("STUDY_LEVELS_INVALID"),
                       QStringLiteral("maximumLevels must be an integer from 3 through 4, including the baseline."));

    const RTrustedMechanicsService::ValidationResult validation = RTrustedMechanicsService::validate(problemSpec);
    if (!validation.valid) {
        return failure(QStringLiteral("PROBLEM_SPEC_INVALID"),
                       QStringLiteral("ProblemSpec is not valid for a convergence study."),
                       {{QStringLiteral("issues"), validation.issues}});
    }
    const QJsonObject normalized = validation.normalizedSpec;
    if (RTrustedRecordSerialization::compact(normalized).size() > 64 * 1024)
        return failure(QStringLiteral("STUDY_PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("A convergence ProblemSpec must be at most 64 KiB so its durable evidence remains bounded."));
    const QJsonObject analysis = normalized.value(QStringLiteral("analysis")).toObject();
    const QString analysisKind = analysis.value(QStringLiteral("kind")).toString();
    if (!supportedAnalysis(analysisKind))
        return failure(QStringLiteral("ANALYSIS_KIND_UNSUPPORTED"), QStringLiteral("Analysis kind is unsupported."));
    if (kind == QLatin1String("timeStep") && analysisKind != QLatin1String("linearTransient"))
        return failure(QStringLiteral("STUDY_ANALYSIS_MISMATCH"),
                       QStringLiteral("Time-step convergence is supported only for linearTransient analysis."));

    // The verifier currently extracts point displacement QOIs and the selected
    // modal frequency. Reject every other declared quantity rather than silently
    // reporting convergence while ignoring a user-requested stress/reaction/etc.
    const QJsonArray observables = normalized.value(QStringLiteral("observables")).toArray();
    int displacementCount = 0;
    int frequencyCount = 0;
    const QString modelType = normalized.value(QStringLiteral("modelType")).toString();
    for (const QJsonValue &value : observables) {
        const QJsonObject observable = value.toObject();
        const QString quantity = observable.value(QStringLiteral("quantity")).toString();
        const QJsonObject selector = observable.value(QStringLiteral("selector")).toObject();
        const QString selectorKind = selector.value(QStringLiteral("kind")).toString();
        if (quantity == QLatin1String("displacement")) {
            if (QJsonDocument(selector).toJson(QJsonDocument::Compact).size()
                > MaximumSelectorIdentityBytes)
                return failure(QStringLiteral("STUDY_QOI_SELECTOR_TOO_LARGE"),
                    QStringLiteral("Normalized convergence selectors must fit within 256 compact bytes so their change identity remains bounded."),
                    {{QStringLiteral("maximumSelectorIdentityBytes"), MaximumSelectorIdentityBytes}});
            if (analysisKind == QLatin1String("modal"))
                return failure(QStringLiteral("STUDY_QOI_UNSUPPORTED"),
                    QStringLiteral("Modal displacement amplitude is arbitrary; modal convergence supports frequency only."),
                    {{QStringLiteral("observable"), observable}});
            const QString component = observable.value(QStringLiteral("component")).toString();
            const bool supportedComponent = component == QLatin1String("ux")
                || (modelType == QLatin1String("cantileverBeam")
                    && (component == QLatin1String("uy") || component == QLatin1String("uz")));
            if (!supportedComponent || (selectorKind != QLatin1String("fixedEnd")
                    && selectorKind != QLatin1String("freeEnd") && selectorKind != QLatin1String("position")))
                return failure(QStringLiteral("STUDY_QOI_UNSUPPORTED"),
                    QStringLiteral("Convergence displacement QOIs require a supported component and a single fixedEnd, freeEnd, or position selector."),
                    {{QStringLiteral("observable"), observable}});
            ++displacementCount;
        } else if (quantity == QLatin1String("frequency") && analysisKind == QLatin1String("modal")
                   && observable.value(QStringLiteral("component")).toString() == QLatin1String("mode")) {
            ++frequencyCount;
        } else {
            return failure(QStringLiteral("STUDY_QOI_UNSUPPORTED"),
                QStringLiteral("This convergence verifier cannot compare every declared quantity; supported quantities are displacement and the selected modal frequency."),
                {{QStringLiteral("observable"), observable}});
        }
    }
    if (displacementCount == 0 && analysisKind != QLatin1String("modal"))
        return failure(QStringLiteral("STUDY_QOI_MISSING"),
                       QStringLiteral("Static and transient studies require at least one verified displacement QOI."));
    if (analysisKind == QLatin1String("static")) {
        const qsizetype maximumMeasurementBytes =
            estimateMaximumStaticConvergenceMeasurementBytes(normalized);
        if (maximumMeasurementBytes > MaximumConvergenceMeasurementBytes)
            return failure(QStringLiteral("STUDY_MEASUREMENT_SIZE_LIMIT"),
                           QStringLiteral("The maximum static convergence measurement exceeds the 64 KiB verification-report limit; reduce the declared displacement QOIs before any solve."),
                           {{QStringLiteral("maximumMeasurementBytes"), double(maximumMeasurementBytes)},
                            {QStringLiteral("maximumConvergenceMeasurementBytes"),
                             double(MaximumConvergenceMeasurementBytes)},
                            {QStringLiteral("displacementQois"), displacementCount}});
    }
    if (analysisKind == QLatin1String("modal") && frequencyCount != 1)
        return failure(QStringLiteral("STUDY_QOI_INCOMPLETE"),
                       QStringLiteral("Modal convergence requires exactly one selected frequency observable."),
                       {{QStringLiteral("frequencyObservableCount"), frequencyCount}});
    const int levels = levelsValue.toInt();
    if (kind == QLatin1String("mesh")) {
        const QJsonArray initialConditions = normalized.value(QStringLiteral("initialConditions")).toArray();
        if (analysisKind == QLatin1String("linearTransient")) {
            for (const QJsonValue &value : initialConditions) {
                if (!value.isObject() || !isAllZeroInitialCondition(value.toObject()))
                    return failure(QStringLiteral("MESH_INITIAL_CONDITION_UNSUPPORTED"),
                                   QStringLiteral("Transient mesh refinement preserves initial fields only when every explicit component is zero and selectors remain well-defined."));
            }
            const qint64 steps = analysis.value(QStringLiteral("steps")).toInt(-1);
            if (steps < 1 || steps + 1 > MaximumTransientFrames)
                return failure(QStringLiteral("STUDY_FRAME_LIMIT"),
                               QStringLiteral("The fixed transient mesh-study time grid exceeds 10000 saved frames, including the initial frame."),
                               {{QStringLiteral("maximumTransientFrames"), MaximumTransientFrames},
                                {QStringLiteral("finalSteps"), double(steps)},
                                {QStringLiteral("finalFrames"), double(steps + 1)}});
            const qsizetype serializedBytes = estimateMaximumTransientRecordBytes(
                problemSpec, int(steps + 1), displacementCount);
            if (serializedBytes > MaximumDurableRecordBytes)
                return failure(QStringLiteral("STUDY_RECORD_SIZE_LIMIT"),
                               QStringLiteral("The maximum permitted transient mesh-study record exceeds the 4 MiB durable experiment-record limit."),
                               {{QStringLiteral("maximumSerializedRecordBytes"), double(serializedBytes)},
                                {QStringLiteral("maximumDurableRecordBytes"), double(MaximumDurableRecordBytes)},
                                {QStringLiteral("finalFrames"), double(steps + 1)},
                                {QStringLiteral("displacementQois"), displacementCount}});
        }
        const qint64 baselineSegments = normalized.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toInt(-1);
        const qint64 factor = qint64(1) << (levels - 1);
        if (baselineSegments < 1 || baselineSegments > std::numeric_limits<qint64>::max() / factor
            || dofsAtSegments(normalized, baselineSegments * factor) > MaximumReferenceDofs)
            return failure(QStringLiteral("STUDY_DOF_LIMIT"),
                           QStringLiteral("The finest requested mesh level exceeds the verifier's 192 total-DOF dense-reference limit."),
                           {{QStringLiteral("maximumReferenceDofs"), MaximumReferenceDofs},
                            {QStringLiteral("maximumLevels"), levels}});
    } else {
        const double timeStep = analysis.value(QStringLiteral("timeStep")).toDouble(0.0);
        const qint64 baselineSteps = analysis.value(QStringLiteral("steps")).toInt(-1);
        const qint64 factor = qint64(1) << (levels - 1);
        if (!std::isfinite(timeStep) || timeStep <= 0.0 || baselineSteps < 1
            || baselineSteps > std::numeric_limits<qint64>::max() / factor)
            return failure(QStringLiteral("STUDY_DURATION_INVALID"),
                           QStringLiteral("The baseline time grid has no finite positive duration."));
        const qint64 finalSteps = baselineSteps * factor;
        const double baselineDuration = timeStep * double(baselineSteps);
        const double finalDuration = (timeStep / double(factor)) * double(finalSteps);
        if (!std::isfinite(baselineDuration) || baselineDuration <= 0.0
            || !std::isfinite(finalDuration) || !nearlyEqual(baselineDuration, finalDuration))
            return failure(QStringLiteral("STUDY_DURATION_INVALID"),
                           QStringLiteral("The requested time-step levels cannot preserve the finite baseline duration."));
        if (finalSteps + 1 > MaximumTransientFrames)
            return failure(QStringLiteral("STUDY_FRAME_LIMIT"),
                           QStringLiteral("The finest time grid exceeds 10000 saved frames, including the initial frame."),
                           {{QStringLiteral("maximumTransientFrames"), MaximumTransientFrames},
                            {QStringLiteral("finalSteps"), double(finalSteps)},
                            {QStringLiteral("finalFrames"), double(finalSteps + 1)}});
        const qsizetype serializedBytes = estimateMaximumTransientRecordBytes(
            problemSpec, int(finalSteps + 1), displacementCount);
        if (serializedBytes > MaximumDurableRecordBytes)
            return failure(QStringLiteral("STUDY_RECORD_SIZE_LIMIT"),
                           QStringLiteral("The maximum permitted transient time-step-study record exceeds the 4 MiB durable experiment-record limit."),
                           {{QStringLiteral("maximumSerializedRecordBytes"), double(serializedBytes)},
                            {QStringLiteral("maximumDurableRecordBytes"), double(MaximumDurableRecordBytes)},
                            {QStringLiteral("finalFrames"), double(finalSteps + 1)},
                            {QStringLiteral("displacementQois"), displacementCount}});
    }

    if (analysisKind == QLatin1String("linearTransient") && displacementCount != 1)
        return failure(QStringLiteral("STUDY_QOI_INCOMPLETE"),
                       QStringLiteral("Transient convergence currently requires exactly one selected displacement history."),
                       {{QStringLiteral("displacementObservableCount"), displacementCount}});

    const QJsonObject physicalSummary = RTrustedMechanicsService::physicalSummary(normalized);
    return success({{QStringLiteral("study"), study},
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("maximumLevels"), levels},
                    {QStringLiteral("fixedToleranceRelative"), FixedRelativeTolerance},
                    {QStringLiteral("analysisKind"), analysisKind},
                    {QStringLiteral("normalizedProblemSpec"), normalized},
                    {QStringLiteral("physicalSummary"), physicalSummary},
                    {QStringLiteral("physicalSummaryDigest"), RTrustedMechanicsService::summaryDigest(physicalSummary)}});
}

RTrustedConvergenceService::Outcome RTrustedConvergenceService::proposeLevel(
    const QJsonObject &baselineSpec, const QJsonObject &study, int levelIndex, QJsonObject *candidateSpec)
{
    if (!candidateSpec)
        return failure(QStringLiteral("CANDIDATE_OUTPUT_MISSING"), QStringLiteral("A candidate output pointer is required."));
    const Outcome studyResult = validateStudy(baselineSpec, study);
    if (!studyResult.ok) return studyResult;
    const int maximumLevels = studyResult.data.value(QStringLiteral("maximumLevels")).toInt();
    if (levelIndex < 1 || levelIndex >= maximumLevels)
        return failure(QStringLiteral("LEVEL_INDEX_INVALID"),
                       QStringLiteral("levelIndex must select a refinement after baseline and before maximumLevels."),
                       {{QStringLiteral("levelIndex"), levelIndex}, {QStringLiteral("maximumLevels"), maximumLevels}});

    const QJsonObject normalized = studyResult.data.value(QStringLiteral("normalizedProblemSpec")).toObject();
    const QString kind = studyResult.data.value(QStringLiteral("kind")).toString();
    const qint64 factor = qint64(1) << levelIndex;
    QJsonObject candidate = baselineSpec;
    if (kind == QLatin1String("mesh")) {
        const qint64 baselineSegments = normalized.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toInt(-1);
        if (baselineSegments < 1 || baselineSegments > std::numeric_limits<int>::max() / factor)
            return failure(QStringLiteral("MESH_SEGMENT_LIMIT"), QStringLiteral("Requested mesh level exceeds supported segment count."));
        const qint64 segments = baselineSegments * factor;
        if (dofsAtSegments(normalized, segments) > MaximumReferenceDofs)
            return failure(QStringLiteral("STUDY_DOF_LIMIT"), QStringLiteral("Candidate exceeds the verifier's 192 total-DOF limit."),
                           {{QStringLiteral("segments"), double(segments)},
                            {QStringLiteral("totalDofs"), double(dofsAtSegments(normalized, segments))}});
        QJsonObject geometryInput = candidate.value(QStringLiteral("geometry")).toObject();
        geometryInput.insert(QStringLiteral("segments"), int(segments));
        candidate.insert(QStringLiteral("geometry"), geometryInput);
    } else {
        QJsonObject analysisInput = candidate.value(QStringLiteral("analysis")).toObject();
        const QJsonObject normalizedAnalysis = normalized.value(QStringLiteral("analysis")).toObject();
        const double baselineTimeStep = normalizedAnalysis.value(QStringLiteral("timeStep")).toDouble(0.0);
        const qint64 baselineSteps = normalizedAnalysis.value(QStringLiteral("steps")).toInt(-1);
        if (!std::isfinite(baselineTimeStep) || baselineTimeStep <= 0.0
            || baselineSteps < 1 || baselineSteps > std::numeric_limits<int>::max() / factor)
            return failure(QStringLiteral("STUDY_DURATION_INVALID"), QStringLiteral("Requested time-step level overflows its finite time grid."));
        const qint64 steps = baselineSteps * factor;
        const double timeStep = baselineTimeStep / double(factor);
        const double baselineDuration = baselineTimeStep * double(baselineSteps);
        const double candidateDuration = timeStep * double(steps);
        if (!std::isfinite(candidateDuration) || !nearlyEqual(baselineDuration, candidateDuration))
            return failure(QStringLiteral("STUDY_DURATION_INVALID"), QStringLiteral("Proposed time grid does not preserve baseline duration."));
        analysisInput.insert(QStringLiteral("timeStep"),
                             QJsonObject{{QStringLiteral("value"), timeStep}, {QStringLiteral("unit"), QStringLiteral("s")}});
        analysisInput.insert(QStringLiteral("steps"), int(steps));
        candidate.insert(QStringLiteral("analysis"), analysisInput);
    }

    const RTrustedMechanicsService::ValidationResult candidateValidation =
        RTrustedMechanicsService::validate(withoutDerivedDuration(candidate));
    if (!candidateValidation.valid)
        return failure(QStringLiteral("CANDIDATE_SPEC_INVALID"),
                       QStringLiteral("Generated convergence candidate failed mechanics validation."),
                       {{QStringLiteral("issues"), candidateValidation.issues}});
    const QJsonObject candidateNormalized = candidateValidation.normalizedSpec;
    const QString expectedPhysicalDigest = studyResult.data.value(QStringLiteral("physicalSummaryDigest")).toString();
    const QString observedPhysicalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::physicalSummary(candidateNormalized));
    if (expectedPhysicalDigest.isEmpty() || expectedPhysicalDigest != observedPhysicalDigest)
        return failure(QStringLiteral("CANDIDATE_PHYSICS_CHANGED"),
                       QStringLiteral("Generated numerical refinement changed the validated physical problem."));

    if (kind == QLatin1String("timeStep")) {
        const QJsonObject candidateAnalysis = candidateNormalized.value(QStringLiteral("analysis")).toObject();
        const double duration = candidateAnalysis.value(QStringLiteral("duration")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double baselineDuration = normalized.value(QStringLiteral("analysis")).toObject()
            .value(QStringLiteral("duration")).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(duration) || !std::isfinite(baselineDuration) || !nearlyEqual(duration, baselineDuration))
            return failure(QStringLiteral("STUDY_DURATION_INVALID"), QStringLiteral("Validated candidate duration differs from the baseline."));
    }
    *candidateSpec = candidate;
    return success({{QStringLiteral("candidateProblemSpec"), candidate},
                    {QStringLiteral("candidateNormalizedSpec"), candidateNormalized},
                    {QStringLiteral("levelIndex"), levelIndex},
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("physicalSummaryDigest"), observedPhysicalDigest},
                    {QStringLiteral("fixedToleranceRelative"), FixedRelativeTolerance}});
}

RTrustedConvergenceService::Outcome RTrustedConvergenceService::compareMeasurements(
    const QString &kind, const QJsonObject &previousRaw, const QJsonObject &currentRaw)
{
    static const QSet<QString> allowedKinds = {QStringLiteral("mesh"), QStringLiteral("timeStep"),
                                                QStringLiteral("static"), QStringLiteral("modal"),
                                                QStringLiteral("linearTransient")};
    if (!allowedKinds.contains(kind))
        return failure(QStringLiteral("COMPARISON_KIND_INVALID"), QStringLiteral("Comparison kind is unsupported."));
    Measurement previous, current;
    QString reason;
    if (!parseMeasurement(previousRaw, &previous, &reason))
        return failure(QStringLiteral("MEASUREMENT_INVALID"), QStringLiteral("Previous measurement is invalid: %1").arg(reason));
    if (!parseMeasurement(currentRaw, &current, &reason))
        return failure(QStringLiteral("MEASUREMENT_INVALID"), QStringLiteral("Current measurement is invalid: %1").arg(reason));
    if (previous.analysisKind != current.analysisKind)
        return failure(QStringLiteral("MEASUREMENT_ANALYSIS_MISMATCH"), QStringLiteral("Adjacent measurements have different analysis kinds."));
    if ((kind == QLatin1String("static") || kind == QLatin1String("modal")
         || kind == QLatin1String("linearTransient")) && kind != previous.analysisKind)
        return failure(QStringLiteral("COMPARISON_KIND_MISMATCH"), QStringLiteral("Comparison kind disagrees with measurement analysisKind."));
    if (kind == QLatin1String("timeStep") && previous.analysisKind != QLatin1String("linearTransient"))
        return failure(QStringLiteral("COMPARISON_KIND_MISMATCH"), QStringLiteral("Time-step comparison requires linearTransient measurements."));

    if (previous.displacements.keys() != current.displacements.keys())
        return failure(QStringLiteral("MEASUREMENT_QOI_MISMATCH"), QStringLiteral("Adjacent measurements do not contain the same selected displacement components."));
    QJsonArray qoiChanges;
    for (auto it = previous.displacements.cbegin(); it != previous.displacements.cend(); ++it) {
        const Displacement &coarse = it.value();
        const Displacement &fine = current.displacements.value(it.key());
        if (coarse.selectorCanonical != fine.selectorCanonical || coarse.component != fine.component
            || !sameCoordinates(coarse.coordinate, fine.coordinate))
            return failure(QStringLiteral("MEASUREMENT_SELECTOR_MISMATCH"),
                           QStringLiteral("Selected displacement selector, component, or coordinate changed between levels."),
                           {{QStringLiteral("key"), it.key()}});
        QVector<double> previousValues, currentValues;
        if (previous.analysisKind == QLatin1String("linearTransient")) {
            for (int i = 0; i < coarse.times.size(); ++i) {
                int match = -1;
                for (int j = 0; j < fine.times.size(); ++j) {
                    if (!timeEqual(coarse.times.at(i), fine.times.at(j))) continue;
                    if (match >= 0)
                        return failure(QStringLiteral("MEASUREMENT_TIME_AMBIGUOUS"),
                                       QStringLiteral("A previous time matches multiple current samples."));
                    match = j;
                }
                if (match >= 0) {
                    previousValues.append(coarse.values.at(i));
                    currentValues.append(fine.values.at(match));
                }
            }
            if (previousValues.size() < 2)
                return failure(QStringLiteral("MEASUREMENT_COMMON_TIMES_MISSING"),
                               QStringLiteral("Transient histories need at least two common timestamps; interpolation is not performed."));
        } else {
            previousValues = coarse.values;
            currentValues = fine.values;
        }
        double maxDifference = 0.0;
        double fineScale = 0.0;
        for (int i = 0; i < previousValues.size(); ++i) {
            maxDifference = std::max(maxDifference, std::abs(currentValues.at(i) - previousValues.at(i)));
            fineScale = std::max(fineScale, std::abs(currentValues.at(i)));
        }
        qoiChanges.append(comparisonEntry(it.key(), QStringLiteral("displacement"), maxDifference, fineScale));
    }

    if (previous.analysisKind == QLatin1String("static")) {
        if (previous.hasEnergy != current.hasEnergy)
            return failure(QStringLiteral("MEASUREMENT_QOI_MISMATCH"), QStringLiteral("Strain energy is missing from one adjacent static measurement."));
        if (previous.hasEnergy) {
            const double difference = std::abs(current.energy - previous.energy);
            const double scale = std::abs(current.energy);
            qoiChanges.append(comparisonEntry(QStringLiteral("strainEnergyJ"),
                                              QStringLiteral("strainEnergy"), difference, scale));
        }
    } else if (previous.analysisKind == QLatin1String("modal")) {
        if (previous.hasFrequency != current.hasFrequency || previous.hasMode != current.hasMode
            || previous.mode != current.mode)
            return failure(QStringLiteral("MEASUREMENT_QOI_MISMATCH"), QStringLiteral("Adjacent modal measurements must identify the same selected mode and include its frequency."));
        if (previous.hasFrequency) {
            const double difference = std::abs(current.frequency - previous.frequency);
            const double scale = std::abs(current.frequency);
            qoiChanges.append(comparisonEntry(QStringLiteral("frequencyHz"),
                                              QStringLiteral("frequency"), difference, scale));
        }
    }
    if (qoiChanges.isEmpty())
        return failure(QStringLiteral("MEASUREMENT_QOI_MISSING"), QStringLiteral("No comparable quantity of interest was provided."));

    bool allDefined = true;
    bool allBelow = true;
    double aggregate = 0.0;
    for (const QJsonValue &value : qoiChanges) {
        const QJsonObject qoi = value.toObject();
        const bool defined = qoi.value(QStringLiteral("status")).toString() == QLatin1String("defined");
        allDefined = allDefined && defined;
        allBelow = allBelow && defined && qoi.value(QStringLiteral("belowTolerance")).toBool();
        if (defined) aggregate = std::max(aggregate, qoi.value(QStringLiteral("relativeChange")).toDouble());
    }
    QJsonObject data{{QStringLiteral("status"), allDefined ? QStringLiteral("compared") : QStringLiteral("undefined")},
                     {QStringLiteral("analysisKind"), previous.analysisKind},
                     {QStringLiteral("fixedToleranceRelative"), FixedRelativeTolerance},
                     {QStringLiteral("qoiChanges"), qoiChanges},
                     {QStringLiteral("belowTolerance"), allBelow}};
    if (kind == QLatin1String("mesh") || kind == QLatin1String("timeStep"))
        data.insert(QStringLiteral("studyKind"), kind);
    if (allDefined) data.insert(QStringLiteral("aggregateRelativeChange"), aggregate);
    else data.insert(QStringLiteral("aggregateRelativeChange"), QJsonValue(QJsonValue::Null));
    return success(data);
}

bool RTrustedConvergenceService::isConverged(const QJsonArray &adjacentChanges, int consecutiveRequired)
{
    if (consecutiveRequired < 2 || adjacentChanges.size() < consecutiveRequired) return false;
    for (int i = adjacentChanges.size() - consecutiveRequired; i < adjacentChanges.size(); ++i) {
        if (!adjacentChanges.at(i).isObject()) return false;
        const QJsonArray qois = adjacentChanges.at(i).toObject().value(QStringLiteral("qoiChanges")).toArray();
        if (qois.isEmpty()) return false;
        for (const QJsonValue &value : qois) {
            if (!value.isObject()) return false;
            const QJsonObject qoi = value.toObject();
            if (qoi.value(QStringLiteral("status")).toString() != QLatin1String("defined")) return false;
            const QJsonValue relative = qoi.value(QStringLiteral("relativeChange"));
            if (!relative.isDouble() || !std::isfinite(relative.toDouble())
                || relative.toDouble() >= FixedRelativeTolerance)
                return false;
        }
    }
    return true;
}
