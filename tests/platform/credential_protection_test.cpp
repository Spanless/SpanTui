#include "core/product_paths.h"
#include "platform/credential_protection.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using span::core::ProductPaths;
namespace credentials = span::platform::credentials;

// The macOS form: secrets sealed under the data root's key file.
class CredentialProtectionTest final : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { ProductPaths::setRootOverride({}); }
    void sealedSecretOpensUnderTheSameDataRoot();
    void unreadableKeyIsKeptAndNothingIsSealed();
};

void CredentialProtectionTest::sealedSecretOpensUnderTheSameDataRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ProductPaths::setRootOverride(root.path());

    const QByteArray blob = credentials::protectSecret(QStringLiteral("s3cret passphrase"));
    QVERIFY(!blob.isEmpty());
    QVERIFY(!blob.contains("s3cret"));
    bool ok = false;
    QCOMPARE(credentials::unprotectSecret(blob, &ok), QStringLiteral("s3cret passphrase"));
    QVERIFY(ok);
    QCOMPARE(QFile::permissions(ProductPaths::credentialKeyPath()),
             QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
}

void CredentialProtectionTest::unreadableKeyIsKeptAndNothingIsSealed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ProductPaths::setRootOverride(root.path());
    const QByteArray key(64, 'k');
    QFile file(ProductPaths::credentialKeyPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(key), qint64(64));
    file.close();
    QVERIFY(file.setPermissions(QFileDevice::Permissions()));

    QVERIFY(credentials::protectSecret(QStringLiteral("secret")).isEmpty());

    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), key);
}

QTEST_GUILESS_MAIN(CredentialProtectionTest)

#include "credential_protection_test.moc"
