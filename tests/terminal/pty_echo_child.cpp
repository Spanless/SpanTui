#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>

namespace {
std::atomic_bool g_interrupted{false};

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}

void ensureProcessedInput() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        SetConsoleMode(input, mode | ENABLE_PROCESSED_INPUT);
    }
}

// Echoed lines go through the UTF-16 console API. Narrow reads and writes are
// translated through the console code page, which cannot hold CJK on an
// English system (CP 437), so committed IME text would come back as '?'.
bool readLine(std::string &line) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode))
        return static_cast<bool>(std::getline(std::cin, line));
    std::wstring wide;
    wchar_t character = 0;
    DWORD count = 0;
    while (ReadConsoleW(input, &character, 1, &count, nullptr) && count == 1) {
        if (character == L'\n')
            break;
        wide.push_back(character);
    }
    if (wide.empty() && character != L'\n')
        return false;
    line.assign(WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()),
                                    nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), line.data(),
                        int(line.size()), nullptr, nullptr);
    return true;
}

void writeText(const std::string &text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) {
        std::cout << text << std::flush;
        return;
    }
    std::cout << std::flush;
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()),
                                          nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(),
                        int(wide.size()));
    DWORD written = 0;
    WriteConsoleW(output, wide.data(), DWORD(wide.size()), &written, nullptr);
}
} // namespace

int main(int argc, char **argv) {
    // Console stdout is unbuffered: every operator<< became its own write (the
    // CRT even splits off the first byte), and conhost forwards each write
    // through ConPTY as a separate render. Under CPU contention each of those
    // round trips took ~170 ms, so the 40-line reply took over 30 s and
    // session tests timed out. Every reply ends with std::flush, which now
    // hands the whole reply to the console at once.
    std::setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
    if (argc > 1 && std::string(argv[1]) == "interrupt-bytes") {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode))
            return 60;
        const DWORD rawMode = (originalMode
            & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
            | ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(input, rawMode))
            return 61;
        std::cout << "\r\nspan_interrupt_bytes_ready\r\n" << std::flush;
        std::string received;
        char buffer[64];
        while (received.find(char(0x03)) == std::string::npos) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                return 62;
            received.append(buffer, buffer + count);
            if (received == "\x1b[200~")
                std::cout << "\r\nspan_paste_begin_received\r\n" << std::flush;
        }
        const char *digits = "0123456789abcdef";
        std::string hex;
        for (unsigned char byte : received) {
            hex += digits[byte >> 4];
            hex += digits[byte & 15];
        }
        SetConsoleMode(input, originalMode);
        std::cout << "\r\nspan_interrupt_bytes=" << hex << "\r\n" << std::flush;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "shortcut-matrix") {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode))
            return 40;
        DWORD rawMode = originalMode;
        rawMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(input, rawMode))
            return 41;

        std::cout << "span_shortcut_matrix_ready\r\n" << std::flush;
        const std::string expected =
            "\x1b\r\x1b[13;5u\x1b[112;6u\x1b\x01@";
        std::string received;
        char buffer[64];
        while (received.find(expected) == std::string::npos) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                return 42;
            received.append(buffer, buffer + count);
        }
        received.clear();
        std::cout << "\r\nspan_shortcut_legacy_ok\r\n"
                     "\x1b[=3u"
                     "span_shortcut_kitty_ready\r\n" << std::flush;
        const std::string kittyExpected =
            "\x1b[13;2:1u\x1b[13;2:3u";
        while (received.find(kittyExpected) == std::string::npos) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                return 43;
            received.append(buffer, buffer + count);
        }
        SetConsoleMode(input, originalMode);
        std::cout << "\r\nspan_shortcut_matrix_ok\r\n" << std::flush;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "backtab-tui") {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode))
            return 30;
        DWORD rawMode = originalMode;
        rawMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(input, rawMode))
            return 31;

        std::cout << "span_backtab_tui_ready\r\n" << std::flush;
        const std::string expected = "\x1b[Z\x1b[Z";
        std::string received;
        char buffer[64];
        while (received.find(expected) == std::string::npos) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                return 32;
            received.append(buffer, buffer + count);
        }
        SetConsoleMode(input, originalMode);
        std::cout << "\r\nspan_backtab_sequence_ok\r\n" << std::flush;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "kitty-tui") {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode))
            return 20;
        DWORD rawMode = originalMode;
        rawMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(input, rawMode))
            return 21;

        // Ask for disambiguated keys plus event types, matching OpenCode's
        // press/release path. Deliberately exit without restoring Kitty mode:
        // Ctrl+C can terminate a TUI before its cleanup handler runs.
        std::cout << "\x1b[?1049h\x1b[2J\x1b[H\x1b[=3u"
                     "span_kitty_tui_ready\r\n" << std::flush;
        std::string received;
        char buffer[64];
        while (received.find("\x1b[99;5:1u") == std::string::npos) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                return 22;
            received.append(buffer, buffer + count);
        }
        SetConsoleMode(input, originalMode);
        std::cout << "\x1b[?1049l" << std::flush;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "raw-tui") {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode))
            return 10;
        DWORD rawMode = originalMode;
        rawMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(input, rawMode))
            return 11;

        std::cout << "\x1b[?1049h\x1b[2J\x1b[Hspan_raw_tui_ready\r\n" << std::flush;
        char buffer[64];
        bool interrupted = false;
        while (!interrupted) {
            DWORD count = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0)
                break;
            interrupted = std::find(buffer, buffer + count, char(0x03)) != buffer + count;
        }
        SetConsoleMode(input, originalMode);
        std::cout << "\x1b[?1049l" << std::flush;
        return interrupted ? 0 : 12;
    }
    ensureProcessedInput();
    DWORD outputMode = 0;
    const bool attachedToConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &outputMode);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    char workingDirectory[MAX_PATH]{};
    GetCurrentDirectoryA(MAX_PATH, workingDirectory);

    const bool silentSceneStart = argc > 1
        && std::string(argv[1]) == "scene-switch";
    if (!silentSceneStart) {
        std::cout << "\r\n";
        std::cout << "span_child_ready\r\n";
        std::cout << "span_child_console=" << (attachedToConsole ? "1" : "0") << "\r\n";
        std::cout << "span_child_cwd=" << workingDirectory << "\r\n" << std::flush;
    }

    std::string line;
    while (readLine(line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "size") {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
                const int columns = info.srWindow.Right - info.srWindow.Left + 1;
                const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
                std::cout << "span_child_size=" << columns << "x" << rows << "\r\n" << std::flush;
            } else {
                std::cout << "span_child_size=error\r\n" << std::flush;
            }
            continue;
        }
        if (line == "burst") {
            const std::string chunk(4092, 'x');
            for (int index = 0; index < 256; ++index) {
                std::cout << chunk << "\r\n";
            }
            std::cout << "span_child_burst_complete\r\n" << std::flush;
            continue;
        }
        if (line == "lines") {
            for (int index = 0; index < 40; ++index) {
                std::cout << "span_child_line=" << index << "\r\n";
            }
            std::cout << "span_child_lines_complete\r\n" << std::flush;
            continue;
        }
        if (line == "stream") {
            // Deliberately keep the process alive after several independently
            // flushed redraw chunks. The session test uses this to prove that
            // an intermediate frame is visible before the command exits.
            for (int index = 0; index < 4; ++index) {
                std::cout << "\x1b[?2026h\x1b[2J\x1b[Hspan_child_stream_frame=" << index
                          << "\r\n\x1b[?2026l" << std::flush;
                Sleep(120);
            }
            std::cout << "span_child_stream_running\r\n" << std::flush;
            continue;
        }
        if (line == "ime-cursor") {
            // Match the Kimi/pi-tui failure mechanism exactly: one native
            // write contains the synchronized redraw, a reverse-video
            // software caret, the final cursor move, and cursor hiding.
            // ConPTY can preserve the rendered cells while losing the final
            // hidden cursor column in the bytes observed by its client.
            const std::string redraw =
                "\x1b[?2026h\x1b[2J\x1b[H"
                "span_ime_redraw\x1b[2;3H\x1b[7m \x1b[27m"
                "\x1b[4;11Hrender-tail\x1b[?2026l"
                "\x1b[2A\x1b[3G\x1b[?25l";
            HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (!GetConsoleMode(output, &mode)
                || !SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                return 49;
            }
            DWORD written = 0;
            if (!WriteFile(output, redraw.data(), DWORD(redraw.size()), &written, nullptr)
                || written != redraw.size()) {
                return 50;
            }
            continue;
        }
        if (line == "fragmented-sync-cursor") {
            // Separate native writes force ConPTY read fragmentation. Inbox
            // ConPTY does not forward the 2026 markers, so the host may see
            // intermediate renders; only the final cursor is guaranteed.
            const std::array<std::string, 4> chunks = {
                "\x1b[?2026h\x1b[2J\x1b[2;2HWorking",
                "\x1b[6;4Hspan_atomic_final",
                "\x1b[10;3Hgpt-5.6-sol",
                "\x1b[?25h\x1b[6;4H\x1b[?2026l",
            };
            HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
            for (const std::string &chunk : chunks) {
                DWORD written = 0;
                if (!WriteFile(output, chunk.data(), DWORD(chunk.size()),
                               &written, nullptr)
                    || written != chunk.size()) {
                    return 51;
                }
                Sleep(35);
            }
            continue;
        }
        if (line == "scene-first") {
            // Twenty colored cells that must not survive a restart. Set the
            // title as an ordering fence without adding more cells.
            std::cout << "\x1b]0;span_scene_first\x07"
                         "\x1b[2J\x1b[H\x1b[6;1H"
                         "\x1b[42m                    \x1b[0m\x1b[H"
                      << std::flush;
            continue;
        }
        if (line == "open-sync") {
            // Leave synchronized-update deliberately unmatched. Restart tests
            // verify that a dead transport cannot poison the next shell.
            std::cout << "\x1b[?2026h\x1b[2J\x1b[Hspan_child_open_sync\r\n" << std::flush;
            continue;
        }
        if (line == "alternate") {
            std::cout << "\x1b[?1049h\x1b[2J\x1b[Hspan_child_alternate\r\n" << std::flush;
            continue;
        }
        if (line == "link-one" || line == "link-two") {
            const std::string url = "https://span.test/" + line;
            std::cout << "\x1b]8;;" << url << "\x1b\\" << line
                      << "\x1b]8;;\x1b\\\r\n" << std::flush;
            continue;
        }
        if (line == "bare-link") {
            std::cout << "span_bare_url=http://localhost:5173/\r\n" << std::flush;
            continue;
        }
        if (line == "block") {
            // Wait for CTRL_C_EVENT (from ConPTY 0x03 synthesis or host event).
            ensureProcessedInput();
            g_interrupted.store(false);
            std::cout << "span_child_blocking\r\n" << std::flush;
            for (int tick = 0; tick < 300 && !g_interrupted.load(); ++tick) {
                Sleep(100);
            }
            if (g_interrupted.load()) {
                std::cout << "span_child_interrupted\r\n" << std::flush;
            } else {
                std::cout << "span_child_block_timeout\r\n" << std::flush;
            }
            continue;
        }
        writeText("span_child_echo=" + line + "\r\n");
        if (line == "exit") {
            return 7;
        }
    }
    return 8;
}
