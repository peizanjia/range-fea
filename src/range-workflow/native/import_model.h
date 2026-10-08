#ifndef RANGE_WORKFLOW_IMPORT_MODEL_H
#define RANGE_WORKFLOW_IMPORT_MODEL_H

#include <QJsonObject>
#include <QString>

namespace RangeNativeImport
{
//! Describe one immutable, hash-bound raw native MODEL for WF-03.
//! The function only reads the supplied file and always returns the
//! range.native-import/v1 response envelope, including on validation failure.
QJsonObject describe(const QString &inputPath, const QString &expectedSha256);

//! Prepare an isolated source copy after a successful hash-bound preflight.
//! The accepted source is never written; the output is atomically published.
QJsonObject prepareSource(const QString &inputPath,
                          const QString &expectedSha256,
                          const QString &outputPath,
                          uint steps,
                          uint outputFrequency,
                          double timeStep,
                          bool passive);
}

#endif // RANGE_WORKFLOW_IMPORT_MODEL_H
