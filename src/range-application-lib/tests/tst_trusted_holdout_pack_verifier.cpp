#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <rtrusted_holdout_pack_verifier.h>

namespace
{
using Verifier = RTrustedHoldoutPackVerifier;
QString hash(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}

struct Fixture
{
    QTemporaryDir directory;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    QJsonObject publicKey, cases, payload, manifest;
    Verifier::Request request;
    bool initialize()
    {
        // Fresh process-local test key: never read or persist a private key.
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY *raw = nullptr;
        if (!directory.isValid() || !context || EVP_PKEY_keygen_init(context.get()) <= 0
            || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 || EVP_PKEY_generate(context.get(), &raw) <= 0) return false;
        key.reset(raw);
        BIGNUM *rawN = nullptr;
        if (EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_N, &rawN) <= 0) return false;
        std::unique_ptr<BIGNUM, decltype(&BN_free)> n(rawN, BN_free);
        char *hex = BN_bn2hex(n.get());
        if (!hex) return false;
        const QString modulus = QString::fromLatin1(hex).toLower();
        OPENSSL_free(hex);
        publicKey = {{"schemaVersion", "trusted-agent-stage7-rsa-public-key/v1"}, {"keyId", "fixture-key"},
                     {"modulusHex", modulus}, {"publicExponent", 65537}};
        QJsonArray list;
        for (int i = 0; i < 20; ++i)
            list.append(QJsonObject{{"caseId", QStringLiteral("case:%1").arg(i)}, {"category", i < 12 ? "normal" : "fault"},
                                    {"prompt", i == 0 ? QString::fromUtf8("計算軸向位移 😀\n長度 １ m / ε") : QStringLiteral("Fixture prompt %1").arg(i)}});
        const QJsonArray arms{"direct_nl", "tools_feedback", "full_trusted"}, repeats{1, 2, 3};
        cases = {{"schemaVersion", "trusted-agent-stage7-external-public/v1"}, {"holdoutId", "holdout:fixture"},
                 {"unitSystem", "SI"}, {"arms", arms}, {"repeats", repeats}, {"cases", list}};
        payload = {{"holdoutId", "holdout:fixture"}, {"protocolVersion", "trusted-agent-stage7-holdout/v1"},
                   {"publicCasesSha256", hash(json(cases))}, {"answerKeySha256", QString(64, 'a')}, {"caseCount", 20},
                   {"categoryCounts", QJsonObject{{"normal", 12}, {"fault", 8}}}, {"arms", arms}, {"repeats", repeats}, {"unitSystem", "SI"},
                   {"custodyDeclaration", QJsonObject{{"role", "independent-case-custodian"}, {"independenceStatement", "Synthetic test declaration"},
                                                    {"overlapSearchScope", "Synthetic fixture scope"}}}};
        request = {directory.filePath("manifest.json"), directory.filePath("public.json"), directory.filePath("key.json"),
                   hash(json(publicKey)), {}, "run-fixture", "full_trusted", "full_trusted:holdout:fixture:case:0:r2"};
        return write(request.publicKeyPath, json(publicKey)) && resign();
    }
    bool resign()
    {
        payload["publicCasesSha256"] = hash(json(cases));
        const auto signedBytes = json(QJsonObject{{"keyId", "fixture-key"}, {"payload", payload}});
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        EVP_PKEY_CTX *sign = nullptr;
        size_t size = 0;
        if (!md || EVP_DigestSignInit(md.get(), &sign, EVP_sha256(), nullptr, key.get()) <= 0
            || EVP_PKEY_CTX_set_rsa_padding(sign, RSA_PKCS1_PADDING) <= 0
            || EVP_DigestSign(md.get(), nullptr, &size, reinterpret_cast<const unsigned char *>(signedBytes.constData()), size_t(signedBytes.size())) <= 0) return false;
        QByteArray signature(qsizetype(size), '\0');
        if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char *>(signature.data()), &size,
                           reinterpret_cast<const unsigned char *>(signedBytes.constData()), size_t(signedBytes.size())) <= 0) return false;
        signature.resize(qsizetype(size));
        manifest = {{"schemaVersion", "trusted-agent-stage7-external-manifest/v1"}, {"keyId", "fixture-key"},
                    {"payload", payload}, {"signatureBase64", QString::fromLatin1(signature.toBase64())}};
        return saveManifest() && write(request.publicCasesPath, json(cases));
    }
    bool saveManifest(const QByteArray &overrideBytes = {})
    {
        const auto bytes = overrideBytes.isEmpty() ? json(manifest) : overrideBytes;
        request.externallyRecordedPreRunCommitmentSha256 = hash(bytes);
        return write(request.manifestPath, bytes);
    }
};

void emptySlot(const Verifier::Outcome &outcome)
{
    QVERIFY(!outcome.valid);
    QVERIFY(!outcome.code.isEmpty());
    QVERIFY(!outcome.message.isEmpty());
    QVERIFY(outcome.slot.runId.isEmpty());
    QVERIFY(outcome.slot.armId.isEmpty());
    QVERIFY(outcome.slot.holdoutId.isEmpty());
    QVERIFY(outcome.slot.slotId.isEmpty());
    QVERIFY(outcome.slot.caseId.isEmpty());
    QCOMPARE(outcome.slot.repeat, 0);
    QVERIFY(outcome.slot.promptSha256.isEmpty());
    QVERIFY(outcome.slot.manifestCommitmentSha256.isEmpty());
}
}

class TstTrustedHoldoutPackVerifier : public QObject
{
    Q_OBJECT
private slots:
    void verifiesUnicodeAndAllArms();
    void rejectsTrustAndSignatureFailures();
    void rejectsCaseAndProtocolFailures();
    void rejectsCanonicalFailures();
    void rejectsSlotAndRequestFailures();
    void rejectsKeyConstraints();
};

void TstTrustedHoldoutPackVerifier::verifiesUnicodeAndAllArms()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto expectedPrompt = QByteArray::fromHex("e8a888e7ae97e8bbb8e59091e4bd8de7a7bb20f09f98800ae995b7e5baa620efbc91206d202f20ceb5");
    for (const auto &arm : {QStringLiteral("direct_nl"), QStringLiteral("tools_feedback"), QStringLiteral("full_trusted")})
    {
        fixture.request.armId = arm;
        fixture.request.slotId = arm + ":holdout:fixture:case:0:r2";
        const auto result = Verifier::verifyPublicSlot(fixture.request);
        QVERIFY2(result.valid, qPrintable(result.code + ": " + result.message));
        QCOMPARE(result.code, QStringLiteral("verified"));
        QCOMPARE(result.slot.runId, fixture.request.runId);
        QCOMPARE(result.slot.armId, arm);
        QCOMPARE(result.slot.holdoutId, QStringLiteral("holdout:fixture"));
        QCOMPARE(result.slot.slotId, fixture.request.slotId);
        QCOMPARE(result.slot.caseId, QStringLiteral("case:0"));
        QCOMPARE(result.slot.repeat, 2);
        QCOMPARE(result.slot.promptSha256, hash(expectedPrompt));
        QCOMPARE(result.slot.manifestCommitmentSha256, fixture.request.externallyRecordedPreRunCommitmentSha256);
    }
}

void TstTrustedHoldoutPackVerifier::rejectsTrustAndSignatureFailures()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    auto request = fixture.request;
    request.independentlyPinnedKeySha256 = QString(64, '0');
    QCOMPARE(Verifier::verifyPublicSlot(request).code, QStringLiteral("key_fingerprint_mismatch"));
    emptySlot(Verifier::verifyPublicSlot(request));
    request = fixture.request;
    request.externallyRecordedPreRunCommitmentSha256 = QString(64, '0');
    QCOMPARE(Verifier::verifyPublicSlot(request).code, QStringLiteral("manifest_commitment_mismatch"));
    emptySlot(Verifier::verifyPublicSlot(request));
    auto signature = QByteArray::fromBase64(fixture.manifest.value("signatureBase64").toString().toLatin1());
    signature[3] = char(signature[3] ^ 1);
    fixture.manifest["signatureBase64"] = QString::fromLatin1(signature.toBase64());
    QVERIFY(fixture.saveManifest());
    QCOMPARE(Verifier::verifyPublicSlot(fixture.request).code, QStringLiteral("invalid_signature"));
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
    fixture.manifest["signatureBase64"] = QStringLiteral("!!not-base64!!");
    QVERIFY(fixture.saveManifest());
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
    // A freshly signed alternate ASN.1 DigestInfo (omitted NULL parameters)
    // must not satisfy the exact Python EMSA-PKCS1-v1_5 byte contract.
    const auto signedBytes = json(QJsonObject{{"keyId", "fixture-key"}, {"payload", fixture.payload}});
    const auto alternate = QByteArray::fromHex("302f300b06096086480165030402010420")
        + QCryptographicHash::hash(signedBytes, QCryptographicHash::Sha256);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> signer(EVP_PKEY_CTX_new(fixture.key.get(), nullptr), EVP_PKEY_CTX_free);
    QVERIFY(signer);
    QVERIFY(EVP_PKEY_sign_init(signer.get()) > 0);
    QVERIFY(EVP_PKEY_CTX_set_rsa_padding(signer.get(), RSA_PKCS1_PADDING) > 0);
    signature.resize(256);
    size_t signatureSize = size_t(signature.size());
    QVERIFY(EVP_PKEY_sign(signer.get(), reinterpret_cast<unsigned char *>(signature.data()), &signatureSize,
                          reinterpret_cast<const unsigned char *>(alternate.constData()), size_t(alternate.size())) > 0);
    signature.resize(qsizetype(signatureSize));
    fixture.manifest["signatureBase64"] = QString::fromLatin1(signature.toBase64());
    QVERIFY(fixture.saveManifest());
    QCOMPARE(Verifier::verifyPublicSlot(fixture.request).code, QStringLiteral("invalid_signature"));
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
}

void TstTrustedHoldoutPackVerifier::rejectsCaseAndProtocolFailures()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto originalCases = fixture.cases;
    const auto originalPayload = fixture.payload;
    auto list = fixture.cases.value("cases").toArray();
    auto item = list.last().toObject(); // Invalid undispatched case must also reject.
    item["prompt"] = "tampered";
    list[19] = item;
    fixture.cases["cases"] = list;
    QVERIFY(write(fixture.request.publicCasesPath, json(fixture.cases)));
    QCOMPARE(Verifier::verifyPublicSlot(fixture.request).code, QStringLiteral("public_cases_digest_mismatch"));
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
    for (const auto &mutation : {QStringLiteral("duplicate"), QStringLiteral("category"), QStringLiteral("empty"), QStringLiteral("python_whitespace"), QStringLiteral("large"), QStringLiteral("extra"), QStringLiteral("invalid_id"), QStringLiteral("count")})
    {
        fixture.cases = originalCases;
        list = fixture.cases.value("cases").toArray();
        item = list.last().toObject();
        if (mutation == "duplicate") item["caseId"] = "case:0";
        if (mutation == "category") item["category"] = "normal";
        if (mutation == "empty") item["prompt"] = " \n\t";
        if (mutation == "python_whitespace") item["prompt"] = QString(QChar(0x1c));
        if (mutation == "large") item["prompt"] = QString(16385, 'x');
        if (mutation == "extra") item["answer"] = "must reject";
        if (mutation == "invalid_id") item["caseId"] = "../case";
        list[19] = item;
        if (mutation == "count") list.removeLast();
        fixture.cases["cases"] = list;
        QVERIFY(fixture.resign());
        emptySlot(Verifier::verifyPublicSlot(fixture.request));
    }
    fixture.cases = originalCases;
    for (const auto &field : {QStringLiteral("caseCount"), QStringLiteral("arms"), QStringLiteral("repeats"), QStringLiteral("unitSystem"), QStringLiteral("custodyDeclaration"), QStringLiteral("answerKeySha256")})
    {
        fixture.payload = originalPayload;
        fixture.payload[field] = QJsonValue();
        QVERIFY(fixture.resign());
        emptySlot(Verifier::verifyPublicSlot(fixture.request));
    }
}

void TstTrustedHoldoutPackVerifier::rejectsCanonicalFailures()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto canonicalManifest = json(fixture.manifest);
    auto duplicate = canonicalManifest;
    duplicate.insert(1, "\"keyId\":\"fixture-key\",");
    auto floatInteger = canonicalManifest;
    floatInteger.replace("\"caseCount\":20", "\"caseCount\":20.0");
    auto surrogate = canonicalManifest;
    surrogate.replace("Synthetic test declaration", "\\ud800");
    for (const auto &bytes : {canonicalManifest + '\n', duplicate, floatInteger, surrogate, QByteArray("\xef\xbb\xbf") + canonicalManifest,
                              QByteArray("{\"bad\":NaN}"), QByteArray(8 * 1024 * 1024 + 1, ' ')})
    {
        QVERIFY(fixture.saveManifest(bytes));
        emptySlot(Verifier::verifyPublicSlot(fixture.request));
    }
    QVERIFY(fixture.saveManifest());
    auto escapedPublic = json(fixture.cases);
    escapedPublic.replace(QByteArray::fromHex("e8a888"), "\\u8a08");
    QVERIFY(write(fixture.request.publicCasesPath, escapedPublic));
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
    QVERIFY(write(fixture.request.publicKeyPath, json(fixture.publicKey) + '\n'));
    fixture.request.independentlyPinnedKeySha256 = hash(json(fixture.publicKey) + '\n');
    emptySlot(Verifier::verifyPublicSlot(fixture.request));
}

void TstTrustedHoldoutPackVerifier::rejectsSlotAndRequestFailures()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    for (const auto &slot : {QStringLiteral("full_trusted:holdout:fixture:case:0:r4"), QStringLiteral("full_trusted:holdout:fixture:missing:r2"),
                             QStringLiteral("direct_nl:holdout:fixture:case:0:r2"), QStringLiteral("full_trusted:wrong:case:0:r2")})
    {
        auto request = fixture.request;
        request.slotId = slot;
        emptySlot(Verifier::verifyPublicSlot(request));
    }
    auto request = fixture.request;
    request.runId = "../run";
    emptySlot(Verifier::verifyPublicSlot(request));
    request = fixture.request;
    request.armId = "unknown";
    emptySlot(Verifier::verifyPublicSlot(request));
    request = fixture.request;
    request.manifestPath = fixture.directory.filePath("missing.json");
    emptySlot(Verifier::verifyPublicSlot(request));
}

void TstTrustedHoldoutPackVerifier::rejectsKeyConstraints()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto original = fixture.publicKey;
    for (const auto &mutation : {QStringLiteral("small"), QStringLiteral("large"), QStringLiteral("even"), QStringLiteral("leading_zero"),
                                 QStringLiteral("exponent_even"), QStringLiteral("exponent_large"), QStringLiteral("exponent_bool"),
                                 QStringLiteral("key_id"), QStringLiteral("schema"), QStringLiteral("extra")})
    {
        fixture.publicKey = original;
        if (mutation == "small") fixture.publicKey["modulusHex"] = QString(511, 'f');
        if (mutation == "large") fixture.publicKey["modulusHex"] = QString(2049, 'f');
        if (mutation == "even") fixture.publicKey["modulusHex"] = original.value("modulusHex").toString().chopped(1) + 'e';
        if (mutation == "leading_zero") fixture.publicKey["modulusHex"] = '0' + original.value("modulusHex").toString();
        if (mutation == "exponent_even") fixture.publicKey["publicExponent"] = 4;
        if (mutation == "exponent_large") fixture.publicKey["publicExponent"] = 4294967297.0;
        if (mutation == "exponent_bool") fixture.publicKey["publicExponent"] = true;
        if (mutation == "key_id") fixture.publicKey["keyId"] = "other-key";
        if (mutation == "schema") fixture.publicKey["schemaVersion"] = "other-schema";
        if (mutation == "extra") fixture.publicKey["privateExponent"] = "forbidden-extra-field";
        const auto bytes = json(fixture.publicKey);
        QVERIFY(write(fixture.request.publicKeyPath, bytes));
        fixture.request.independentlyPinnedKeySha256 = hash(bytes);
        emptySlot(Verifier::verifyPublicSlot(fixture.request));
    }
}

QTEST_GUILESS_MAIN(TstTrustedHoldoutPackVerifier)
#include "tst_trusted_holdout_pack_verifier.moc"
