#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "cli.h"
#include "daemon.h"
#include "tmux.h"
#include "util.h"

/* `nuq doctor`, `nuq completion` and `nuq skills`. */

static int check(int ok, const char *what, const char *detail)
{
    printf("  %s  %-22s %s\n", ok ? "ok  " : "FAIL", what, detail ? detail : "");
    return ok ? 0 : 1;
}

int cmd_doctor(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char buf[PATH_MAX], err[512], line[PATH_MAX + 128];
    int bad = 0;

    printf("nuq %s\n", NQ_VERSION);

    sb_t ver = {0};
    const char *tv[] = {"tmux", "-V", NULL};
    run_cmd(tv, NULL, 0, &ver, 0);
    if (ver.p) ver.p[strcspn(ver.p, "\n")] = '\0';
    if (tmux_check(err, sizeof err) == 0)
        bad += check(1, "tmux", ver.p);
    else
        bad += check(0, "tmux", err);
    sb_free(&ver);

    char self[PATH_MAX] = "";
    self_exe(self, sizeof self);
    if (find_on_path("nuq", buf, sizeof buf) == 0) {
        bad += check(1, "nuq on PATH", buf);
    } else {
        snprintf(line, sizeof line, "not on PATH (agents get %s added automatically)", self);
        check(1, "nuq on PATH", line);
    }

    /* Agents are optional: report, do not fail. */
    static const char *const agents[] = {"claude", "codex", NULL};
    for (int i = 0; agents[i]; i++) {
        if (find_on_path(agents[i], buf, sizeof buf) == 0)
            check(1, agents[i], buf);
        else
            printf("  --    %-22s not installed\n", agents[i]);
    }

    if (paths_mkdirs(&c->p, err, sizeof err) == 0)
        bad += check(1, "runtime directory", c->p.rundir);
    else
        bad += check(0, "runtime directory", err);

    const char *shell = getenv("SHELL");
    check(1, "shell", shell ? shell : "(SHELL is not set)");

    if (daemon_alive(&c->p)) {
        cJSON *r = cli_rpc(c, cli_req("ping"), 1000);
        if (r && jbool(r, "ok")) {
            snprintf(line, sizeof line, "running, pid %.0f", jnum(r, "pid", 0));
            bad += check(1, "broker for this dir", line);
        } else {
            bad += check(0, "broker for this dir", "holds the lock but does not answer");
        }
        cJSON_Delete(r);
    } else {
        printf("  --    %-22s not running (starts with the first agent)\n", "broker for this dir");
    }
    printf("  --    %-22s %s\n", "grid directory", c->p.dir);
    printf("  --    %-22s %s\n", "journal", c->p.journal);
    printf("  --    %-22s %s\n", "broker log", c->p.logfile);
    return bad ? 1 : 0;
}

/* Kept free of bash 4 features: macOS still ships bash 3.2. */
static const char BASH_COMPLETION[] =
    "# nuq completion for bash. Load with:  eval \"$(nuq completion bash)\"\n"
    "_nuq() {\n"
    "    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
    "    local cmds=\"claude codex run send pub sub unsub push take done fail inbox listen show ls tail work attach kill down up doctor completion skills version help\"\n"
    "    if [ \"$COMP_CWORD\" -eq 1 ]; then\n"
    "        COMPREPLY=($(compgen -W \"$cmds\" -- \"$cur\"))\n"
    "        return\n"
    "    fi\n"
    "    case \"${COMP_WORDS[1]}\" in\n"
    "    send)\n"
    "        if [ \"$COMP_CWORD\" -eq 2 ]; then\n"
    "            local names\n"
    "            names=$(nuq ls 2>/dev/null | awk 'NR > 1 && NF >= 6 { print \"@\" $1 }')\n"
    "            COMPREPLY=($(compgen -W \"@all $names\" -- \"$cur\"))\n"
    "        fi\n"
    "        ;;\n"
    "    kill)\n"
    "        local names\n"
    "        names=$(nuq ls 2>/dev/null | awk 'NR > 1 && NF >= 6 && $6 != \"-\" { print $1 }')\n"
    "        COMPREPLY=($(compgen -W \"$names\" -- \"$cur\"))\n"
    "        ;;\n"
    "    completion)\n"
    "        COMPREPLY=($(compgen -W \"bash zsh\" -- \"$cur\"))\n"
    "        ;;\n"
    "    skills)\n"
    "        COMPREPLY=($(compgen -W \"status install show\" -- \"$cur\"))\n"
    "        ;;\n"
    "    inbox)\n"
    "        COMPREPLY=($(compgen -W \"--peek\" -- \"$cur\"))\n"
    "        ;;\n"
    "    work)\n"
    "        COMPREPLY=($(compgen -W \"--all\" -- \"$cur\"))\n"
    "        ;;\n"
    "    esac\n"
    "}\n"
    "complete -F _nuq nuq\n";

static const char ZSH_COMPLETION[] =
    "#compdef nuq\n"
    "# nuq completion for zsh. Load with:  eval \"$(nuq completion zsh)\"\n"
    "# or save as _nuq in a directory on $fpath.\n"
    "_nuq() {\n"
    "    local -a cmds names\n"
    "    cmds=(\n"
    "        'claude:start Claude Code in the grid'\n"
    "        'codex:start Codex in the grid'\n"
    "        'run:start any command as an agent'\n"
    "        'send:send to @name, several, or @all'\n"
    "        'pub:publish to a topic'\n"
    "        'sub:subscribe to a topic'\n"
    "        'unsub:leave a topic'\n"
    "        'push:add a task to the work queue'\n"
    "        'take:claim the next task'\n"
    "        'done:finish a task'\n"
    "        'fail:give a task back as failed'\n"
    "        'inbox:read your messages'\n"
    "        'listen:wait for the next message'\n"
    "        'show:show one message in full'\n"
    "        'ls:agents and their state'\n"
    "        'tail:live message log'\n"
    "        'work:open tasks'\n"
    "        'attach:open the tmux grid'\n"
    "        'kill:stop one agent'\n"
    "        'down:stop the whole grid'\n"
    "        'up:start only the broker'\n"
    "        'doctor:check the installation'\n"
    "        'completion:print a completion script'\n"
    "        'skills:install the nuq skill for your agents'\n"
    "        'version:print the version'\n"
    "    )\n"
    "    if (( CURRENT == 2 )); then\n"
    "        _describe 'command' cmds\n"
    "        return\n"
    "    fi\n"
    "    case \"${words[2]}\" in\n"
    "    send)\n"
    "        if (( CURRENT == 3 )); then\n"
    "            names=(@all ${(f)\"$(nuq ls 2>/dev/null | awk 'NR > 1 && NF >= 6 { print \"@\" $1 }')\"})\n"
    "            compadd -a names\n"
    "        fi\n"
    "        ;;\n"
    "    kill)\n"
    "        names=(${(f)\"$(nuq ls 2>/dev/null | awk 'NR > 1 && NF >= 6 && $6 != \"-\" { print $1 }')\"})\n"
    "        compadd -a names\n"
    "        ;;\n"
    "    completion) compadd bash zsh ;;\n"
    "    skills) compadd status install show ;;\n"
    "    inbox) compadd -- --peek ;;\n"
    "    work) compadd -- --all ;;\n"
    "    claude|codex|run) _normal ;;\n"
    "    esac\n"
    "}\n"
    "if [ \"${funcstack[1]}\" = _nuq ]; then\n"
    "    _nuq \"$@\"\n"
    "else\n"
    "    compdef _nuq nuq\n"
    "fi\n";

int cmd_completion(nq_ctx *c, int argc, char **argv)
{
    (void)c;
    if (argc == 1 && !strcmp(argv[0], "bash")) {
        fputs(BASH_COMPLETION, stdout);
        return 0;
    }
    if (argc == 1 && !strcmp(argv[0], "zsh")) {
        fputs(ZSH_COMPLETION, stdout);
        return 0;
    }
    die("usage: nuq completion bash|zsh");
}

/* ---- nuq skills ------------------------------------------------------- */

/* skills/nuq/SKILL.md, compiled in (see cmake/EmbedFile.cmake). */
extern const unsigned char nq_skill_md[];
extern const size_t nq_skill_md_len;

enum { SKILL_MISSING, SKILL_CURRENT, SKILL_DIFFERENT };

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Each agent keeps personal skills in <its config dir>/skills/<name>/SKILL.md.
 * An agent counts as present when its config directory exists; the
 * environment variable is the agent's own way of moving that directory. */
static const struct {
    const char *agent, *env, *dotdir;
} SKILL_HOMES[] = {
    {"Claude Code", "CLAUDE_CONFIG_DIR", ".claude"},
    {"Codex", "CODEX_HOME", ".codex"},
};
#define N_SKILL_HOMES (sizeof SKILL_HOMES / sizeof SKILL_HOMES[0])

/* The skills directory of agent i, or NULL if that agent is not set up. */
static char *skills_dir_of(size_t i)
{
    sb_t root = {0};
    const char *over = getenv(SKILL_HOMES[i].env), *home = getenv("HOME");
    if (over && *over)
        sb_adds(&root, over);
    else if (home && *home)
        sb_addf(&root, "%s/%s", home, SKILL_HOMES[i].dotdir);
    else
        return NULL;
    if (!is_dir(root.p)) {
        sb_free(&root);
        return NULL;
    }
    sb_adds(&root, "/skills");
    return root.p;
}

static int skill_state(const char *file)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0) return SKILL_MISSING;
    size_t len = 0;
    char *have = read_fd_all(fd, 16 * 1024 * 1024, &len);
    close(fd);
    int same = have && len == nq_skill_md_len && !memcmp(have, nq_skill_md, len);
    free(have);
    return same ? SKILL_CURRENT : SKILL_DIFFERENT;
}

/* Writes the skill to <skills_dir>/nuq/SKILL.md. A copy that differs (an
 * older version, or one edited by hand) is kept as SKILL.md.bak. */
static int install_skill(const char *label, const char *skills_dir)
{
    sb_t dir = {0}, file = {0}, tmp = {0}, bak = {0};
    sb_addf(&dir, "%s/nuq", skills_dir);
    sb_addf(&file, "%s/SKILL.md", dir.p);
    sb_addf(&tmp, "%s/SKILL.md.tmp", dir.p);
    sb_addf(&bak, "%s/SKILL.md.bak", dir.p);

    int rc = 0;
    const char *result = "installed";
    char why[256] = "";
    int before = skill_state(file.p);
    if (before == SKILL_CURRENT) {
        result = "up to date";
    } else if (mkdir_p(dir.p, 0755) < 0) {
        snprintf(why, sizeof why, "cannot create %s: %s", dir.p, strerror(errno));
        rc = -1;
    } else {
        /* Write beside the target and rename, so a reader never sees half. */
        int fd = open(tmp.p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        size_t off = 0;
        while (fd >= 0 && off < nq_skill_md_len) {
            ssize_t n = write(fd, nq_skill_md + off, nq_skill_md_len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        if (fd < 0 || off != nq_skill_md_len || close(fd) < 0) {
            snprintf(why, sizeof why, "cannot write %s: %s", tmp.p, strerror(errno));
            unlink(tmp.p);
            rc = -1;
        } else {
            if (before == SKILL_DIFFERENT && rename(file.p, bak.p) == 0)
                result = "updated (previous copy kept as SKILL.md.bak)";
            if (rename(tmp.p, file.p) < 0) {
                snprintf(why, sizeof why, "cannot write %s: %s", file.p, strerror(errno));
                unlink(tmp.p);
                rc = -1;
            }
        }
    }
    if (rc == 0)
        printf("  %-12s %s  %s\n", label, file.p, result);
    else
        printf("  %-12s FAILED: %s\n", label, why);
    sb_free(&dir);
    sb_free(&file);
    sb_free(&tmp);
    sb_free(&bak);
    return rc;
}

int cmd_skills(nq_ctx *c, int argc, char **argv)
{
    (void)c;
    const char *sub = argc > 0 ? argv[0] : "status";

    if (!strcmp(sub, "show")) {
        fwrite(nq_skill_md, 1, nq_skill_md_len, stdout);
        return 0;
    }

    if (!strcmp(sub, "status")) {
        size_t found = 0;
        for (size_t i = 0; i < N_SKILL_HOMES; i++) {
            char *dir = skills_dir_of(i);
            if (!dir) {
                printf("  %-12s not set up on this machine\n", SKILL_HOMES[i].agent);
                continue;
            }
            found++;
            sb_t file = {0};
            sb_addf(&file, "%s/nuq/SKILL.md", dir);
            static const char *const words[] = {"not installed", "installed, current",
                                                "installed, differs from this nuq's copy"};
            printf("  %-12s %s  %s\n", SKILL_HOMES[i].agent, file.p, words[skill_state(file.p)]);
            sb_free(&file);
            free(dir);
        }
        if (found) puts("install or update with: nuq skills install");
        return 0;
    }

    if (!strcmp(sub, "install")) {
        int failed = 0, done = 0;
        /* --dir DIR (repeatable): install there instead of detecting. */
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--dir") || i + 1 >= argc)
                die("usage: nuq skills install [--dir SKILLS_DIR]...");
            failed += install_skill("--dir", argv[++i]) < 0;
            done++;
        }
        if (!done) {
            for (size_t i = 0; i < N_SKILL_HOMES; i++) {
                char *dir = skills_dir_of(i);
                if (!dir) continue;
                failed += install_skill(SKILL_HOMES[i].agent, dir) < 0;
                done++;
                free(dir);
            }
        }
        if (!done) {
            nq_warn("no agent configuration found (looked for ~/.claude and ~/.codex)\n"
                    "  to install somewhere else: nuq skills install --dir SKILLS_DIR");
            return 1;
        }
        if (!failed) puts("agents pick the skill up in their next session");
        return failed ? 1 : 0;
    }

    die("usage: nuq skills [status | install [--dir SKILLS_DIR]... | show]");
}
