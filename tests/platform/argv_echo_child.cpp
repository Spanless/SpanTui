// Writes the arguments it receives, as Windows parses its command line, to
// argv.txt in its working directory: UTF-8, one per line. Tests launch it in
// place of a real program to observe the exact command line that was built.
#include <windows.h>
#include <shellapi.h>

#include <string>

int main()
{
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return 1;
    }
    std::wstring text;
    for (int i = 1; i < count; ++i) {
        if (i > 1) {
            text += L'\n';
        }
        text += arguments[i];
    }
    LocalFree(arguments);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);

    // Written aside and renamed, so a reader never sees a partial file.
    HANDLE file = CreateFileW(L"argv.tmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 2;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || !MoveFileExW(L"argv.tmp", L"argv.txt", MOVEFILE_REPLACE_EXISTING)) {
        return 3;
    }
    return 0;
}
