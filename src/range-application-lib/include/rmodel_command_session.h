#ifndef RMODEL_COMMAND_SESSION_H
#define RMODEL_COMMAND_SESSION_H

#include <QJsonObject>
#include <QThread>
#include <memory>
#include <rml_model.h>

// Single-owner, in-memory editing session. The input model is copied; callers
// publish the accepted model separately. Tokens are session-local, not file hashes.
class RModelCommandSession
{
public:
    static constexpr int MaxCommands = 256;
    explicit RModelCommandSession(const RModel &model);
    ~RModelCommandSession();
    RModelCommandSession(const RModelCommandSession &) = delete;
    RModelCommandSession &operator=(const RModelCommandSession &) = delete;

    const RModel &model() const;
    QJsonObject query() const;
    QJsonObject execute(const QJsonObject &request);
    static QJsonObject modalParameters(const RModalSetup &setup);

private:
    struct State;
    std::unique_ptr<State> state;
    QString identity;
    Qt::HANDLE ownerThread;
    QString revision() const;
};

#endif
