#ifndef RUNTIME_CONTROL_JSON_H
#define RUNTIME_CONTROL_JSON_H
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <cmath>

namespace RuntimeControlJson {
// Qt's parser accepts duplicate object keys. Walk the original lexical tree
// first, decoding each key so escaped aliases also count as duplicates.
class UniqueKeys {
    const QByteArray &b; qsizetype i = 0;
    void space() { while (i < b.size() && (b[i]==' ' || b[i]=='\t' || b[i]=='\r' || b[i]=='\n')) ++i; }
    bool string(QString *decoded = nullptr) {
        space(); if (i >= b.size() || b[i] != '"') return false;
        const qsizetype start = i++;
        while (i < b.size()) {
            const char c = b[i++];
            if (c == '\\') { if (i >= b.size()) return false; ++i; }
            else if (c == '"') {
                if (decoded) {
                    QJsonParseError e;
                    const auto d = QJsonDocument::fromJson("[" + b.mid(start,i-start) + "]", &e);
                    if (e.error != QJsonParseError::NoError || d.array().size()!=1 || !d.array()[0].isString()) return false;
                    *decoded = d.array()[0].toString();
                }
                return true;
            }
        }
        return false;
    }
    bool value(int depth) {
        space(); if (depth>16 || i>=b.size()) return false;
        if (b[i]=='"') return string();
        if (b[i]=='{' || b[i]=='[') {
            const bool object = b[i++]=='{'; const char end = object ? '}' : ']'; QSet<QString> keys;
            space(); if (i<b.size() && b[i]==end) { ++i; return true; }
            for (;;) {
                if (object) {
                    QString key; if (!string(&key) || keys.contains(key)) return false; keys.insert(key);
                    space(); if (i>=b.size() || b[i++]!=':') return false;
                }
                if (!value(depth+1)) return false;
                space(); if (i>=b.size()) return false;
                if (b[i]==end) { ++i; return true; }
                if (b[i++]!=',') return false;
            }
        }
        const auto start = i;
        while (i<b.size() && b[i]!=',' && b[i]!=']' && b[i]!='}' && b[i]!=' ' && b[i]!='\t' && b[i]!='\r' && b[i]!='\n') ++i;
        return i>start;
    }
public:
    explicit UniqueKeys(const QByteArray &bytes):b(bytes) {}
    bool valid() { if (!value(0)) return false; space(); return i==b.size(); }
};
inline bool finite(const QJsonValue &v) {
    if (v.isNull() || v.isUndefined()) return false;
    if (v.isDouble()) return std::isfinite(v.toDouble());
    if (v.isArray()) { for (const auto &x:v.toArray()) if (!finite(x)) return false; }
    if (v.isObject()) { for (const auto &x:v.toObject()) if (!finite(x)) return false; }
    return true;
}
inline bool parse(const QByteArray &bytes, QJsonObject &object)
{
    if (bytes.isEmpty() || bytes.size()>65536 || QString::fromUtf8(bytes).toUtf8()!=bytes || !UniqueKeys(bytes).valid()) return false;
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(bytes,&error);
    if (error.error!=QJsonParseError::NoError || !document.isObject() || !finite(document.object())) return false;
    object=document.object(); return true;
}
}
#endif
