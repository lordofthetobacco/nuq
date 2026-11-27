// Stands in for Pi in tests/hooks.sh: loads the extension nuq generated and
// plays Pi's extension runtime for it, one step per argument.
//
//   node pi_host.mjs EXTENSION STEP...
//
//   EVENT[:JSON]   fire that event (session_start, tool_result:{...}) and
//                  wait for its handlers
//   send:TEXT      a person (dana) sends TEXT to this agent
//   wait_message   wait until the extension calls pi.sendMessage()
//
// Prints one line per thing the extension did. Needs NQ and NUQ_AGENT.
import { spawnSync } from "node:child_process";
import { pathToFileURL } from "node:url";

const [file, ...steps] = process.argv.slice(2);
const handlers = {};
const sent = [];
let printed = 0;

const pi = {
  on(event, handler) {
    (handlers[event] ??= []).push(handler);
  },
  sendMessage(message, options) {
    sent.push(`sendMessage triggerTurn=${options?.triggerTurn} deliverAs=${options?.deliverAs} ${JSON.stringify(message.content)}`);
  },
};

function flush() {
  for (; printed < sent.length; printed++) console.log(sent[printed]);
}

async function emit(name, event) {
  for (const handler of handlers[name] ?? []) {
    const result = await handler(event, {});
    if (name === "before_agent_start") {
      const intro = event.systemPromptOptions.sections.nuq_grid ?? "";
      console.log(`before_agent_start intro=${JSON.stringify(intro.split("\n")[0])} message=${JSON.stringify(result?.message?.content ?? null)}`);
    }
  }
  flush();
}

(await import(pathToFileURL(file).href)).default(pi);

for (const step of steps) {
  const colon = step.indexOf(":");
  const name = colon < 0 ? step : step.slice(0, colon);
  const arg = colon < 0 ? "" : step.slice(colon + 1);
  if (name === "send") {
    const env = { ...process.env, NUQ_AGENT: "dana" };
    spawnSync(process.env.NQ, ["send", `@${process.env.NUQ_AGENT}`, "--", arg], { env, stdio: "ignore" });
  } else if (name === "wait_message") {
    for (let i = 0; i < 150 && printed === sent.length; i++) await new Promise((r) => setTimeout(r, 100));
    if (printed === sent.length) console.log("wait_message timed out");
    flush();
  } else {
    const event = arg ? JSON.parse(arg) : {};
    if (name === "before_agent_start") event.systemPromptOptions = { sections: {} };
    await emit(name, event);
  }
}
await emit("session_shutdown", { reason: "quit" });
