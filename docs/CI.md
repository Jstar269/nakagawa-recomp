# Continuous integration and dependency maintenance

The public workflow is intentionally one always-present workflow with a cheap
classifier followed by job-level applicability checks. The classifier lives in
[`tools/ci_paths.py`](../tools/ci_paths.py), and its regression tests are in
[`tools/test_ci_paths.py`](../tools/test_ci_paths.py). The stable aggregate is
implemented by [`tools/ci_required.py`](../tools/ci_required.py) and tested in
[`tools/test_ci_required.py`](../tools/test_ci_required.py). It fails closed when
the change set cannot be determined, so an uncertain checkout runs the broader
gates instead of silently skipping them.

Name-status parsing is structural: ordinary records contain exactly one path,
rename/copy records contain both endpoints, and any malformed record becomes a
history-unavailable sentinel that selects the full matrix. `CI required` also
requires every `RUN_*` and `ALLOW_SUBSTANTIVE` output to be an explicit
case-insensitive `true` or `false`; missing or malformed control state is red.

## Workflow topology

| Event/change | Jobs that run | Jobs intentionally skipped |
| --- | --- | --- |
| Draft pull request | classification, hygiene/security, Markdown when Markdown changed, `CI required` | Python/native, Windows, dashboard, and other substantive jobs |
| Ready pull request, docs-only | classification, hygiene/security, Markdown, `CI required` | Python/native, Windows, dashboard |
| Ready pull request, `interface/**` | classification, hygiene/security, dashboard, `CI required` | Python/native, Windows |
| Ready pull request, native C/build files | classification, hygiene/security, Python tooling, native/translation, Windows, `CI required` | dashboard |
| Ready pull request, ordinary `tools/*.py` | classification, hygiene/security, Python tooling, `CI required` | native/translation, Windows, dashboard |
| Workflow/CI configuration | classification, hygiene/security, Python tooling, native/translation, Windows, dashboard, `CI required` | none of the substantive public gates |
| Dependency-only metadata (`.github/dependabot.yml`) | classification, hygiene/security, `CI required` | Python/native, Windows, dashboard |
| Mixed dashboard/native changes | classification, hygiene/security, Python tooling, native/translation, Windows, dashboard, `CI required` | none of the applicable product gates |
| Ordinary push to `main` after a validated merge | classification, hygiene/security, Markdown when needed, compact main smoke, `CI required` | expensive platform matrix; the merged PR carried it |
| Workflow push to `main` | the full applicable validation above plus main smoke | none of the substantive public gates |
| Manual `workflow_dispatch` | the full matrix, regardless of paths | none |

The `CI required` job is the stable aggregate status required by branch
protection. It runs with `always()`, accepts an intentionally skipped irrelevant
job, and fails when a classifier-applicable job fails, is cancelled, or is
otherwise incomplete. A failed hygiene/security job is never hidden by the
aggregate. Python/native jobs also wait for hygiene, so an early full-tree
failure does not spend additional runner time on dependent expensive gates.

The full-tree pre-commit run retains the publication audit and the separate
Betterleaks current-tree scan. Hygiene then runs an explicit Betterleaks
reachable-history scan and the synthetic canary gate. Markdown linting is
separate so documentation changes do not pay for a dashboard install. Dashboard
dependency changes run the clean `npm ci`,
test, lint, type-check, build, and standalone-output leakage checks. Native and
Windows jobs remain synthetic/public-input gates; no private game input is put in
Actions.

The Windows job also runs `mingw32-make production-smoke` in the existing MSYS2 UCRT64/GCC,
SDL3, and Vulkan environment. That target generates its PSP-shaped input from committed source,
uses the ordinary loader/import/analyzer/codegen pipeline, links the complete public-safe
production runtime and real driver, and then reaches a registered HLE NID through the scheduler.
Its pass condition is a relocation-dependent guest-memory sentinel checked by the production
driver. The link map and runtime markers make reduced `gate_stub` substitution or omitted critical
objects fail closed. This is a production-composition integration test, not PSP-hardware or private
title acceptance evidence.

The Windows job also runs `production-smoke-gap`: the same fixture with its helper omitted from
native emission at build time, proving region A reaches the omitted guest address through the
ordinary production `dispatch()` seam. Analyzer-owned executable-span registration permits only
those guest bytes to enter the fail-closed interpreter; the gate then requires a registered AOT
region-B handoff, real HLE call, and final `0x00001235` production-driver assertion.

The Windows job also runs `cosim-selftest` and `cosim-mutants`. The first executes the same
source-owned guest bytes twice — once as generated native code, once through the production
interpreter floor — and reports the first difference in the canonical instruction trace, the
ordered guest writes, the guest memory window, or the architectural state vector. The second
rebuilds that comparator against deliberately mutated copies of the interpreter and requires each
defect class to fail the gate; a mutant that only breaks the build is rejected as `INVALID`, not
counted as a kill. Both are source-owned and need no game input. See
[`fixtures/cosim/README.md`](../fixtures/cosim/README.md) for the comparison contract and the
limits of the evidence.

## Local readiness before opening a pull request

Readiness used to be assembled ad hoc and was repeatedly wrong: a locally green
candidate failed the hosted publication audit, and the repaired candidate then
failed the hosted *attestation* gate — a verifier that was runnable locally the
whole time and simply never run. Run all of these, in this order, and treat any
failure as blocking:

```bash
python tools/policy_sync.py
python tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
python tools/provenance_attest_verify.py --repo . --candidate HEAD --base origin/main     --trusted-ledger <external detailed ledger> --workdir <scratch outside the repo>
python tools/lint_docs.py
python -m unittest discover -s tools -p "test_*.py"
git diff --check origin/main...HEAD
```

The attestation verifier is the one most easily forgotten and the one that most
often blocks a merge, because changed implementation bytes need an exact
reviewed-blob approval that no automated step can grant. See
[`docs/PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) for the two authority
tiers and what each class needs.

These checks have no local equivalent and are never implied by a local pass:
CodeQL, `dependency-review`, the Betterleaks history scan, the Windows runtime
compile gate, and the main integration smoke.

## Text, encoding, and line-ending contract

The repository's text bytes are **UTF-8 without BOM, LF, with a final newline**.
`.gitattributes` (`* text=auto eol=lf`) and `.editorconfig`
(`charset = utf-8`, `end_of_line = lf`, `insert_final_newline = true`) are the
authority; this section says how to *produce* bytes that satisfy them.

Git normalises on commit, so the index is clean by construction — a scan of all
674 tracked files finds no CRLF, no BOM and no UTF-16. The damage happens
elsewhere: in working trees and in generated artifacts. A checkout polluted with
CRLF makes every touched file's content hash disagree with the provenance
ledger, and the resulting wall of `PROVENANCE_CONTENT_MISMATCH` names the
symptom rather than the cause. That is why `publish_audit` now reports
`TEXT_LINE_ENDING_CRLF`, `TEXT_ENCODING_BOM`, `TEXT_ENCODING_UTF16` and
`TEXT_FINAL_NEWLINE` directly, skipping anything binary.

**Python.** Text mode translates `
` to the host newline unless told otherwise,
so on Windows `write_text(s, encoding="utf-8")` silently emits CRLF and a
different SHA-256 than the same code on Linux. Every writer that produces
canonical, tracked, or hash-participating text must pin it:

```python
path.write_text(text, encoding="utf-8", newline="
")   # canonical text
path.write_bytes(canonical_bytes)                        # already-canonical bytes
```

`CanonicalWriterTests` asserts this for the canonical writers, so the rule
cannot quietly regress.

**PowerShell.** Use PowerShell 7+, which the project already requires: in
Windows PowerShell 5.1 `-Encoding utf8` means UTF-8 **with** BOM, and in 7+ it
means without. `Set-Content` and `Out-File` also join with the host newline. For
anything byte-exact, bypass the text pipeline entirely:

```powershell
[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllBytes($path, $bytes)
```

**Never route exact bytes through a text pipeline.** `git show ... | Set-Content`
and `<binary> | Out-File` re-encode and re-line-end their input. To extract exact
repository bytes use `git archive`, `git cat-file`, or a redirect from a
binary-safe shell. No tracked script does this today; keep it that way.

**Bash and WSL** must likewise preserve LF and must not rewrite bytes
incidentally — a heredoc that reflows content is a rewrite.

**Generated evidence outside the worktree** follows the same contract whenever it
participates in a SHA-256 manifest or a forensic byte comparison, for the obvious
reason: a manifest that changes with the shell that produced it proves nothing.
Historical raw or binary capture evidence is never normalised.

The invariant, stated once: **the same logical generated text must produce the
same bytes and the same SHA-256 whether it came from PowerShell, Windows Python,
Bash, or WSL.**

## Classifier invariants

`tools/ci_paths.py` decides which gates run. The only failure that matters is a
**false negative** — a build-affecting change classified as documentation or
tooling and therefore skipping a native or Windows gate. These invariants exist
to prevent that, and `tools/test_ci_paths.py` asserts each one:

- **Unknown paths fail closed.** Any path matching no predicate forces the full
  matrix. Adding a new kind of file makes CI more expensive, never less.
- **Every change type counts.** The changed-file query uses name-status without
  a narrowing diff filter, and retains both sides of a rename. Filtering to
  `ACMR` or keeping only a rename's new name can drop a build-affecting source,
  making a commit that removes or renames C code classify as docs-only and skip
  the native and Windows compile gates.
- **An empty or unobtainable file list forces the full matrix**, so a shallow
  clone or an unusual event payload cannot quietly narrow the run.
- **Draft suppression never rewrites classification.** Only `allow_substantive`
  goes false, including when an unknown path forces full applicability; the path
  facts stay true, so the ready-for-review transition needs no reclassification.
- **`hygiene` is ungated, and that is load-bearing.** The all-files pre-commit
  run — which includes `publish_audit --provenance-self-consistency`,
  `policy_sync`, and the Betterleaks scan — executes on every event, so the
  security and publication boundary is never path-gated. This is what makes the
  cheap paths safe: every tracked file is inside the published surface and the
  provenance ledger hashes each one, so *any* change invalidates the generated
  ledger and `PUBLIC_EXPORT.json` until they are refreshed. A docs-only change
  may therefore skip the Python suite only because the audit still runs here.
  `PublicationCoverageInvariantTests` pins both halves so this cannot regress
  into a path-gated audit.
- **The published surface is derived, not listed.** `_is_public_surface` asks the
  publication policy instead of maintaining a second list that can drift; it
  fails closed to "published" when the policy cannot be read. The `public_surface`
  output is exported so a local readiness check can route the same decision,
  where no ungated hygiene equivalent runs.

Test modules use their logical implementation subject for classification:
`tools/test_<subject>.py` is evaluated through the same subsystem predicates as
`tools/<subject>.py`. This keeps build-relevant HST, title, codegen, and native
tool tests on the native and Windows gates without making every Python test
expensive. A new native-relevant tool should therefore be named and classified
like its implementation; add a predicate only when the implementation itself
belongs to a new subsystem.

## Cost and caching rules

GitHub-hosted Windows time is billed at a higher multiplier than Linux time. The
workflow therefore gates the Windows runner behind the cheaper Linux hygiene and
native gates, cancels superseded PR runs, and avoids repeating the full matrix on
ordinary main pushes. The workflow uses dependency/tool caches only (pip and npm);
compiled runtime objects and generated shader/code output are not cached, so the
repository's content-addressed invalidation and freshness checks remain the
source of truth. No volatile dollar figure is part of the repository contract.

Hosted GitHub Actions execution is active. The `main` ruleset requires `CI required`,
`OSV Vulnerability Scan`, `dependency-review`, `Hygiene and security`, and
`CodeQL` on exact pull-request heads. Path-gated workflows also run the applicable
classifier, Markdown, native/translation, dashboard, main-smoke, Python, and
Windows gates. A green public-safe run proves only the paths it executes; it is
not a complete private-title gameplay route, and local verification remains
local-only.

## Windows hosted runner policy

The Windows job intentionally remains on `windows-2022`. Hosted execution is
active, but that floating label is a GitHub-hosted Windows Server image rather
than an end-user support promise; the supported developer platform is Windows 11
x64 as documented in [SETUP.md](SETUP.md). A `windows-2025` migration is a
separate hosted-validation decision. Consult the current
[Windows 2022 image inventory](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)
instead of treating a dated image version or tool inventory as an evergreen
repository guarantee.

## Dependabot policy

`.github/dependabot.yml` checks GitHub Actions, dashboard npm, root pip, and
pre-commit ecosystems monthly. Minor and patch updates are grouped per ecosystem;
major updates remain standalone because they can change APIs, runners, or build
semantics. Security updates remain enabled and are not suppressed by the routine
groups. The open-PR limits keep routine maintenance from crowding out focused
engineering work.
