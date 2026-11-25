#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "daemon.h"
#include "util.h"

static const char USAGE[] =
    "nuq " NQ_VERSION " - a tmux grid of coding agents joined by a message queue\n"
    "\n"
    "  nuq            (no arguments) the nuq UI for this directory's grid:\n"
    "                   live dashboard; :takeover N shows an agent, F12 or Ctrl-q comes back\n"
    "  nuq dash --once  the same, printed once as text\n"
    "\n"
    "Start agents (one grid per directory):\n"
    "  nuq [-n NAME] [--sub TOPICS] <agent> [args...]   e.g. nuq claude, nuq codex\n"
    "  nuq run [--] <command> [args...]                 same, for a command named like a subcommand\n"
    "      -n NAME      name in the grid (default: <agent>-<n>)\n"
    "      --sub a,b    subscribe the agent to topics\n"
    "      --inject     paste messages into an agent nuq has no hooks for\n"
    "      -d           do not attach to the grid\n"
    "\n"
    "Messages:\n"
    "  nuq send @NAME[,@NAME...]|@all [-r ID] [--] TEXT   direct or broadcast\n"
    "  nuq pub TOPIC [--] TEXT        to a topic      nuq sub|unsub TOPIC\n"
    "  nuq push [--] TEXT             queue a task    nuq take | done ID [-m NOTE] | fail ID\n"
    "  nuq inbox [--peek]             read your messages\n"
    "  nuq listen [-t SECONDS]        wait for the next message\n"
    "  nuq show ID                    one message in full\n"
    "  TEXT may also come from stdin.\n"
    "\n"
    "The grid:\n"
    "  nuq ls         agents and their state     nuq tail      live message log\n"
    "  nuq work       open tasks (--all: every)  nuq attach    raw tmux (F12 leaves)\n"
    "  nuq kill NAME  stop one agent             nuq down      stop the whole grid\n"
    "  nuq up         start only the broker      nuq doctor    check the installation\n"
    "  nuq completion bash|zsh                   nuq version\n"
    "  nuq skills install   give your agents the nuq skill (nuq skills: where it is installed)\n"
    "\n"
    "  -C DIR           act on the grid of DIR instead of the current directory\n"
    "\n"
    "Default arguments per agent go in ~/.config/nuq/config, one per line:\n"
    "  claude.args = --model opus\n";

typedef int (*cmd_fn)(nq_ctx *, int, char **);

static const struct {
    const char *name;
    cmd_fn fn;
} COMMANDS[] = {
    {"up", cmd_up},         {"down", cmd_down},     {"send", cmd_send},   {"pub", cmd_pub},
    {"sub", cmd_sub},       {"unsub", cmd_unsub},   {"push", cmd_push},   {"take", cmd_take},
    {"done", cmd_done},     {"fail", cmd_fail},     {"inbox", cmd_inbox}, {"show", cmd_show},
    {"listen", cmd_listen}, {"ls", cmd_ls},         {"tail", cmd_tail},   {"work", cmd_work},
    {"kill", cmd_kill},     {"attach", cmd_attach}, {"_pane", cmd_pane},  {"hook", cmd_hook},
    {"doctor", cmd_doctor}, {"completion", cmd_completion}, {"skills", cmd_skills}, {"_layout", cmd_layout}, {"dash", cmd_dash},
};

int main(int argc, char **argv)
{
    const char *dir = NULL;
    nq_launch_opts opts = {0};
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-C") && i + 1 < argc)
            dir = argv[++i];
        else if ((!strcmp(a, "-n") || !strcmp(a, "--name")) && i + 1 < argc)
            opts.name = argv[++i];
        else if (!strcmp(a, "--sub") && i + 1 < argc)
            opts.subs = argv[++i];
        else if (!strcmp(a, "--inject"))
            opts.inject = 1;
        else if (!strcmp(a, "-d") || !strcmp(a, "--detach"))
            opts.detach = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            puts("nuq " NQ_VERSION);
            return 0;
        } else if (a[0] == '-')
            die("unknown option '%s' (see: nuq --help)", a);
        else
            break;
    }
    /* Bare `nuq` in a terminal opens the dashboard. */
    const char *cmd = i < argc ? argv[i] : "dash";
    char **rest = i < argc ? argv + i + 1 : argv + argc;
    int nrest = i < argc ? argc - i - 1 : 0;
    if (i >= argc && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fputs(USAGE, stderr);
        return 2;
    }

    if (!strcmp(cmd, "version")) {
        puts("nuq " NQ_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "help")) {
        fputs(USAGE, stdout);
        return 0;
    }

    nq_ctx ctx;
    char err[512];
    int is_hook = !strcmp(cmd, "hook");
    if (paths_init(&ctx.p, dir, err, sizeof err) < 0) {
        if (is_hook) return 0; /* a hook must never fail its agent */
        die("%s", err);
    }
    ctx.me = getenv("NUQ_AGENT");
    if (!ctx.me || !valid_name(ctx.me)) ctx.me = getenv("USER");
    if (!ctx.me || !valid_name(ctx.me)) ctx.me = "human";

    if (!strcmp(cmd, "daemon")) return cmd_daemon(&ctx.p);
    for (size_t k = 0; k < sizeof COMMANDS / sizeof COMMANDS[0]; k++)
        if (!strcmp(cmd, COMMANDS[k].name)) return COMMANDS[k].fn(&ctx, nrest, rest);

    if (!strcmp(cmd, "run")) {
        if (nrest > 0 && !strcmp(rest[0], "--")) {
            rest++;
            nrest--;
        }
        return cmd_launch(&ctx, &opts, nrest, rest);
    }
    return cmd_launch(&ctx, &opts, argc - i, argv + i);
}
