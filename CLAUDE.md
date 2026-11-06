# CLAUDE.md — GlassineOS/home

The Home screen and its widget runtime (see `README.md`). Every change: run `make check` and extend the tests for new behaviour.

- **Multi-device:** no device names in generic code. What differs per device comes from per-device config.
- Specs: Spec Kit 1.1.0 with the org preset (`.specify/`, setup in `.glassine/org/SPECS.md`). A new
  feature starts with `specs/###-name/spec.md` before any plan or code; only `spec.md` is committed.
  The repository principles are the last section of `.specify/memory/constitution.md`.
- Commits carry an `Assisted-by:` or `Co-Authored-By:` trailer. Never add `Signed-off-by`. Run
  `.glassine/org/scripts/safety-scan.sh --staged` before committing.
- Organization guidance for coding agents, including the project knowledge search (run
  `git submodule update --init` if the import below is missing):

@.glassine/org/AGENTS.md
