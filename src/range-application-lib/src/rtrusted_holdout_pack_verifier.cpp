#include <rtrusted_holdout_pack_verifier.h>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

namespace
{
using Outcome = RTrustedHoldoutPackVerifier::Outcome;
constexpr qint64 maxJsonBytes = 8 * 1024 * 1024;
const QJsonArray arms{QStringLiteral("direct_nl"), QStringLiteral("tools_feedback"), QStringLiteral("full_trusted")};
const QJsonArray repeats{1, 2, 3};

Outcome reject(const char *code, const char *message)
{
    Outcome result;
    result.code = QString::fromLatin1(code);
    result.message = QString::fromLatin1(message);
    return result;
}

QString hash(const QByteArray &raw)
{
    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}

bool identifier(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\\z"));
    return pattern.match(value).hasMatch();
}

bool digest(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return pattern.match(value).hasMatch();
}

bool fields(const QJsonValue &value, std::initializer_list<const char *> expected)
{
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    if (object.size() != qsizetype(expected.size())) return false;
    for (const char *field : expected)
        if (!object.contains(QLatin1String(field))) return false;
    return true;
}

// Python json.dumps(ensure_ascii=False, sort_keys=True, separators=(',', ':')).
// Accepted schemas contain only integer numbers. Canonical round-trip also
// rejects duplicate keys (Qt otherwise retains only their final occurrence),
// alternate escapes, BOMs, invalid UTF-8 and noncanonical number spellings.
bool appendString(const QString &value, QByteArray &out)
{
    out += '"';
    for (qsizetype i = 0; i < value.size(); ++i)
    {
        const ushort code = value[i].unicode();
        if (QChar::isHighSurrogate(code))
        {
            if (++i >= value.size() || !value[i].isLowSurrogate()) return false;
            out += QString(value.mid(i - 1, 2)).toUtf8();
        }
        else if (QChar::isLowSurrogate(code)) return false;
        else if (code == '"') out += "\\\"";
        else if (code == '\\') out += "\\\\";
        else if (code == '\b') out += "\\b";
        else if (code == '\f') out += "\\f";
        else if (code == '\n') out += "\\n";
        else if (code == '\r') out += "\\r";
        else if (code == '\t') out += "\\t";
        else if (code < 32) out += "\\u" + QByteArray::number(code, 16).rightJustified(4, '0');
        else out += QString(value[i]).toUtf8();
    }
    out += '"';
    return true;
}

bool canonical(const QJsonValue &value, QByteArray &out)
{
    if (value.isObject())
    {
        const auto object = value.toObject();
        auto keys = object.keys();
        std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
            const auto x = a.toUcs4(), y = b.toUcs4();
            return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
        });
        out += '{';
        bool first = true;
        for (const auto &key : keys)
        {
            if (!first) out += ',';
            first = false;
            if (!appendString(key, out)) return false;
            out += ':';
            if (!canonical(object.value(key), out)) return false;
        }
        out += '}';
    }
    else if (value.isArray())
    {
        out += '[';
        bool first = true;
        for (const auto &element : value.toArray())
        {
            if (!first) out += ',';
            first = false;
            if (!canonical(element, out)) return false;
        }
        out += ']';
    }
    else if (value.isString()) return appendString(value.toString(), out);
    else if (value.isBool()) out += value.toBool() ? "true" : "false";
    else if (value.isNull()) out += "null";
    else if (value.isDouble())
    {
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || std::abs(number) > 9007199254740991.0) return false;
        out += QByteArray::number(qint64(number));
    }
    else return false;
    return true;
}

bool readCanonical(const QString &path, QByteArray &raw, QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxJsonBytes) return false;
    raw = file.read(maxJsonBytes + 1);
    if (file.error() != QFileDevice::NoError || raw.size() > maxJsonBytes
        || QString::fromUtf8(raw).toUtf8() != raw) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    QByteArray serialized;
    return canonical(object, serialized) && serialized == raw;
}

bool integer(const QJsonValue &value, qint64 expected)
{
    return value.isDouble() && value.toDouble() == double(expected);
}

bool nonemptyText(const QJsonValue &value, qsizetype maxBytes)
{
    if (!value.isString() || value.toString().toUtf8().size() > maxBytes) return false;
    // Python str.strip() additionally treats U+001C..U+001F as whitespace.
    for (const auto character : value.toString())
        if (!character.isSpace() && !(character.unicode() >= 0x1c && character.unicode() <= 0x1f)) return true;
    return false;
}

bool verifySignature(const QJsonObject &key, const QByteArray &message, const QByteArray &signature)
{
    const auto hex = key.value(QStringLiteral("modulusHex")).toString().toLatin1();
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9a-f][0-9a-f]*\\z"));
    const auto exponent = key.value(QStringLiteral("publicExponent"));
    if (hex.size() < 512 || hex.size() > 2048 || !pattern.match(QString::fromLatin1(hex)).hasMatch() || !exponent.isDouble()
        || exponent.toDouble() < 3 || exponent.toDouble() > 0xffffffffULL
        || std::floor(exponent.toDouble()) != exponent.toDouble()
        || (quint64(exponent.toDouble()) % 2) == 0) return false;
    BIGNUM *rawN = nullptr;
    if (BN_hex2bn(&rawN, hex.constData()) == 0) return false;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n(rawN, BN_free), e(BN_new(), BN_free);
    if (!e || BN_num_bits(n.get()) < 2048 || BN_num_bits(n.get()) > 8192
        || !BN_is_odd(n.get()) || signature.size() != BN_num_bytes(n.get())
        || !BN_set_word(e.get(), BN_ULONG(exponent.toDouble())) || BN_cmp(e.get(), n.get()) >= 0) return false;
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    if (!builder || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, n.get())
        || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, e.get())) return false;
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *rawKey = nullptr;
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) <= 0
        || EVP_PKEY_fromdata(context.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(rawKey, EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> verify(EVP_PKEY_CTX_new(publicKey.get(), nullptr), EVP_PKEY_CTX_free);
    QByteArray recovered(BN_num_bytes(n.get()), '\0');
    size_t recoveredSize = size_t(recovered.size());
    // Recover with strict PKCS#1 padding, then compare the exact SHA-256
    // DigestInfo encoding used by Python. This also excludes alternative ASN.1
    // encodings that a general-purpose signature verifier might accept.
    if (!verify || EVP_PKEY_verify_recover_init(verify.get()) <= 0
        || EVP_PKEY_CTX_set_rsa_padding(verify.get(), RSA_PKCS1_PADDING) <= 0
        || EVP_PKEY_verify_recover(verify.get(), reinterpret_cast<unsigned char *>(recovered.data()), &recoveredSize,
                                   reinterpret_cast<const unsigned char *>(signature.constData()), size_t(signature.size())) <= 0) return false;
    recovered.resize(qsizetype(recoveredSize));
    return recovered == QByteArray::fromHex("3031300d060960864801650304020105000420")
        + QCryptographicHash::hash(message, QCryptographicHash::Sha256);
}
}

Outcome RTrustedHoldoutPackVerifier::verifyPublicSlot(const Request &request)
{
    if (!identifier(request.runId) || !arms.contains(request.armId) || request.slotId.isEmpty()
        || !digest(request.independentlyPinnedKeySha256)
        || !digest(request.externallyRecordedPreRunCommitmentSha256))
        return reject("invalid_request", "Run, arm, slot and independently supplied trust anchors are required.");
    QByteArray rawKey, rawManifest, rawCases;
    QJsonObject key, manifest, publicCases;
    if (!readCanonical(request.publicKeyPath, rawKey, key) || !readCanonical(request.manifestPath, rawManifest, manifest))
        return reject("invalid_canonical_json", "Public key and manifest must be bounded canonical UTF-8 JSON objects.");
    if (!fields(manifest, {"schemaVersion", "keyId", "payload", "signatureBase64"})
        || manifest.value("schemaVersion") != QStringLiteral("trusted-agent-stage7-external-manifest/v1")
        || !identifier(manifest.value("keyId").toString()))
        return reject("invalid_manifest", "The signed manifest schema or key identity is invalid.");
    if (!fields(key, {"schemaVersion", "keyId", "modulusHex", "publicExponent"})
        || key.value("schemaVersion") != QStringLiteral("trusted-agent-stage7-rsa-public-key/v1")
        || key.value("keyId") != manifest.value("keyId"))
        return reject("invalid_public_key", "The public key schema or identity is invalid.");
    if (hash(rawKey) != request.independentlyPinnedKeySha256)
        return reject("key_fingerprint_mismatch", "Public key bytes do not match the independently supplied fingerprint.");
    if (hash(rawManifest) != request.externallyRecordedPreRunCommitmentSha256)
        return reject("manifest_commitment_mismatch", "Manifest bytes do not match the externally recorded pre-run commitment.");
    const auto payloadValue = manifest.value("payload");
    const auto payload = payloadValue.toObject();
    if (!fields(payloadValue, {"holdoutId", "protocolVersion", "publicCasesSha256", "answerKeySha256", "caseCount", "categoryCounts", "arms", "repeats", "unitSystem", "custodyDeclaration"})
        || !identifier(payload.value("holdoutId").toString())
        || !digest(payload.value("publicCasesSha256").toString()) || !digest(payload.value("answerKeySha256").toString())
        || payload.value("protocolVersion") != QStringLiteral("trusted-agent-stage7-holdout/v1")
        || !integer(payload.value("caseCount"), 20) || payload.value("arms") != arms || payload.value("repeats") != repeats
        || payload.value("unitSystem") != QStringLiteral("SI")
        || payload.value("categoryCounts") != QJsonObject{{"normal", 12}, {"fault", 8}})
        return reject("invalid_protocol", "Manifest payload does not match the frozen Stage 7 protocol.");
    const auto declaration = payload.value("custodyDeclaration");
    if (!fields(declaration, {"role", "independenceStatement", "overlapSearchScope"})
        || declaration.toObject().value("role") != QStringLiteral("independent-case-custodian")
        || !nonemptyText(declaration.toObject().value("independenceStatement"), 4096)
        || !nonemptyText(declaration.toObject().value("overlapSearchScope"), 4096))
        return reject("invalid_declaration", "Signed custody declaration is incomplete.");
    const auto signatureValue = manifest.value("signatureBase64");
    const auto encoded = signatureValue.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    QByteArray signedBytes;
    if (!signatureValue.isString() || !decoded || decoded.decoded.toBase64() != encoded
        || QString::fromLatin1(encoded) != signatureValue.toString()
        || !canonical(QJsonObject{{"keyId", manifest.value("keyId")}, {"payload", payload}}, signedBytes)
        || !verifySignature(key, signedBytes, decoded.decoded))
        return reject("invalid_signature", "Manifest RSA PKCS#1 v1.5 SHA-256 signature or public-key values are invalid.");
    if (!readCanonical(request.publicCasesPath, rawCases, publicCases))
        return reject("invalid_canonical_json", "Public cases must be bounded canonical UTF-8 JSON.");
    if (hash(rawCases) != payload.value("publicCasesSha256").toString())
        return reject("public_cases_digest_mismatch", "Public case bytes do not match the signed manifest.");
    if (!fields(publicCases, {"schemaVersion", "holdoutId", "unitSystem", "arms", "repeats", "cases"})
        || publicCases.value("schemaVersion") != QStringLiteral("trusted-agent-stage7-external-public/v1")
        || publicCases.value("holdoutId") != payload.value("holdoutId") || publicCases.value("unitSystem") != QStringLiteral("SI")
        || publicCases.value("arms") != arms || publicCases.value("repeats") != repeats
        || !publicCases.value("cases").isArray() || publicCases.value("cases").toArray().size() != 20)
        return reject("invalid_public_cases", "Public case pack identity or frozen protocol is invalid.");
    QSet<QString> ids;
    int normal = 0, fault = 0, matches = 0;
    VerifiedSlot matched;
    for (const auto &caseValue : publicCases.value("cases").toArray())
    {
        const auto item = caseValue.toObject();
        const auto caseId = item.value("caseId").toString();
        const auto category = item.value("category").toString();
        if (!fields(caseValue, {"caseId", "category", "prompt"}) || !identifier(caseId) || ids.contains(caseId)
            || (category != QStringLiteral("normal") && category != QStringLiteral("fault")) || !nonemptyText(item.value("prompt"), 16 * 1024))
            return reject("invalid_public_cases", "A public case schema, identifier, category or prompt is invalid.");
        ids.insert(caseId);
        normal += category == QStringLiteral("normal");
        fault += category == QStringLiteral("fault");
        for (int repeat = 1; repeat <= 3; ++repeat)
        {
            const auto slot = request.armId + ':' + payload.value("holdoutId").toString() + ':' + caseId + ":r" + QString::number(repeat);
            if (slot != request.slotId) continue;
            ++matches;
            matched = {request.runId, request.armId, payload.value("holdoutId").toString(), slot, caseId, repeat,
                       hash(rawManifest), hash(item.value("prompt").toString().toUtf8())};
        }
    }
    if (normal != 12 || fault != 8)
        return reject("invalid_public_cases", "Case categories do not match the signed 12 normal / 8 fault split.");
    if (matches != 1) return reject("invalid_slot", "The request must identify exactly one derived public slot.");
    Outcome result;
    result.valid = true;
    result.code = QStringLiteral("verified");
    result.message = QStringLiteral("Public pack signature, trust anchors and slot bytes verified.");
    result.slot = matched;
    return result;
}
