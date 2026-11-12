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
};

QTEST_GUILESS_MAIN(ShellExtensionContractTest)

#include "shell_extension_contract_test.moc"
