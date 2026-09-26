#ifndef TYPED_CONTROL_TASK_H
#define TYPED_CONTROL_TASK_H

#include <QString>
#include <QStringList>
#include <optional>

struct TypedControlTaskInput
{
    QString inputFile;
    QString configurationFile;
    QString checkpointFile;
    QString controllerIdentity;
    QString program;
    QStringList arguments;
    uint steps=0;
    uint timeoutMs=5000;
    std::optional<uint> nThreads;
};

//! One whole CLI invocation: prepare in memory and atomically publish once.
//! On failure before publication, the previous checkpoint remains intact.
int runTypedControlTask(const TypedControlTaskInput &input,QString &error);

#endif
