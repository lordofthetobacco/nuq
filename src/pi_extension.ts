// The nuq side of a Pi session. nuq writes this file into the grid's runtime
// directory for one run of Pi and loads it with `pi -e`, behind a first line
// that defines NUQ, the path of the nuq binary. Pi's own configuration is
// not touched.
//
// It runs `nuq hook pi <event>` where Claude Code and Codex run their hooks,
// and gives Pi what the hook prints:
//
//   session_start       SessionStart      the introduction to the grid, kept in the system prompt
//   before_agent_start  UserPromptSubmit  waiting mail joins the run that starts
//   tool_result         PostToolUse       reports the tool and its edits; mail is steered into the run
//   agent_settled       Stop              waiting mail starts another run; without any, Pi is idle
//   while idle          Wait              exits 2 with the mail that arrived, which starts a run
//   ui_prompt_start     Notification      an extension's dialog waits for a person: blocked
//
// Mail reaches the model as a message of its own. Nothing is typed into the
// pane, so it cannot land in a draft or a dialog.
//
// Plain JavaScript on purpose: Pi loads it without type checking.
import { spawn } from "node:child_process";

const HOOK_TIMEOUT_MS = 10000;

export default function (pi) {
  // Outside a nuq pane there is no grid to talk to.
  if (!process.env.NUQ_AGENT) return;

  let intro = ""; // what SessionStart said about the grid
  let running = false; // between before_agent_start and agent_settled
  let waiter = null; // the `nuq hook pi Wait` process of an idle session
  let closed = false; // after session_shutdown; a new session gets a new instance of this
  let queue = Promise.resolve(); // hooks run one at a time, in event order

  // Runs `nuq hook pi <event>` with `input` as its JSON. Resolves to
  // { code, out, err } and never rejects: a failing hook must not fail Pi.
  function run(event, input, timeout, started) {
    return new Promise((resolve) => {
      let out = "";
      let err = "";
      let timer = null;
      const done = (code) => {
        if (timer) clearTimeout(timer);
        resolve({ code, out, err });
      };
      let child;
      try {
        child = spawn(NUQ, ["hook", "pi", event], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
      } catch {
        return done(-1);
      }
      if (started) started(child);
      if (timeout) timer = setTimeout(() => child.kill(), timeout);
      child.stdout.on("data", (chunk) => (out += chunk));
      child.stderr.on("data", (chunk) => (err += chunk));
      child.on("error", () => done(-1));
      child.on("close", (code) => done(code));
      child.stdin.on("error", () => {});
      child.stdin.end(JSON.stringify(input ?? {}));
    });
  }

  // The hook's JSON reply, or null when it printed none.
  function hook(event, input) {
    const reply = queue
      .then(() => run(event, input, HOOK_TIMEOUT_MS))
      .then((r) => {
        try {
          return JSON.parse(r.out);
        } catch {
          return null;
        }
      });
    queue = reply;
    return reply;
  }

  const context = (reply) => reply?.hookSpecificOutput?.additionalContext;

  // Idle, a message starts a run. During one it is delivered after the
  // current tool calls, before the next model request.
  function deliver(text) {
    if (!text) return;
    try {
      pi.sendMessage({ customType: "nuq", content: text, display: true }, { triggerTurn: true, deliverAs: "steer" });
    } catch {
      // The session is gone. The sender still has the message in `nuq show`.
    }
  }

  function stopWaiting() {
    const child = waiter;
    waiter = null;
    if (child) child.kill();
  }

  // Pi has nothing left to do. Mail (or a queued task) that is already
  // waiting starts the next run; otherwise a background hook waits for some.
  async function settle() {
    stopWaiting();
    if (closed) return;
    const reply = await hook("Stop");
    if (reply?.decision === "block") return deliver(reply.reason);
    // A waiter left behind would take mail that it can no longer hand over.
    if (running || closed) return;
    let child = null;
    run("Wait", {}, 0, (started) => {
      stopWaiting();
      child = waiter = started;
    }).then((r) => {
      if (waiter === child) waiter = null;
      // The waiter took this mail out of the inbox, so it is delivered even
      // if a run has started meanwhile.
      if (r.code === 2) deliver(r.err.trim());
    });
  }

  pi.on("session_start", async () => {
    running = false;
    intro = context(await hook("SessionStart")) ?? intro;
    await settle();
  });

  pi.on("before_agent_start", async (event) => {
    running = true;
    stopWaiting();
    if (intro) (event.systemPromptOptions.sections ??= {}).nuq_grid = intro;
    const text = context(await hook("UserPromptSubmit"));
    if (text) return { message: { customType: "nuq", content: text, display: true } };
  });

  pi.on("tool_result", async (event) => {
    const input = { tool_name: event.toolName };
    // A failed edit changed nothing.
    if (!event.isError) input.tool_input = event.input;
    deliver(context(await hook("PostToolUse", input)));
  });

  pi.on("agent_settled", async () => {
    running = false;
    await settle();
  });

  pi.on("ui_prompt_start", () => {
    hook("Notification", { notification_type: "permission_prompt" });
  });

  pi.on("ui_prompt_end", async () => {
    if (running) deliver(context(await hook("PostToolUse")));
    else await settle();
  });

  pi.on("session_shutdown", () => {
    closed = true;
    stopWaiting();
  });
}
