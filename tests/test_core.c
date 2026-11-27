/* Unit tests for the parts that need no broker and no tmux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "adapters.h"
#include "paths.h"
#include "proto.h"
#include "store.h"
#include "util.h"

static int g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                    \
        }                                                                  \
    } while (0)

static nq_agent *reg(nq_store *s, const char *name)
{
    char err[256];
    return st_register(s, name, "cli", NULL, NULL, MODE_PULL, 0, err, sizeof err);
}

static uint64_t post(nq_store *s, const char *kind, const char *from, const char *to, const char *body)
{
    char err[256] = "";
    return st_post(s, kind, from, to, body, 0, err, sizeof err);
}

static void test_sanitize(void)
{
    /* ESC, the paste-end sequence and 8-bit CSI must not survive. */
    char a[] = "ok\x1b[201~\rnext\tline\n\xc2\x9b" "31m\x7f.";
    sanitize_text(a);
    CHECK(!strcmp(a, "ok[201~next\tline\n31m."));
    char b[] = "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 \xf0\x9f\x9a\x80";
    char *copy = xstrdup(b);
    sanitize_text(b);
    CHECK(!strcmp(b, copy)); /* ordinary UTF-8 is untouched */
    free(copy);
}

static void test_sha256(void)
{
    char hex[65];
    sha256_hex("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256_hex("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* 56 bytes: the length no longer fits in the first padded block. */
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hex(two, strlen(two), hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_codex_hash(void)
{
    /* Hashes Codex itself accepted for these hook definitions. */
    char h[72];
    adapter_codex_hook_hash("interrupt", "hcom codex-interrupt", 1, NULL, h);
    CHECK(!strcmp(h, "sha256:fa064f86c9e5917b630302a7ddb76d1a283088b62d34a56f21ee53c0d3dc0fa3"));
    adapter_codex_hook_hash("post_tool_use", "hcom codex-posttooluse", 600, NULL, h);
    CHECK(!strcmp(h, "sha256:75ab36e4b0400de2228aa38851363d9a24ef13364e12d902ba78fa52109554e0"));
    adapter_codex_hook_hash("pre_tool_use", "hcom codex-pretooluse", 600, "Bash|apply_patch|spawn_agent", h);
    CHECK(!strcmp(h, "sha256:4ee448f34b5e361739b255ebdeeddb22f53b481f8cd7fb7ea84068b43d0b4032"));
}

static void edit_case(const char *json, const char *file, double added, double removed)
{
    cJSON *in = cJSON_Parse(json), *out = cJSON_CreateArray();
    CHECK(in != NULL);
    int n = proto_edit_stats(jstr(in, "tool_name"), cJSON_GetObjectItemCaseSensitive(in, "tool_input"),
                             cJSON_GetObjectItemCaseSensitive(in, "tool_response"), out);
    if (!file) {
        CHECK(n == 0);
    } else {
        cJSON *hit = NULL, *it;
        cJSON_ArrayForEach(it, out)
        {
            if (!strcmp(jstr(it, "file"), file)) hit = it;
        }
        CHECK(hit != NULL);
        if (hit && (jnum(hit, "added", -1) != added || jnum(hit, "removed", -1) != removed)) {
            fprintf(stderr, "  %s: got +%.0f -%.0f, want +%.0f -%.0f\n", file, jnum(hit, "added", -1),
                    jnum(hit, "removed", -1), added, removed);
            g_failed++;
        }
    }
    cJSON_Delete(in);
    cJSON_Delete(out);
}

static void test_edit_stats(void)
{
    /* Changed one line and appended one, as git counts it. */
    edit_case("{\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"/p/x.c\","
              "\"old_string\":\"a\\nb\\nc\",\"new_string\":\"a\\nB\\nc\\nd\"}}", "/p/x.c", 2, 1);
    edit_case("{\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"/p/n.c\",\"content\":\"1\\n2\\n3\\n\"}}",
              "/p/n.c", 3, 0);
    edit_case("{\"tool_name\":\"MultiEdit\",\"tool_input\":{\"file_path\":\"/p/m.c\",\"edits\":["
              "{\"old_string\":\"x\",\"new_string\":\"y\"},{\"old_string\":\"k\",\"new_string\":\"k\\nl\"}]}}",
              "/p/m.c", 2, 1);
    /* Claude's own patch wins over re-deriving it from the input. */
    edit_case("{\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"/p/s.c\",\"old_string\":\"q\",\"new_string\":\"r\"},"
              "\"tool_response\":{\"structuredPatch\":[{\"lines\":[\" ctx\",\"-q\",\"+r\",\"+r2\"]}]}}",
              "/p/s.c", 2, 1);
    /* Codex: a patch inside the tool input, two files. */
    const char *patch = "{\"tool_name\":\"apply_patch\",\"tool_input\":{\"command\":\"*** Begin Patch\\n"
                        "*** Update File: src/a.c\\n@@ fn\\n-old\\n+new\\n+more\\n"
                        "*** Add File: src/b.c\\n+one\\n+two\\n*** End Patch\\n\"}}";
    edit_case(patch, "src/a.c", 2, 1);
    edit_case(patch, "src/b.c", 2, 0);
    /* Pi: lower-case tools, "path", and a list of replacements. */
    edit_case("{\"tool_name\":\"write\",\"tool_input\":{\"path\":\"n.txt\",\"content\":\"1\\n2\\n\"}}",
              "n.txt", 2, 0);
    edit_case("{\"tool_name\":\"edit\",\"tool_input\":{\"path\":\"src/e.c\",\"edits\":["
              "{\"oldText\":\"x\",\"newText\":\"y\"},{\"oldText\":\"k\",\"newText\":\"k\\nl\"}]}}",
              "src/e.c", 2, 1);
    edit_case("{\"tool_name\":\"read\",\"tool_input\":{\"path\":\"src/e.c\"}}", NULL, 0, 0);
    /* Devin: lower-case tools with Claude's field names. */
    edit_case("{\"tool_name\":\"edit\",\"tool_input\":{\"file_path\":\"src/d.c\","
              "\"old_string\":\"a\\nb\",\"new_string\":\"a\\nB\\nc\"}}", "src/d.c", 2, 1);
    edit_case("{\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls\"}}", NULL, 0, 0);
}

static void test_names(void)
{
    CHECK(valid_name("claude-1"));
    CHECK(valid_name("fake_agent.sh"));
    CHECK(!valid_name(""));
    CHECK(!valid_name("-x"));
    CHECK(!valid_name("a b"));
    CHECK(!valid_name("a/b"));
    CHECK(!valid_name("abcdefghijklmnopqrstuvwxyz0123456789"));
}

static void test_routing(void)
{
    nq_store s;
    st_init(&s);
    nq_agent *alice = reg(&s, "alice");
    reg(&s, "bob");
    reg(&s, "carol");
    CHECK(alice && s.n_agents == 3);

    /* Direct: only the named mailbox. '@' and duplicates are tolerated. */
    CHECK(post(&s, "dm", "bob", "@alice,alice", "hi") == 1);
    CHECK(st_find(&s, "alice")->n_inbox == 1);
    CHECK(st_find(&s, "carol")->n_inbox == 0);

    /* Unknown recipient is an error and nothing is stored. */
    char err[256] = "";
    CHECK(st_post(&s, "dm", "bob", "dave", "hi", 0, err, sizeof err) == 0);
    CHECK(strstr(err, "no agent named 'dave'") && strstr(err, "alice"));
    CHECK(s.n_msgs == 1);

    /* Broadcast: everyone but the sender. */
    CHECK(post(&s, "dm", "bob", "@all", "all hands") == 2);
    CHECK(st_find(&s, "alice")->n_inbox == 2);
    CHECK(st_find(&s, "bob")->n_inbox == 0);
    CHECK(st_find(&s, "carol")->n_inbox == 1);

    /* Topic: subscribers only, never the publisher. */
    CHECK(st_sub(&s, "carol", "review", 1) == 0);
    CHECK(st_sub(&s, "bob", "review", 1) == 0);
    CHECK(post(&s, "topic", "bob", "review", "PR 12") == 3);
    CHECK(st_find(&s, "carol")->n_inbox == 2);
    CHECK(st_find(&s, "alice")->n_inbox == 2);
    CHECK(st_find(&s, "bob")->n_inbox == 0);
    CHECK(st_sub(&s, "carol", "review", 0) == 0);
    post(&s, "topic", "bob", "review", "PR 13");
    CHECK(st_find(&s, "carol")->n_inbox == 2);

    /* Delivery removes from the front, in order. */
    st_take_inbox(&s, st_find(&s, "alice"), 1);
    CHECK(st_find(&s, "alice")->n_inbox == 1 && st_find(&s, "alice")->inbox[0] == 2);

    CHECK(post(&s, "dm", "bob", "alice", "") == 0);
    st_free(&s);
}

static void test_work(void)
{
    nq_store s;
    char err[256];
    st_init(&s);
    nq_agent *alice = reg(&s, "alice");
    reg(&s, "bob");
    uint64_t t1 = post(&s, "work", "bob", NULL, "first");
    uint64_t t2 = post(&s, "work", "bob", NULL, "second");
    CHECK(st_count_work(&s, W_QUEUED) == 2);
    CHECK(alice->n_inbox == 0); /* tasks go to nobody until claimed */

    nq_msg *m = st_claim(&s, alice, 0);
    CHECK(m && m->id == t1 && m->wstate == W_CLAIMED); /* FIFO */
    CHECK(st_claimed_by(&s, "alice") == m);

    /* Only the claimant can finish it, and only once. */
    CHECK(st_finish(&s, "bob", t1, 1, NULL, err, sizeof err) < 0);
    CHECK(st_finish(&s, "alice", t2, 1, NULL, err, sizeof err) < 0);
    CHECK(st_finish(&s, "alice", t1, 1, "ok", err, sizeof err) == 0);
    CHECK(st_msg(&s, t1)->wstate == W_DONE && !strcmp(st_msg(&s, t1)->note, "ok"));
    CHECK(st_finish(&s, "alice", t1, 1, NULL, err, sizeof err) < 0);

    /* A claimant that disappears gives its task back to the queue. */
    m = st_claim(&s, alice, 1);
    CHECK(m && m->id == t2 && alice->n_inbox == 1);
    st_unregister(&s, "alice");
    CHECK(st_msg(&s, t2)->wstate == W_QUEUED && st_msg(&s, t2)->claimant == NULL);
    CHECK(st_find(&s, "alice") == NULL);
    m = st_claim(&s, st_find(&s, "bob"), 0);
    CHECK(m && m->id == t2);
    CHECK(st_claim(&s, st_find(&s, "bob"), 0) == NULL);
    st_free(&s);
}

static void test_autonames(void)
{
    nq_store s;
    char err[256];
    st_init(&s);
    nq_agent *a = st_register(&s, NULL, "claude", NULL, NULL, MODE_WAKE, 0, err, sizeof err);
    CHECK(a && !strcmp(a->name, "claude-1") && a->state == ST_STARTING);
    a = st_register(&s, NULL, "claude", NULL, NULL, MODE_WAKE, 0, err, sizeof err);
    CHECK(a && !strcmp(a->name, "claude-2"));
    CHECK(st_register(&s, "claude-1", "claude", NULL, NULL, MODE_WAKE, 0, err, sizeof err) == NULL);
    CHECK(st_register(&s, "all", "cli", NULL, NULL, MODE_PULL, 0, err, sizeof err) == NULL);
    /* The pane wrapper re-registers the reserved name with its pane. */
    a = st_register(&s, "claude-1", "claude", "%7", "run-a", MODE_WAKE, 1, err, sizeof err);
    CHECK(a && !strcmp(a->pane, "%7"));
    /* Requests from an earlier run of the same name are told apart. */
    CHECK(st_token_ok(a, "run-a") && st_token_ok(a, NULL) && !st_token_ok(a, "run-old"));
    st_unregister(&s, "claude-1");
    a = st_register(&s, NULL, "claude", NULL, NULL, MODE_WAKE, 0, err, sizeof err);
    CHECK(a && !strcmp(a->name, "claude-1")); /* freed names are reused */
    st_free(&s);
}

static void test_replay(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/log.jsonl", dir);
    unlink(path);

    nq_store s;
    char err[256];
    st_init(&s);
    CHECK(st_open_journal(&s, path) == 0);
    reg(&s, "alice");
    reg(&s, "bob");
    reg(&s, "carol");
    st_sub(&s, "carol", "build", 1);
    post(&s, "dm", "bob", "alice", "one\ntwo \"quoted\" \\ back");
    post(&s, "dm", "bob", "all", "broadcast");
    post(&s, "topic", "alice", "build", "red");
    uint64_t t = post(&s, "work", "bob", NULL, "task");
    uint64_t t2 = post(&s, "work", "bob", NULL, "task2");
    st_take_inbox(&s, st_find(&s, "alice"), 1);
    st_claim(&s, st_find(&s, "carol"), 1);
    st_finish(&s, "carol", t, 0, "cannot", err, sizeof err);
    st_claim(&s, st_find(&s, "alice"), 0);
    st_edit(&s, "carol", "src/a.c", 5, 2);
    st_edit(&s, "carol", "src/a.c", 1, 0);
    st_unregister(&s, "bob");
    size_t alice_n = st_find(&s, "alice")->n_inbox, carol_n = st_find(&s, "carol")->n_inbox;
    st_free(&s);

    /* A crash can leave half a record at the end; it must not hide the
     * records before it or corrupt the ones written afterwards. */
    FILE *f = fopen(path, "a");
    fputs("{\"t\":\"msg\",\"from\":\"bo", f);
    fclose(f);

    nq_store r;
    st_init(&r);
    CHECK(st_replay(&r, path) > 0);
    CHECK(r.n_msgs == 5);
    CHECK(st_find(&r, "bob") == NULL);
    CHECK(st_find(&r, "alice") && st_find(&r, "alice")->n_inbox == alice_n);
    CHECK(st_find(&r, "carol") && st_find(&r, "carol")->n_inbox == carol_n);
    CHECK(st_find(&r, "carol")->n_subs == 1);
    CHECK(st_find(&r, "carol")->added == 6 && st_find(&r, "carol")->removed == 2);
    CHECK(st_find(&r, "carol")->n_files == 1 && !strcmp(st_find(&r, "carol")->files[0].path, "src/a.c"));
    CHECK(!strcmp(st_msg(&r, 1)->body, "one\ntwo \"quoted\" \\ back"));
    CHECK(st_msg(&r, t)->wstate == W_FAILED && !strcmp(st_msg(&r, t)->note, "cannot"));
    CHECK(st_msg(&r, t2)->wstate == W_CLAIMED && !strcmp(st_msg(&r, t2)->claimant, "alice"));

    CHECK(st_open_journal(&r, path) == 0);
    post(&r, "dm", "alice", "carol", "after restart");
    st_free(&r);

    nq_store again;
    st_init(&again);
    st_replay(&again, path);
    CHECK(again.n_msgs == 6 && !strcmp(st_msg(&again, 6)->body, "after restart"));
    st_free(&again);
    unlink(path);
}

static void test_format(void)
{
    nq_store s;
    st_init(&s);
    reg(&s, "alice");
    reg(&s, "bob");
    post(&s, "dm", "bob", "alice", "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 and more text");
    post(&s, "work", "bob", NULL, "do it");

    cJSON *m = st_msg_json(st_msg(&s, 1));
    sb_t out = {0};
    proto_fmt_msg(&out, m, 0);
    CHECK(strstr(out.p, "<nuq id=\"1\" from=\"bob\" kind=\"dm\">\n"));
    CHECK(strstr(out.p, "\n</nuq>"));
    sb_free(&out);

    /* Cutting must not split a multi-byte character. */
    proto_fmt_msg(&out, m, 5);
    CHECK(strstr(out.p, ">\nza\xc5\xbc\n[truncated:"));
    CHECK(strstr(out.p, "nuq show 1"));
    sb_free(&out);
    cJSON_Delete(m);

    m = st_msg_json(st_msg(&s, 2));
    proto_fmt_msg(&out, m, 0);
    CHECK(strstr(out.p, "nuq done 2"));
    sb_free(&out);
    proto_fmt_line(&out, m, 0);
    CHECK(!strcmp(out.p, "#2 bob -> work queue: do it"));
    sb_free(&out);
    cJSON_Delete(m);
    st_free(&s);
}

static void test_paths(const char *dir)
{
    nq_paths a, b;
    char err[256];
    CHECK(paths_init(&a, dir, err, sizeof err) == 0);
    CHECK(paths_init(&b, "/", err, sizeof err) == 0);
    CHECK(strlen(a.grid) == 8 && strcmp(a.grid, b.grid));
    CHECK(!strncmp(a.rpc_url, "ipc://", 6));
    /* Unix socket paths are limited to about 104 bytes. */
    CHECK(strlen(a.ev_url) - 6 < 104);
    CHECK(!strncmp(a.session, "nq-", 3) && !strchr(a.session, '.') && !strchr(a.session, ':'));
    CHECK(paths_init(&b, "/no/such/dir", err, sizeof err) < 0);
#ifdef _WIN32
    /* The broker's addresses are named pipes: one flat name, no path. */
    CHECK(!strpbrk(a.rpc_url + 6, "/\\:") && !strpbrk(a.ev_url + 6, "/\\:"));
    CHECK(strcmp(a.rpc_url, a.ev_url));
    /* Any spelling of the directory is the same grid: backslashes, a
     * lower-case drive letter, other letter case, a trailing separator. */
    char alt[PATH_MAX];
    snprintf(alt, sizeof alt, "%s\\", a.dir);
    for (char *q = alt; *q; q++) {
        if (*q == '/') *q = '\\';
        else if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    }
    CHECK(paths_init(&b, alt, err, sizeof err) == 0);
    CHECK(!strcmp(a.grid, b.grid) && !strcmp(a.dir, b.dir) && !strcmp(a.rpc_url, b.rpc_url));
    /* The stored form: absolute, forward slashes, upper-case drive. */
    CHECK(a.dir[0] >= 'A' && a.dir[0] <= 'Z' && a.dir[1] == ':' && a.dir[2] == '/' && !strchr(a.dir, '\\'));
#endif
}

static void test_compat(const char *dir)
{
    CHECK(nq_is_abs("/x"));
    CHECK(!nq_is_abs("x/y") && !nq_is_abs(""));
    CHECK(!strcmp(nq_basename("a/b/c"), "c") && !strcmp(nq_basename("c"), "c"));
    char real[PATH_MAX];
    CHECK(nq_realpath(dir, real, sizeof real) == 0 && nq_is_abs(real));
#ifdef _WIN32
    CHECK(nq_is_abs("C:/x") && nq_is_abs("c:\\x") && nq_is_abs("//server/share/x"));
    CHECK(!nq_is_abs("C:x"));
    CHECK(!strcmp(nq_basename("C:\\a\\b.exe"), "b.exe") && !strcmp(nq_basename("a\\b/c"), "c"));
    char p[] = "c:\\Users\\x/proj";
    nq_path_norm(p);
    CHECK(!strcmp(p, "C:/Users/x/proj"));
    CHECK(nq_home() == NULL || (nq_is_abs(nq_home()) && !strchr(nq_home(), '\\')));

    /* One holder of the broker lock at a time, and the pid text written
     * through the holder stays readable by others. */
    char lock[PATH_MAX], text[32] = "";
    snprintf(lock, sizeof lock, "%s/test.lock", dir);
    unlink(lock);
    CHECK(nq_lock_probe(lock) == 0);
    int h = nq_lock_acquire(lock);
    CHECK(h >= 0);
    CHECK(nq_lock_write(h, "4242\n") >= 0);
    CHECK(nq_lock_probe(lock) == 1);
    CHECK(nq_lock_acquire(lock) < 0);
    FILE *f = fopen(lock, "rb");
    CHECK(f && fgets(text, sizeof text, f) && !strcmp(text, "4242\n"));
    if (f) fclose(f);
    nq_lock_release(h);
    CHECK(nq_lock_probe(lock) == 0);
    unlink(lock);
#endif
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_sanitize();
    test_edit_stats();
    test_sha256();
    test_codex_hash();
    test_names();
    test_routing();
    test_work();
    test_autonames();
    test_replay(dir);
    test_format();
    test_paths(dir);
    test_compat(dir);
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    puts("ok");
    return 0;
}
