#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest prefix of s that is at most max bytes and ends on a UTF-8
 * character boundary. */
static size_t utf8_cut(const char *s, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

void proto_fmt_msg(sb_t *out, const cJSON *msg, size_t max_body)
{
    unsigned long long id = (unsigned long long)jnum(msg, "id", 0);
    unsigned long long re = (unsigned long long)jnum(msg, "re", 0);
    const char *from = jstr(msg, "from");
    const char *kind = jstr(msg, "kind");
    const char *to = jstr(msg, "to");
    const char *body = jstr(msg, "body");
    if (!from) from = "?";
    if (!kind) kind = "dm";
    if (!body) body = "";

    sb_addf(out, "<nuq id=\"%llu\" from=\"%s\" kind=\"%s\"", id, from, kind);
    if (!strcmp(kind, "topic") && to) sb_addf(out, " topic=\"%s\"", to);
    if (re) sb_addf(out, " re=\"%llu\"", re);
    sb_adds(out, ">\n");

    size_t len = strlen(body);
    size_t keep = max_body ? utf8_cut(body, len, max_body) : len;
    sb_add(out, body, keep);
    if (keep < len)
        sb_addf(out, "\n[truncated: %zu more bytes; full text: nuq show %llu]", len - keep, id);
    sb_adds(out, "\n</nuq>");

    if (!strcmp(kind, "work")) {
        sb_addf(out,
                "\nTask #%llu comes from the shared work queue and is now yours. When it is "
                "finished run: nuq done %llu -m 'one-line result' (to give it back: "
                "nuq fail %llu -m 'reason')",
                id, id, id);
    }
}

void proto_fmt_msgs(sb_t *out, const cJSON *msgs, size_t max_body)
{
    const cJSON *m;
    int first = 1;
    cJSON_ArrayForEach(m, msgs)
    {
        if (!first) sb_adds(out, "\n\n");
        first = 0;
        proto_fmt_msg(out, m, max_body);
    }
}

void proto_fmt_line(sb_t *out, const cJSON *msg, size_t max_body)
{
    unsigned long long id = (unsigned long long)jnum(msg, "id", 0);
    const char *from = jstr(msg, "from");
    const char *kind = jstr(msg, "kind");
    const char *to = jstr(msg, "to");
    const char *body = jstr(msg, "body");
    if (!from) from = "?";
    if (!kind) kind = "dm";
    if (!to) to = "";
    if (!body) body = "";

    sb_addf(out, "#%llu %s -> ", id, from);
    if (!strcmp(kind, "all"))
        sb_adds(out, "@all");
    else if (!strcmp(kind, "topic"))
        sb_addf(out, "topic %s", to);
    else if (!strcmp(kind, "work"))
        sb_adds(out, "work queue");
    else
        sb_addf(out, "@%s", to);
    sb_adds(out, ": ");

    /* First line only, so one message is always one line of output. */
    size_t len = strcspn(body, "\n");
    size_t keep = max_body ? utf8_cut(body, len, max_body) : len;
    sb_add(out, body, keep);
    if (keep < strlen(body)) sb_adds(out, " ...");
}

/* ---- edit statistics ---------------------------------------------------- */

static int count_lines(const char *s)
{
    if (!s || !*s) return 0;
    int n = 0;
    for (const char *c = s; *c; c++)
        if (*c == '\n') n++;
    return n + (s[strlen(s) - 1] != '\n');
}

/* Splits s into line spans. Caller frees *starts and *lens. */
static int split_lines(const char *s, const char ***starts, size_t **lens)
{
    int n = count_lines(s), i = 0;
    *starts = xcalloc((size_t)n + 1, sizeof **starts);
    *lens = xcalloc((size_t)n + 1, sizeof **lens);
    for (const char *c = s; c && *c && i < n; i++) {
        size_t len = strcspn(c, "\n");
        (*starts)[i] = c;
        (*lens)[i] = len;
        c += len + (c[len] == '\n');
    }
    return n;
}

/* Lines removed and added between old and new: lines outside their
 * longest common subsequence, as a diff would count them. Very large edits
 * fall back to ignoring only the lines shared at the start and the end. */
static void diff_count(const char *old, const char *new_, long *added, long *removed)
{
    const char **os, **ns;
    size_t *ol, *nl;
    int no = split_lines(old, &os, &ol), nn = split_lines(new_, &ns, &nl);
    int pre = 0, suf = 0;
    while (pre < no && pre < nn && ol[pre] == nl[pre] && !memcmp(os[pre], ns[pre], ol[pre])) pre++;
    while (suf < no - pre && suf < nn - pre && ol[no - 1 - suf] == nl[nn - 1 - suf] &&
           !memcmp(os[no - 1 - suf], ns[nn - 1 - suf], ol[no - 1 - suf]))
        suf++;
    int a = no - pre - suf, b = nn - pre - suf, lcs = 0;
    if (a > 0 && b > 0 && (long)a * b <= 4000000L) {
        int *prev = xcalloc((size_t)b + 1, sizeof *prev), *cur = xcalloc((size_t)b + 1, sizeof *cur);
        for (int i = 1; i <= a; i++) {
            for (int j = 1; j <= b; j++) {
                int oi = pre + i - 1, nj = pre + j - 1;
                if (ol[oi] == nl[nj] && !memcmp(os[oi], ns[nj], ol[oi]))
                    cur[j] = prev[j - 1] + 1;
                else
                    cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
            }
            int *t = prev;
            prev = cur;
            cur = t;
        }
        lcs = prev[b];
        free(prev);
        free(cur);
    }
    *removed += a - lcs;
    *added += b - lcs;
    free(os);
    free(ol);
    free(ns);
    free(nl);
}

static void add_stat(cJSON *out, const char *file, long added, long removed)
{
    if (!file || !*file || (added == 0 && removed == 0)) return;
    /* Merge with an earlier entry for the same file from this call. */
    cJSON *it;
    cJSON_ArrayForEach(it, out)
    {
        const char *f = jstr(it, "file");
        if (f && !strcmp(f, file)) {
            cJSON_ReplaceItemInObject(it, "added", cJSON_CreateNumber(jnum(it, "added", 0) + (double)added));
            cJSON_ReplaceItemInObject(it, "removed",
                                      cJSON_CreateNumber(jnum(it, "removed", 0) + (double)removed));
            return;
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "file", file);
    cJSON_AddNumberToObject(o, "added", (double)added);
    cJSON_AddNumberToObject(o, "removed", (double)removed);
    cJSON_AddItemToArray(out, o);
}

/* Codex's patch format:
 *   *** Begin Patch
 *   *** Update File: src/a.c     (or Add File / Delete File)
 *   @@ context
 *   -old line
 *   +new line
 *   *** End Patch */
static void patch_stats(const char *patch, cJSON *out)
{
    char file[1024] = "";
    long added = 0, removed = 0;
    for (const char *line = patch; *line;) {
        size_t len = strcspn(line, "\n");
        static const char *const heads[] = {"*** Update File: ", "*** Add File: ", "*** Delete File: "};
        int head = 0;
        for (int k = 0; k < 3; k++) {
            size_t hl = strlen(heads[k]);
            if (len > hl && !strncmp(line, heads[k], hl)) {
                add_stat(out, file, added, removed);
                added = removed = 0;
                snprintf(file, sizeof file, "%.*s", (int)(len - hl), line + hl);
                head = 1;
            }
        }
        if (!head && file[0] && strncmp(line, "***", 3)) {
            if (line[0] == '+' && strncmp(line, "+++", 3)) added++;
            if (line[0] == '-' && strncmp(line, "---", 3)) removed++;
        }
        line += len + (line[len] == '\n');
    }
    add_stat(out, file, added, removed);
}

/* The first string anywhere in v that holds a patch, or NULL. */
static const char *find_patch(const cJSON *v)
{
    if (cJSON_IsString(v)) return strstr(v->valuestring, "*** Begin Patch") ? v->valuestring : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, v)
    {
        const char *p = find_patch(it);
        if (p) return p;
    }
    return NULL;
}

/* One {old_string, new_string} replacement; Pi names them oldText, newText. */
static void replacement_count(const cJSON *r, long *added, long *removed)
{
    const char *old = jstr(r, "old_string"), *new_ = jstr(r, "new_string");
    if (!old) old = jstr(r, "oldText");
    if (!new_) new_ = jstr(r, "newText");
    if (old || new_) diff_count(old, new_, added, removed);
}

int proto_edit_stats(const char *tool, const cJSON *input, const cJSON *response, cJSON *out)
{
    int before = cJSON_GetArraySize(out);
    if (!tool) tool = "";
    const char *file = jstr(input, "file_path");
    if (!file) file = jstr(response, "filePath");
    if (!file) file = jstr(input, "path"); /* Pi */

    /* Claude reports the actual change it made. */
    const cJSON *patch = cJSON_GetObjectItemCaseSensitive(response, "structuredPatch");
    if (file && cJSON_IsArray(patch) && cJSON_GetArraySize(patch) > 0) {
        long added = 0, removed = 0;
        const cJSON *hunk, *line;
        cJSON_ArrayForEach(hunk, patch)
        {
            cJSON_ArrayForEach(line, cJSON_GetObjectItemCaseSensitive(hunk, "lines"))
            {
                if (!cJSON_IsString(line)) continue;
                if (line->valuestring[0] == '+') added++;
                if (line->valuestring[0] == '-') removed++;
            }
        }
        add_stat(out, file, added, removed);
    } else if (!strcmp(tool, "Write") || !strcmp(tool, "write")) {
        add_stat(out, file, count_lines(jstr(input, "content")), 0);
    } else if (!strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit") || !strcmp(tool, "edit")) {
        /* One replacement in the input itself (Claude's Edit), or a list of
         * them (MultiEdit, Cursor's afterFileEdit, the edit tool of Pi). */
        long added = 0, removed = 0;
        const cJSON *e;
        replacement_count(input, &added, &removed);
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(input, "edits"))
        replacement_count(e, &added, &removed);
        add_stat(out, file, added, removed);
    } else {
        const char *p = find_patch(input);
        if (p) patch_stats(p, out);
    }
    return cJSON_GetArraySize(out) - before;
}
