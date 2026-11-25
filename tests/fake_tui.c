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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;

static void restore(void)
{
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

static void record(const char *path, const char *text)
{
    FILE *f = fopen(path, "a");
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

    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0) return 2;
    raw = g_saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
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
        if (read(STDIN_FILENO, &ch, 1) != 1) break;

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
