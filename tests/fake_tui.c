/* A stand-in for a full-screen coding agent, for the tests.
 *
 * Like the real ones it puts the terminal in raw mode and turns on
 * bracketed paste, so a paste arrives as one block and only a real Enter
 * key submits it.
 *
 *   fake_tui            pasted or typed text collects in an input line and
 *                       is appended to $FAKE_AGENT_LOG when Enter is pressed
 *   fake_tui --dialog   behaves like a permission dialog: pastes are
 *                       ignored and never shown; a bare Enter writes
 *                       "APPROVED" to the log. nuq must never cause that.
 *
 * On Windows the console is put in virtual-terminal input mode, which is
 * what a raw terminal is there: keys and pastes arrive as VT bytes, Ctrl-C
 * as 0x03, and nothing is echoed. Windows has no `ps` to read command lines
 * from, so if $FAKE_ARGS_LOG is set the program appends its own (program
 * path without .exe, then the arguments) to that file for the tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static HANDLE g_in, g_out;
static DWORD g_in_mode, g_out_mode;
static UINT g_in_cp, g_out_cp;

static int raw_mode(void)
{
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_in, &g_in_mode) || !GetConsoleMode(g_out, &g_out_mode)) return -1;
    g_in_cp = GetConsoleCP();
    g_out_cp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(g_in, ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(g_out, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                              DISABLE_NEWLINE_AUTO_RETURN);
    return 0;
}

static void restore(void)
{
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    SetConsoleMode(g_in, g_in_mode);
    SetConsoleMode(g_out, g_out_mode);
    SetConsoleCP(g_in_cp);
    SetConsoleOutputCP(g_out_cp);
}

static int read_byte(unsigned char *ch)
{
    DWORD n = 0;
    return ReadFile(g_in, ch, 1, &n, NULL) && n == 1;
}

static void log_args(int argc, char **argv)
{
    const char *path = getenv("FAKE_ARGS_LOG");
    char self[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, self, sizeof self);
    if (!path || n == 0 || n >= sizeof self) return;
    for (char *p = self; *p; p++)
        if (*p == '\\') *p = '/';
    if (n > 4 && !_stricmp(self + n - 4, ".exe")) self[n - 4] = '\0';
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fputs(self, f);
    for (int i = 1; i < argc; i++)
        fprintf(f, " %s", argv[i]);
    fputc('\n', f);
    fclose(f);
}
#else
static struct termios g_saved;

static int raw_mode(void)
{
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0) return -1;
    raw = g_saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    return 0;
}

static void restore(void)
{
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

static int read_byte(unsigned char *ch)
{
    return read(STDIN_FILENO, ch, 1) == 1;
}

static void log_args(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}
#endif

static void record(const char *path, const char *text)
{
    /* Binary, so Windows writes the same bytes as POSIX. */
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s\n", text);
    fclose(f);
}

int main(int argc, char **argv)
{
    int dialog = argc > 1 && !strcmp(argv[1], "--dialog");
    const char *log = getenv("FAKE_AGENT_LOG");
    if (!log) {
        fputs("FAKE_AGENT_LOG must be set\n", stderr);
        return 2;
    }

    log_args(argc, argv);
    if (raw_mode() < 0) return 2;
    atexit(restore);
    fputs("\033[?2004h", stdout);
    if (dialog)
        fputs("Allow the agent to run: rm -rf build ?\r\n  [Enter] Yes   [n] No\r\n", stdout);
    else
        fputs("fake tui ready\r\n> ", stdout);
    fflush(stdout);

    static const char OPEN[] = "\033[200~", CLOSE[] = "\033[201~";
    char input[65536];
    size_t len = 0;
    char seq[8];
    size_t seqlen = 0;
    int pasting = 0;

    for (;;) {
        unsigned char ch;
        if (!read_byte(&ch)) break;

        /* Recognise the paste brackets; both start with ESC. */
        if (seqlen > 0 || ch == 0x1b) {
            seq[seqlen++] = (char)ch;
            const char *want = pasting ? CLOSE : OPEN;
            if (strncmp(seq, want, seqlen) != 0) {
                seqlen = 0; /* some other escape sequence: drop it */
                continue;
            }
            if (seqlen == 6) {
                pasting = !pasting;
                seqlen = 0;
            }
            continue;
        }

        if (dialog) {
            if (pasting) continue;
            if (ch == '\r' || ch == '\n') {
                record(log, "APPROVED");
                fputs("approved\r\n", stdout);
                fflush(stdout);
            } else if (ch == 'n' || ch == 3) {
                break;
            }
            continue;
        }

        if (!pasting && (ch == '\r' || ch == '\n')) {
            input[len] = '\0';
            record(log, input);
            if (!strcmp(input, "/exit")) break;
            len = 0;
            fputs("\r\n> ", stdout);
            fflush(stdout);
            continue;
        }
        if (ch == 3) break; /* Ctrl-C */
        if (ch == '\r') ch = '\n';
        if (len < sizeof input - 1) input[len++] = (char)ch;
        if (ch == '\n')
            fputs("\r\n", stdout);
        else
            fputc(ch, stdout);
        fflush(stdout);
    }
    return 0;
}
