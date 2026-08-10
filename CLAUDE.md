# CLAUDE.md

Adapted from https://github.com/jbarbier/CLAUDE.md — trimmed to fit this repo (embedded ESP32 C++ framework, not a services/microservices app) and reconciled with this org's safety rules (no auto-commit/push without confirmation).

## Read AGENT_INSTRUCTIONS.md first

`AGENT_INSTRUCTIONS.md` is the source of truth for every module: lifecycle (`begin/loop/enable/disable/isEnabled`), module independence, GPIO ownership, memory rules (no `new`/`delete`/`malloc`, no `delay()`), `enum class` + Result-enum error handling, folder structure (`Module/README.md` + `instruction.md`). Read it before touching any module. Then read that module's `README.md` and `instruction*.md` — on conflict, `instruction-partX.md` > `instruction.md` > `README.md` > current implementation.

## How to work

Ship the complete thing, not a plan to build it. Don't offer to "table this for later" when the real fix is reachable. Don't leave a dangling thread when finishing it takes five more minutes. Don't present a workaround when the real fix exists.

Tests passing is not understanding. Before calling anything done, be able to say why the code is correct and where it would break — for firmware that means: what happens on brownout, wrong credentials, GPIO conflict, or a dropped WiFi/MQTT connection.

## Latent vs deterministic work

If the same input always has the same correct answer, it's deterministic — write it as code, don't reason it out per-request. Judgment, ambiguous trade-offs, and open design questions are the only things worth spending model reasoning on.

## Search before building

1. Is there an existing manager or established embedded pattern that already does this? Use it.
2. Is there a well-supported library (PlatformIO/Arduino registry) with real traction? Evaluate it.
3. Only write custom code if 1 and 2 genuinely don't fit — and say why.

Don't recreate what an existing manager in this repo already solves. Don't add a library dependency for something `PreferencesManager`/`EventBus`/etc. already provide.

## Tests and validation

- Every module change should be validated against its `instruction.md` before being called done.
- Every public function validates its inputs and returns a `Result` enum — never crashes, never assumes valid input (per `AGENT_INSTRUCTIONS.md`).
- New modules (e.g. `ConfigStore`, `CaptivePortalManager`) need a `README.md` + `instruction.md` — don't skip this even for "quick" additions.
- This is embedded firmware with no CI/unit-test harness today — validation means: compiles, matches the documented API/lifecycle, and the failure modes (bad creds, GPIO conflicts, reconnect storms) have been reasoned through explicitly.

## Confusion protocol

Stop and ask when you hit:
- Two plausible architectures for the same requirement.
- A request that contradicts `AGENT_INSTRUCTIONS.md` or an established manager pattern.
- A destructive operation with unclear scope.
- Missing context that would materially change the approach.

Name the ambiguity in one sentence, present 2-3 real options with trade-offs, ask. Don't guess on architecture. Routine changes don't need this.

## Completion status

At the end of a task, state one of:
- **DONE** — completed, you can explain why it's correct and where it'd break.
- **DONE_WITH_CONCERNS** — completed, but list each concern + severity + proposed follow-up.
- **BLOCKED** — state what's blocking and what was tried.
- **NEEDS_CONTEXT** — state exactly what's missing.

"Partially done" isn't a status.

## Git — ask first, always

- Never commit or push without the user explicitly asking, every time — one approval doesn't carry over to the next change.
- Never use `--no-verify`, force-push, or `reset --hard` without explicit instruction.
- Never commit secrets — this repo already gitignores `secrets.h`; keep using the `.example` template pattern for new sketches.
- State what you're about to do before any commit/push and wait for confirmation.

## Tone

Direct, short, concrete. Reference exact file:line, not vague descriptions. No filler preamble. State what's broken plainly. End with the next action, not a recap of what already happened.
