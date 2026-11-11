#ifdef _WIN32

#include <windows.h>

#include <shobjidl.h>

#include <array>
#include <atomic>
#include <new>
#include <string>
#include <vector>

#include "platform/windows/shell_extension_contract.h"

namespace {

enum class CommandKind {
    Root,
    OpenProject,
    OpenTerminal,
};

HINSTANCE g_module = nullptr;
std::atomic_ulong g_objectCount{0};
std::atomic_ulong g_serverLocks{0};

bool isChineseUi()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

HRESULT duplicateString(const std::wstring &value, PWSTR *result)
{
    if (!result) {
        return E_POINTER;
    }
    *result = nullptr;
    const auto bytes = (value.size() + 1) * sizeof(wchar_t);
    auto *copy = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!copy) {
        return E_OUTOFMEMORY;
    }
    memcpy(copy, value.c_str(), bytes);
    *result = copy;
    return S_OK;
}

std::wstring moduleDirectory()
{
    std::vector<wchar_t> path(260);
    for (;;) {
        const DWORD length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size() - 1) {
            std::wstring result(path.data(), length);
            const auto separator = result.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring{} : result.substr(0, separator);
        }
        path.resize(path.size() * 2);
    }
}

std::wstring applicationPath()
{
    const auto directory = moduleDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory + L"\\SpanTui.exe";
}

std::wstring quoteCommandLineArgument(const std::wstring &argument)
{
    std::wstring result;
    result.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

HRESULT selectedDirectory(IShellItemArray *selection, std::wstring *directory)
{
    if (!selection || !directory) {
        return E_INVALIDARG;
    }
    directory->clear();

    DWORD count = 0;
    HRESULT result = selection->GetCount(&count);
    if (FAILED(result) || count != 1) {
        return FAILED(result) ? result : E_INVALIDARG;
    }

    IShellItem *item = nullptr;
    result = selection->GetItemAt(0, &item);
    if (FAILED(result)) {
        return result;
    }

    SFGAOF attributes = 0;
    result = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
    if (SUCCEEDED(result)
        && (attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM))
            != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) {
        result = E_INVALIDARG;
    }

    PWSTR rawPath = nullptr;
    if (SUCCEEDED(result)) {
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    }
    if (SUCCEEDED(result) && rawPath) {
        *directory = rawPath;
    }
    CoTaskMemFree(rawPath);
    item->Release();
    return result;
}

HRESULT launchSpanTui(CommandKind kind, IShellItemArray *selection)
{
    std::wstring directory;
    HRESULT result = selectedDirectory(selection, &directory);
    if (FAILED(result)) {
        return result;
    }

    const std::wstring executable = applicationPath();
    if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::wstring commandLine = quoteCommandLineArgument(executable);
    if (kind == CommandKind::OpenProject) {
        commandLine += L" --project ";
    } else if (kind == CommandKind::OpenTerminal) {
        commandLine += L" --terminal --cwd ";
    } else {
        return E_INVALIDARG;
    }
    commandLine += quoteCommandLineArgument(directory);

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, directory.c_str(), &startup, &process)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return S_OK;
}

class ExplorerCommand final : public IExplorerCommand
{
public:
    explicit ExplorerCommand(CommandKind kind)
        : m_kind(kind)
    {
        ++g_objectCount;
    }

    IFACEMETHODIMP QueryInterface(REFIID interfaceId, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(interfaceId, IID_IUnknown) || IsEqualIID(interfaceId, IID_IExplorerCommand)) {
            *object = static_cast<IExplorerCommand *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_references; }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG references = --m_references;
        if (references == 0) {
            delete this;
        }
        return references;
    }

    IFACEMETHODIMP GetTitle(IShellItemArray *, PWSTR *title) override
    {
        switch (m_kind) {
        case CommandKind::Root:
            return duplicateString(L"SpanTui", title);
        case CommandKind::OpenProject:
            return duplicateString(isChineseUi() ? L"\u5728 SpanTui \u4e2d\u6253\u5f00" : L"Open in SpanTui", title);
        case CommandKind::OpenTerminal:
            return duplicateString(isChineseUi() ? L"\u5728 SpanTui \u7ec8\u7aef\u4e2d\u6253\u5f00"
                                                 : L"Open in SpanTui Terminal",
                                   title);
        }
        return E_UNEXPECTED;
    }

    IFACEMETHODIMP GetIcon(IShellItemArray *, PWSTR *icon) override
    {
        const std::wstring executable = applicationPath();
        return executable.empty() ? E_FAIL : duplicateString(executable, icon);
    }

    IFACEMETHODIMP GetToolTip(IShellItemArray *, PWSTR *toolTip) override
    {
        if (!toolTip) {
            return E_POINTER;
        }
        *toolTip = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetCanonicalName(GUID *canonicalName) override
    {
        if (!canonicalName) {
            return E_POINTER;
        }
        switch (m_kind) {
        case CommandKind::Root:
            *canonicalName = spantui::shell::kRootCanonicalName;
            break;
        case CommandKind::OpenProject:
            *canonicalName = spantui::shell::kOpenProjectCanonicalName;
            break;
        case CommandKind::OpenTerminal:
            *canonicalName = spantui::shell::kOpenTerminalCanonicalName;
            break;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray *, BOOL, EXPCMDSTATE *state) override
    {
        if (!state) {
            return E_POINTER;
        }
        *state = ECS_ENABLED;
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray *selection, IBindCtx *) override
    {
        return m_kind == CommandKind::Root ? E_NOTIMPL : launchSpanTui(m_kind, selection);
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS *flags) override
    {
        if (!flags) {
            return E_POINTER;
        }
        *flags = m_kind == CommandKind::Root ? ECF_HASSUBCOMMANDS : ECF_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand **commands) override;

private:
    ~ExplorerCommand() { --g_objectCount; }

    std::atomic_ulong m_references{1};
    CommandKind m_kind;
};

class ExplorerCommandEnumerator final : public IEnumExplorerCommand
{
public:
    ExplorerCommandEnumerator() { ++g_objectCount; }

    IFACEMETHODIMP QueryInterface(REFIID interfaceId, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(interfaceId, IID_IUnknown)
            || IsEqualIID(interfaceId, IID_IEnumExplorerCommand)) {
            *object = static_cast<IEnumExplorerCommand *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_references; }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG references = --m_references;
        if (references == 0) {
            delete this;
        }
        return references;
    }

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand **commands, ULONG *fetched) override
    {
        if (!commands || (count != 1 && !fetched)) {
            return E_POINTER;
        }
        ULONG produced = 0;
        constexpr std::array<CommandKind, 2> kinds = {CommandKind::OpenProject,
                                                       CommandKind::OpenTerminal};
        while (produced < count && m_index < kinds.size()) {
            auto *command = new (std::nothrow) ExplorerCommand(kinds[m_index++]);
            if (!command) {
                if (fetched) {
                    *fetched = produced;
                }
                return E_OUTOFMEMORY;
            }
            commands[produced++] = command;
        }
        if (fetched) {
            *fetched = produced;
        }
        return produced == count ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Skip(ULONG count) override
    {
        const size_t remaining = 2 - m_index;
        if (count > remaining) {
            m_index = 2;
            return S_FALSE;
        }
        m_index += count;
        return S_OK;
    }

    IFACEMETHODIMP Reset() override
    {
        m_index = 0;
        return S_OK;
    }

    IFACEMETHODIMP Clone(IEnumExplorerCommand **result) override
    {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        auto *clone = new (std::nothrow) ExplorerCommandEnumerator;
        if (!clone) {
            return E_OUTOFMEMORY;
        }
        clone->m_index = m_index;
        *result = clone;
        return S_OK;
    }

private:
    ~ExplorerCommandEnumerator() { --g_objectCount; }

    std::atomic_ulong m_references{1};
    size_t m_index = 0;
};

HRESULT ExplorerCommand::EnumSubCommands(IEnumExplorerCommand **commands)
{
    if (!commands) {
        return E_POINTER;
    }
    *commands = nullptr;
    if (m_kind != CommandKind::Root) {
        return E_NOTIMPL;
    }
    auto *enumerator = new (std::nothrow) ExplorerCommandEnumerator;
    if (!enumerator) {
        return E_OUTOFMEMORY;
    }
    *commands = enumerator;
    return S_OK;
}

class ExplorerCommandClassFactory final : public IClassFactory
{
public:
    ExplorerCommandClassFactory() { ++g_objectCount; }

    IFACEMETHODIMP QueryInterface(REFIID interfaceId, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(interfaceId, IID_IUnknown) || IsEqualIID(interfaceId, IID_IClassFactory)) {
            *object = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_references; }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG references = --m_references;
        if (references == 0) {
            delete this;
        }
        return references;
    }

    IFACEMETHODIMP CreateInstance(IUnknown *outer, REFIID interfaceId, void **object) override
    {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        auto *command = new (std::nothrow) ExplorerCommand(CommandKind::Root);
        if (!command) {
            return E_OUTOFMEMORY;
        }
        const HRESULT result = command->QueryInterface(interfaceId, object);
        command->Release();
        return result;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) {
            ++g_serverLocks;
        } else if (g_serverLocks.load() > 0) {
            --g_serverLocks;
        }
        return S_OK;
    }

private:
    ~ExplorerCommandClassFactory() { --g_objectCount; }

    std::atomic_ulong m_references{1};
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID classId, REFIID interfaceId, void **object)
{
    if (!IsEqualCLSID(classId, spantui::shell::kRootCommandClsid)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto *factory = new (std::nothrow) ExplorerCommandClassFactory;
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT result = factory->QueryInterface(interfaceId, object);
    factory->Release();
    return result;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return g_objectCount.load() == 0 && g_serverLocks.load() == 0 ? S_OK : S_FALSE;
}

#endif
