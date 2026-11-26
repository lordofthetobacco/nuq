/* fake_agent.sh for Windows, where a pane cannot run a shell script: the
 * same line-mode stand-in for a coding agent, in the console's ordinary
 * cooked mode (line editing and echo by the console). See fake_agent.sh.
 *
 *   FAKE_AGENT_LOG     file that receives the lines (required)
 *   FAKE_AGENT_DIALOG  if set, the console does not echo, like an agent
 *                      showing a permission dialog
 *   FAKE_ARGS_LOG      if set, the command line is appended to it, because
 *                      the tests have no `ps` to read it from (see fake_tui.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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

int main(int argc, char **argv)
{
    const char *log = getenv("FAKE_AGENT_LOG");
    if (!log) {
        fputs("FAKE_AGENT_LOG must be set\n", stderr);
        return 2;
    }
    log_args(argc, argv);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    if (getenv("FAKE_AGENT_DIALOG") && *getenv("FAKE_AGENT_DIALOG")) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(in, &mode)) SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        printf("Allow this command? [y/N] ");
    } else {
        char cwd[MAX_PATH];
        const char *who = getenv("NUQ_AGENT");
        DWORD n = GetCurrentDirectoryA(sizeof cwd, cwd);
        if (n == 0 || n >= sizeof cwd) strcpy(cwd, "?");
        /* The tests compare it with the forward-slash form nuq uses. */
        for (char *p = cwd; *p; p++)
            if (*p == '\\') *p = '/';
        printf("fake agent %s ready in %s\n> ", who && *who ? who : "?", cwd);
    }
    fflush(stdout);

    char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        FILE *f = fopen(log, "ab");
        if (f) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
        if (!strcmp(line, "/exit")) return 0;
        printf("> ");
        fflush(stdout);
    }
    return 0;
}
