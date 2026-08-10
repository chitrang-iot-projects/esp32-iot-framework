# Agent Instructions

## Purpose

This is the primary operating guide for AI agents working in this repository. Detailed project knowledge belongs in `docs/`.

## Core Principles

- Understand before changing; prefer evidence over assumptions.
- Keep changes focused and preserve existing behavior unless the task requires otherwise.
- Follow established project conventions and patterns.
- Avoid unnecessary refactoring, dependencies, duplication, and invented behavior.
- Keep documentation concise, useful, and valuable to future agents.

## Before Starting Work

1. Read this file and `docs/README.md`.
2. Identify and read only the documentation relevant to the task.
3. Inspect the repository itself.
4. Check `docs/maintenance.md` for recent or unfinished work.
5. Check `docs/decisions.md` when the task affects an important design or decision.
6. Understand the requested outcome before changing files.

## Understand Before Changing

Scale investigation to the task. For meaningful changes, determine the requested outcome, relevant behavior and interactions, established patterns, constraints, prior investigation, and an appropriate validation method. Keep trivial changes lightweight.

## Repository Is the Source of Truth

Documentation provides context; the current repository is the ultimate source of truth. If they conflict, investigate and verify actual behavior. Do not blindly follow stale documentation or fabricate an explanation. Correct documentation when appropriate and record important discoveries in maintenance notes.

## Use Existing Patterns

Before adding an approach, inspect whether the repository already solves a comparable problem. Prefer established patterns and avoid unnecessary duplication.

## Make Focused Changes

Change only what the task requires. Avoid unrelated cleanup, refactoring, broad architectural changes, and edits to unrelated files simply because they could be improved.

## Validation

Discover and use the repository's appropriate validation mechanisms, such as tests, builds, static or type checks, automated verification, or manual verification. Report only what was actually checked; never claim an unperformed validation passed.

## Documentation

Update durable documentation when meaningful work changes architecture, behavior, constraints, workflow, decisions, or domain knowledge. Do not add documentation solely for trivial edits.

## Maintenance and Handoff

For meaningful work, add an engineering handoff to `docs/maintenance.md`: request, investigation, changes, validation, remaining work, important discoveries, and notes for future agents. Never copy an entire AI conversation into repository documentation; summarize useful engineering information instead.

Promote repeatedly useful information out of maintenance history:

- Project Context: durable purpose and context
- Architecture: durable structural knowledge
- Decisions: durable reasoning behind important choices
- Workflow: durable working process
- Maintenance: historical and current work context

## Final Review

Before finishing meaningful work, review changed files and unintended changes, check validation results, update durable documentation and maintenance when appropriate, identify unfinished work, and provide a clear handoff.

## Documentation Map

- `docs/README.md` — documentation navigation
- `docs/project-context.md` — what the project is and why it exists
- `docs/architecture.md` — how major parts are organized and interact
- `docs/decisions.md` — why important choices were made
- `docs/workflow.md` — how work should be performed
- `docs/maintenance.md` — current state, history, and handoff context

The general flow is: read guidance, identify relevant context, inspect the repository, understand and plan as needed, implement, validate, review, document durable knowledge, record meaningful maintenance context, and hand off. Apply this proportionally to task complexity.
