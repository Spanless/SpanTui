#include <windows.h>

#include <shobjidl.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include "platform/windows/shell_extension_contract.h"

namespace {

using DllGetClassObjectFunction = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
using DllCanUnloadNowFunction = HRESULT(WINAPI *)();

QString takeString(PWSTR value)
{
    const QString result = QString::fromWCharArray(value ? value : L"");
    CoTaskMemFree(value);
    return result;
}

// What Explorer hands a command: an array holding the one clicked item.
IShellItemArray *selectionOf(const QString &path)
{
    IShellItem *item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(QDir::toNativeSeparators(path).toStdWString().c_str(),
                                           nullptr, IID_IShellItem,
                                           reinterpret_cast<void **>(&item)))) {
        return nullptr;
    }
    IShellItemArray *selection = nullptr;
    SHCreateShellItemArrayFromShellItem(item, IID_IShellItemArray,
                                        reinterpret_cast<void **>(&selection));
    item->Release();
    return selection;
}

QStringList takeRecordedArguments(const QString &directory)
{
    QFile file(QDir(directory).filePath(QStringLiteral("argv.txt")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QStringList arguments = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();
    file.remove();
    return arguments;
}

} // namespace

class ShellExtensionContractTest : public QObject
{
    Q_OBJECT

private slots:
    void exposesTwoCommandFlyout()
    {
        const QString libraryPath = QString::fromUtf8(SPANTUI_SHELL_EXTENSION_PATH);
        QVERIFY2(QFileInfo::exists(libraryPath), qPrintable(libraryPath));
        QVERIFY2(QFileInfo::exists(QDir(QFileInfo(libraryPath).absolutePath()).filePath("SpanTui-test.exe"))
                     || QFileInfo::exists(
                         QDir(QFileInfo(libraryPath).absolutePath()).filePath("SpanTui-pre.exe")),
                 "The application build must be staged beside the shell extension test image");

        QLibrary library(libraryPath);
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        const auto getClassObject = reinterpret_cast<DllGetClassObjectFunction>(
            library.resolve("DllGetClassObject"));
        const auto canUnloadNow = reinterpret_cast<DllCanUnloadNowFunction>(
            library.resolve("DllCanUnloadNow"));
        QVERIFY(getClassObject);
        QVERIFY(canUnloadNow);

        IClassFactory *factory = nullptr;
        QCOMPARE(getClassObject(spantui::shell::kRootCommandClsid, IID_IClassFactory,
                                reinterpret_cast<void **>(&factory)),
                 S_OK);
        QVERIFY(factory);

        IExplorerCommand *root = nullptr;
        QCOMPARE(factory->CreateInstance(nullptr, IID_IExplorerCommand,
                                         reinterpret_cast<void **>(&root)),
                 S_OK);
        QVERIFY(root);

        PWSTR rootTitle = nullptr;
        QCOMPARE(root->GetTitle(nullptr, &rootTitle), S_OK);
        QCOMPARE(takeString(rootTitle), QStringLiteral("SpanTui"));

        PWSTR rootIcon = nullptr;
        QCOMPARE(root->GetIcon(nullptr, &rootIcon), S_OK);
        QCOMPARE(QFileInfo(takeString(rootIcon)).fileName(), QStringLiteral("SpanTui.exe"));

        EXPCMDFLAGS rootFlags = ECF_DEFAULT;
        QCOMPARE(root->GetFlags(&rootFlags), S_OK);
        QVERIFY(rootFlags & ECF_HASSUBCOMMANDS);

        GUID rootCanonical{};
        QCOMPARE(root->GetCanonicalName(&rootCanonical), S_OK);
        QVERIFY(IsEqualGUID(rootCanonical, spantui::shell::kRootCanonicalName));

        IEnumExplorerCommand *enumerator = nullptr;
        QCOMPARE(root->EnumSubCommands(&enumerator), S_OK);
        QVERIFY(enumerator);

        IExplorerCommand *children[3] = {nullptr, nullptr, nullptr};
        ULONG fetched = 0;
        QCOMPARE(enumerator->Next(3, children, &fetched), S_FALSE);
        QCOMPARE(fetched, 2UL);
        QVERIFY(children[0]);
        QVERIFY(children[1]);
        QVERIFY(!children[2]);

        PWSTR firstTitle = nullptr;
        PWSTR secondTitle = nullptr;
        QCOMPARE(children[0]->GetTitle(nullptr, &firstTitle), S_OK);
        QCOMPARE(children[1]->GetTitle(nullptr, &secondTitle), S_OK);
        const QString first = takeString(firstTitle);
        const QString second = takeString(secondTitle);
        QVERIFY(first.contains(QStringLiteral("SpanTui")));
        QVERIFY(second.contains(QStringLiteral("SpanTui")));
        QVERIFY(first != second);

        GUID firstCanonical{};
        GUID secondCanonical{};
        QCOMPARE(children[0]->GetCanonicalName(&firstCanonical), S_OK);
        QCOMPARE(children[1]->GetCanonicalName(&secondCanonical), S_OK);
        QVERIFY(IsEqualGUID(firstCanonical, spantui::shell::kOpenProjectCanonicalName));
        QVERIFY(IsEqualGUID(secondCanonical, spantui::shell::kOpenTerminalCanonicalName));

        EXPCMDFLAGS childFlags = ECF_HASSUBCOMMANDS;
        QCOMPARE(children[0]->GetFlags(&childFlags), S_OK);
        QCOMPARE(childFlags, ECF_DEFAULT);

        children[0]->Release();
        children[1]->Release();
        enumerator->Release();
        root->Release();
        factory->Release();
        QCOMPARE(canUnloadNow(), S_OK);
        library.unload();
    }

    void commandsLaunchTheAppForTheSelectedFolder()
    {
        // The extension starts SpanTui.exe from its own directory. Staged
        // beside a copy of it, argv_echo_child stands in for the app and
        // records the command line instead of opening a window.
        const HRESULT initialization = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QVERIFY(SUCCEEDED(initialization) || initialization == RPC_E_CHANGED_MODE);
        const auto uninitialize = qScopeGuard([initialization] {
            if (SUCCEEDED(initialization)) {
                CoUninitialize();
            }
        });

        QTemporaryDir stage;
        QVERIFY(stage.isValid());
        const QString libraryPath = stage.filePath(QStringLiteral("SpanTui-shell-extension.dll"));
        QVERIFY(QFile::copy(QString::fromUtf8(SPANTUI_SHELL_EXTENSION_PATH), libraryPath));
        QVERIFY(QFile::copy(QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("argv_echo_child.exe")),
                            stage.filePath(QStringLiteral("SpanTui.exe"))));
        const QString folder = stage.filePath(QStringLiteral("folder with space 测试"));
        QVERIFY(QDir().mkpath(folder));
        const QString file = stage.filePath(QStringLiteral("plain.txt"));
        QFile plain(file);
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.close();

        QLibrary library(libraryPath);
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        const auto getClassObject = reinterpret_cast<DllGetClassObjectFunction>(
            library.resolve("DllGetClassObject"));
        QVERIFY(getClassObject);
        IClassFactory *factory = nullptr;
        QCOMPARE(getClassObject(spantui::shell::kRootCommandClsid, IID_IClassFactory,
                                reinterpret_cast<void **>(&factory)),
                 S_OK);
        IExplorerCommand *root = nullptr;
        QCOMPARE(factory->CreateInstance(nullptr, IID_IExplorerCommand,
                                         reinterpret_cast<void **>(&root)),
                 S_OK);
        IEnumExplorerCommand *enumerator = nullptr;
        QCOMPARE(root->EnumSubCommands(&enumerator), S_OK);
        IExplorerCommand *children[2] = {nullptr, nullptr};
        ULONG fetched = 0;
        QCOMPARE(enumerator->Next(2, children, &fetched), S_OK);
        IShellItemArray *folderSelection = selectionOf(folder);
        IShellItemArray *fileSelection = selectionOf(file);
        QVERIFY(folderSelection);
        QVERIFY(fileSelection);

        const QString nativeFolder = QDir::toNativeSeparators(folder);
        QCOMPARE(root->Invoke(folderSelection, nullptr), E_NOTIMPL);
        QCOMPARE(children[0]->Invoke(folderSelection, nullptr), S_OK);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(folder).filePath(QStringLiteral("argv.txt"))),
                                 10000);
        QCOMPARE(takeRecordedArguments(folder),
                 (QStringList{QStringLiteral("--project"), nativeFolder}));
        QCOMPARE(children[1]->Invoke(folderSelection, nullptr), S_OK);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(folder).filePath(QStringLiteral("argv.txt"))),
                                 10000);
        QCOMPARE(takeRecordedArguments(folder),
                 (QStringList{QStringLiteral("--terminal"), QStringLiteral("--cwd"), nativeFolder}));
        // A file is not a folder: nothing is launched.
        QCOMPARE(children[0]->Invoke(fileSelection, nullptr), E_INVALIDARG);

        fileSelection->Release();
        folderSelection->Release();
        children[0]->Release();
        children[1]->Release();
        enumerator->Release();
        root->Release();
        factory->Release();
        library.unload();
    }

    void packageAndInstallerUseTheSameCommandIdentity()
    {
        const QDir source(QString::fromUtf8(SPANTUI_SOURCE_DIR));
        const auto read = [&source](const QString &relativePath) {
            QFile file(source.filePath(relativePath));
            if (!file.open(QIODevice::ReadOnly)) {
                return QByteArray{};
            }
            return file.readAll();
        };

        const QByteArray packageManifest = read(QStringLiteral(
            "installer/shell-extension/AppxManifest.xml.in"));
        const QByteArray applicationManifest = read(QStringLiteral("assets/brand/spantui.manifest"));
        const QByteArray installer = read(QStringLiteral("installer/SpanTui.iss"));
        const QByteArray registerScript = read(QStringLiteral(
            "installer/shell-extension/register.ps1"));
        const QByteArray machineTrustScript = read(QStringLiteral(
            "installer/shell-extension/trust-machine.ps1"));
        QVERIFY(!packageManifest.isEmpty());
        QVERIFY(!applicationManifest.isEmpty());
        QVERIFY(!installer.isEmpty());
        QVERIFY(!registerScript.isEmpty());
        QVERIFY(!machineTrustScript.isEmpty());

        constexpr auto commandId = "6D5B8A50-0F7C-4E91-A4D2-7E9B1C3F5210";
        QCOMPARE(packageManifest.count(commandId), 3);
        QVERIFY(packageManifest.contains("Path=\"SpanTui-shell-extension.dll\""));
        QVERIFY(packageManifest.contains("<desktop5:ItemType Type=\"Directory\">"));
        QVERIFY(packageManifest.contains("<desktop5:ItemType Type=\"Directory\\Background\">"));
        QVERIFY(packageManifest.contains("<uap10:AllowExternalContent>true"));
        QVERIFY(applicationManifest.contains("name=\"Span.SpanTui\""));
        QVERIFY(!applicationManifest.contains("<msix"));
        QVERIFY(applicationManifest.contains(
            "<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>"));
        QVERIFY(installer.contains("ExpandConstant('{app}\\shell\\')"));
        QVERIFY(installer.contains("ShellScriptParameters('register.ps1')"));
        QVERIFY(installer.contains("function InstallModernMenu"));
        QVERIFY(installer.contains("Version.Build >= 22000"));
        QVERIFY(registerScript.contains("Add-AppxPackage"));
        QVERIFY(registerScript.contains("-ExternalLocation"));
        QVERIFY(machineTrustScript.contains("Cert:\\LocalMachine\\TrustedPeople"));
        QVERIFY(machineTrustScript.contains("CertificateAuthority"));
        QVERIFY(machineTrustScript.contains("WindowsBuiltInRole]::Administrator"));
        QVERIFY(!machineTrustScript.contains("Cert:\\LocalMachine\\Root"));
    }

    void packagedActivationThroughWindowsComCatalog()
    {
        if (!qEnvironmentVariableIsSet("SPANTUI_TEST_PACKAGED_SHELL")) {
            QSKIP("Set SPANTUI_TEST_PACKAGED_SHELL after registering the sparse package.");
        }

        const HRESULT initialization = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QVERIFY(SUCCEEDED(initialization) || initialization == RPC_E_CHANGED_MODE);

        IExplorerCommand *root = nullptr;
        const HRESULT activation = CoCreateInstance(
            spantui::shell::kRootCommandClsid, nullptr, CLSCTX_ALL, IID_IExplorerCommand,
            reinterpret_cast<void **>(&root));
        QCOMPARE(activation, S_OK);
        QVERIFY(root);

        PWSTR title = nullptr;
        QCOMPARE(root->GetTitle(nullptr, &title), S_OK);
        QCOMPARE(takeString(title), QStringLiteral("SpanTui"));
        EXPCMDFLAGS flags = ECF_DEFAULT;
        QCOMPARE(root->GetFlags(&flags), S_OK);
        QVERIFY(flags & ECF_HASSUBCOMMANDS);
        root->Release();

        if (SUCCEEDED(initialization)) {
            CoUninitialize();
        }
    }
};

QTEST_GUILESS_MAIN(ShellExtensionContractTest)

#include "shell_extension_contract_test.moc"
