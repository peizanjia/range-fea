#ifndef PIEZO_NUMERIC_TRANSPORT_H
#define PIEZO_NUMERIC_TRANSPORT_H
#include <QJsonObject>
#include <QString>
#include <memory>
#include <utility>

//! Private owner-thread NDJSON channel, shared by native FE families.
//! Runtime/source hashes are compatibility identities, not authentication.
//! On Windows internal package/executable files are pinned against write/delete
//! and their directory chains against rename until destruction. Every request
//! still checks exact package inventory and held file/directory metadata;
//! user excitation files retain exact per-point validation.
class PiezoNumericTransport final
{
public:
    PiezoNumericTransport(const QString &python,const QString &moduleRoot);
    ~PiezoNumericTransport();
    PiezoNumericTransport(const PiezoNumericTransport &)=delete;
    PiezoNumericTransport &operator=(const PiezoNumericTransport &)=delete;
    QJsonObject request(QJsonObject message);
    //! Owner-thread single pending request. Begin flushes the complete line;
    //! finish receives and applies the original post-source guard.
    void beginRequest(QJsonObject message);
    QJsonObject finishRequest();
    //! Abandoning a pending response kills this transport's own provider.
    void abortPendingRequest();
    const QString &runtimeIdentity() const noexcept;
    const QJsonObject &runtimeMetadata() const noexcept;
    static std::pair<QString,QString> defaultRuntime();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
