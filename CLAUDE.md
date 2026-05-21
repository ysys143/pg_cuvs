
---

## Karpathy Coding Guidelines

> Behavioral guidelines to reduce common LLM coding mistakes. These apply to ALL code-writing agents.

### 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

### 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

### 5. Commit Often

**Keep work units small and commit frequently.**

- Don't mix multiple intentions in a single commit.
- Cut at the point where "I'd want to be able to revert to here."
- If modifying multiple files or working on a feature unit, create a branch.

> Follow the git plugin skills for branch names, commit messages, and PR format:
> `git:branch-name-convention`, `git:commit-message-convention`, `git:pr-convention`

---

*These guidelines are working if: fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.*

---

## Kent Beck Coding Guidelines

> TDD and Tidy First principles. Maintain code quality through test-driven development and structure/behavior separation.
> These apply to ALL code-writing agents.

### 1. Red → Green → Refactor

**Failing test → Make it pass → Clean up. Follow this order.**

- Write a failing test first for each small unit of functionality.
- Implement only the minimum code needed to make the test pass.
- Refactor only after the test is passing.
- Always re-run tests after refactoring.

### 2. Tidy First

**Never mix structural changes with behavioral changes.**

- Structural changes (renaming, extracting methods, moving code) → behavior stays the same
- Behavioral changes (adding features, modifications) → structure stays the same
- If both are needed: structure first, behavior second
- Separate each into its own commit

### 3. Make It Work, Make It Right, Make It Fast

**Work → Clean up → Optimize. Never skip steps.**

- First make it work (simplest thing that works)
- Then clean up the code (remove duplication, reveal intent)
- Optimize only when necessary (don't optimize speculatively)

### 4. Commit Discipline

**Only commit when tests pass.**

- Commit only after all tests pass and linter warnings are resolved
- One commit = one logical unit of change
- State in the commit message whether it is a structural or behavioral change

> Follow the git plugin skills for branch names, commit messages, and PR format:
> `git:branch-name-convention`, `git:commit-message-convention`, `git:pr-convention`

### 5. FIRST Principles

Every test must be:
- **Fast**: milliseconds, not seconds
- **Independent**: no shared state between tests
- **Repeatable**: same result regardless of environment
- **Self-validating**: pass/fail, no manual inspection
- **Timely**: written just before the code it tests

### 6. AAA Pattern

Structure every test as:
```
// Arrange — set up test data and dependencies
// Act     — execute the function/method
// Assert  — verify expected outcome
```

### 7. Test Pyramid

- 70% Unit Tests: fast, isolated, numerous
- 20% Integration Tests: module boundaries
- 10% Acceptance Tests: user-facing scenarios

Don't invert the pyramid. Integration-heavy suites are slow and fragile.

---

*These guidelines are working if: tests are written before implementation, structural and behavioral changes never appear in the same commit, and "make it work" always precedes "make it right".*

---

## Boris Cherny Coding Guidelines

> Workflow orchestration principles. Improve task quality through divide-and-conquer, learning, elegance, and autonomy.
> Applies complementarily alongside Karpathy (code quality) and Kent Beck (TDD).
> These apply to ALL code-writing agents.

### 1. Divide and Conquer

**Break complex problems down and tackle them in parallel.**

- Actively use subagents to keep the main context clean.
- Delegate research, exploration, and analysis to subagents.
- One subagent = one focused goal.
- **Broad, parallelizable, or repository-wide tasks** (full codebase analysis, multi-module changes, parallel research) → delegate to a subagent or team. Simple multi-step sequences (read → edit → verify) stay in the main context.

### 2. Learn from Every Mistake

**After user corrections, always document the lesson learned.**

- Write rules to prevent the same mistake from happening again.
- Iteratively refine lessons until the error rate drops.
- Review relevant project lessons at the start of each session.

### 3. Demand Elegance, But Know When to Stop

**For non-trivial changes, ask: "Is there a more elegant solution?"**

- If something feels hacky: "Synthesize everything learned so far and implement an elegant solution."
- Skip this process for simple, obvious fixes — no over-engineering.
- Challenge your own work before submitting.

### 4. Fix Bugs Autonomously

**When given a bug report within the current task's scope, fix it. Don't ask the user how.**

- Point to logs, errors, and failing tests — then resolve them.
- Keep the user's context switching at zero.
- Fix CI failures on your own without being told.
- If a fix would touch code outside the current task's scope, flag it rather than silently modifying it.

### 5. Pick the Right Parallelization Primitive

**Context isolates cleanly → spawn subagents. Agents need to challenge each other → use `CreateTeam`.**

- Task scope is bounded by a directory, module, or investigation target → spawn a subagent per scope.
- Cross-validation, competing hypotheses, or findings that need debate → use `CreateTeam` so agents message each other directly.
- Cross-layer work (frontend + backend + tests) with coordination needed → use `CreateTeam`.
- Subagents: lower cost, results flow back to main context. One focused goal per subagent.
- Agent Teams: higher cost, agents communicate directly — use only when inter-agent communication adds value.

---

*These guidelines are working if: complex tasks are divided among subagents, mistakes lead to documented lessons, and bugs are fixed autonomously without asking the user how.*
