#include <QByteArray>
#include <QString>
#include <QTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class ApplicationManifestTest final : public QObject {
    Q_OBJECT

private slots:
    void imageMatchesConfiguredEnvironment();
};

void ApplicationManifestTest::imageMatchesConfiguredEnvironment()
{
    const QString executable = QDir::toNativeSeparators(QStringLiteral(SPANTUI_APPLICATION_PATH));
    const std::wstring nativePath = executable.toStdWString();
    const HMODULE module =
        LoadLibraryExW(nativePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    QVERIFY2(module != nullptr, qPrintable(QStringLiteral("Could not load %1 (%2)")
                                               .arg(executable)
                                               .arg(GetLastError())));

    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST);
    QVERIFY(resource != nullptr);
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void *data = loaded ? LockResource(loaded) : nullptr;
    QVERIFY(data != nullptr);
    const QByteArray manifest(static_cast<const char *>(data), qsizetype(size));
    QVERIFY(manifest.contains("requestedExecutionLevel"));
#if SPANTUI_EXPECT_INSTALLED_IMAGE
    QVERIFY2(manifest.contains("name=\"Span.SpanTui\""),
             "The installed image must embed the formal assembly identity.");
#else
    QVERIFY2(manifest.contains("name=\"Span.SpanTui.Development\""),
             "The local image must embed the development assembly identity.");
#endif
    QVERIFY2(!manifest.contains("<msix"),
             "The application process must not claim the Explorer extension's sparse package identity.");
    FreeLibrary(module);
}

QTEST_MAIN(ApplicationManifestTest)

#include "application_manifest_test.moc"
