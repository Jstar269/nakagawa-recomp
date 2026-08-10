# Toolchain baseline — August 2026

Track: dependency/toolchain modernization only. No PSP/runtime semantics changed and no
private HST routes were run.

- Branch: `freebuff/toolchain-refresh-202608`
- Base: `origin/main` @ `b31f5aa548587350901d7ddef3c4b94fa2e47340` (fetched 2026-08-03)
- Local verification host: Windows 11 x64; PowerShell 7.6.4; CPython 3.14.6;
  Git 2.55.0.windows.3; MSYS2 UCRT64 toolchain

## Inventory

| Tool | Repo baseline (origin/main) | Latest stable upstream | Chosen / tested | Change |
| --- | --- | --- | --- | --- |
| Node.js (dashboard) | engines `>=20.19.0` (Node 20 EOL) | 24 LTS `24.18.1` (2026-07-29); Current `26.5.1` | `24.18.1` — engines `>=24.18.1` | changed |
| npm | engines `>=10.0.0` | `11.17.0` (bundled with Node 24.18.1) | `11.17.0` — engines `>=11.17.0` | changed |
| Next.js | `^16.2.11` | `16.3.0` (2026-08-03) | `^16.3.0` (adopted; see audit note) | changed |
| eslint-config-next | `^16.2.11` | `16.3.0` | `^16.3.0` | changed |
| react / react-dom | `^19.2.8` | `19.2.8` | `^19.2.8` | unchanged |
| TypeScript | `^7.0.2` | `7.0.2` | `^7.0.2` | unchanged |
| prisma / @prisma/client / adapter / engines | `^7.9.0` (resolved 7.9.1) | `7.9.1` | `^7.9.1` | changed (range) |
| better-sqlite3 (transitive) | `12.11.1` | `13.0.2` | `12.11.1` | intentionally unchanged |
| sharp | `^0.35.3` | `0.35.3` | `^0.35.3` | unchanged |
| framer-motion | `^12.42.2` | `12.43.0` | `^12.43.0` | changed |
| lucide-react | `^1.25.0` | `1.28.0` | `^1.28.0` | changed |
| recharts | `^3.10.0` | `3.10.1` | `^3.10.1` | changed |
| @radix-ui/react-* (13 packages) | 1.1.20 – 2.1.21 | 1.1.23 – 2.1.24 | latest per package | changed (patch/minor) |
| @types/node | `^26.1.1` | `26.1.2` (Current line); `24.13.3` (24 line) | `^24.13.3` | changed (aligned to Node 24 LTS) |
| @types/react / @types/react-dom | `^19.2.17` / `^19.2.3` | `19.2.18` / `19.2.4` | latest | changed |
| ESLint | `^10.8.0` | `10.8.0` | `^10.8.0` | unchanged |
| dotenv, clsx, cva, tailwind-merge, tailwindcss-animate, tw-animate-css, tailwindcss, @tailwindcss/postcss, unrs-resolver, @types/better-sqlite3 | already latest | same | unchanged | unchanged |
| PowerShell | `#requires -Version 7.6`; docs 7.6+ | `7.6.4` (2026-07-20) | `7.6.4` (requirement still accurate) | unchanged |
| CPython | `>=3.14,<3.15`; CI `3.14` | `3.14.6` (2026-06-10) | `3.14.6` | unchanged |
| Git for Windows | unversioned | `2.55.0(3)` (2026-07-14) | `2.55.0.windows.3` | unchanged |
| MSYS2 UCRT64 gcc | unversioned install | `16.1.0-6` (2026-08-01) | MSYS2-provided | unchanged |
| GNU Make | unversioned install | `4.4.1-5` | MSYS2-provided | unchanged |
| Vulkan SDK | unpinned (discovery order) | `1.4.357.0` (2026-07-28, LunarG) | unpinned (by design) | unchanged |
| Vulkan-Headers / Loader / ValidationLayers | MSYS2 packages | synchronized to 1.4.357 | MSYS2-provided | unchanged |
| SDL3 | MSYS2 package + DLL | `3.4.12` (2026-07-01) | MSYS2-provided | unchanged |
| PSPDEV / PSPSDK | pinned audit input `v20260501` (`cc874700eaef9e00c8ec63e0d116926e1048b656`) | `v20260801` (2026-08-01) | pin kept | intentionally unchanged |
| PPSSPP | external oracle only, not vendored | `v1.20.4` | external | unchanged |
| Ghidra | external analysis aid | `12.1.2` (2026-06-05) | external | unchanged |

## Changes on this branch

1. `interface/package.json` — engines `node >=24.18.1`, `npm >=11.17.0` (Node 20 is EOL; the
   tested baseline is Node 24 LTS 24.18.1, not Node Current); dependency bumps per the table.
2. `interface/package-lock.json` — regenerated with npm 11.17.0 (lockfileVersion 3).
3. `.github/workflows/ci.yml` — dashboard and markdown jobs: `node-version: "22"` → `"24"`.
4. `interface/next.config.ts` — `experimental.useTypeScriptCli: true`. Diagnosed requirement:
   Next.js 16.x in-process type-checking uses the TypeScript compiler API, which TypeScript 7
   (Go-native) does not expose; Next's documented accommodation is the CLI. Dashboard-only build
   config; no runtime/recompiler semantics.
5. `interface/README.md` — dashboard prerequisites updated to Node.js 24 LTS and npm 11.
6. Next.js adopted at `16.3.0` (not the originally chosen `16.2.12`) because `npm audit fix`
   selected it: `16.2.12` sits inside the vulnerable postcss-dependent range
   (`9.3.4-canary.0 - 16.3.0-preview.10`) for GHSA-fxqj-rqcc-2cmp, while `16.3.0` stable is
   outside it. `fast-uri` also moved `3.1.4 → 3.1.5` to clear the high-severity
   GHSA-7p8r-x3mc-p8w7 (host confusion via backslash authority introducer; transitive through
   prisma → @prisma/dev → @prisma/streams-local → ajv).

## Verification record

Executed 2026-08-03 in the worktree with Node `24.18.1` (`npx -y node@24.18.1`) and npm 11.17.0:

| Gate | Result |
| --- | --- |
| `npm ci` | pass |
| `npm audit --audit-level=high` | pass (exit 0; 3 moderate postcss-chain findings via @tailwindcss/postcss, pre-existing, no high/critical) |
| `npm ls brace-expansion minimatch --all` | deduped/overridden as before |
| `npm test` | pass (11/11) |
| `npm run typecheck` | pass (with `@types/node` 24.13.3) |
| `npm run db:push` | pass |
| `npm run build` | pass (Next 16.3.0, standalone; requires `DATABASE_URL` exactly as CI sets it) |
| standalone leakage check (CI-equivalent) | pass (27.4 MB; no repo paths, no game-input files) |
| `python -m unittest discover -s tools -p "test_*.py"` | OK (33 skips; private-input/oracle dependent) |

### Follow-up 2026-08-04 — PostCSS 8.5.23 (issue #253)

The 3 moderate postcss-chain findings recorded above (GHSA-fxqj-rqcc-2cmp / CVE-2026-69153,
affected `<=8.5.22`) became independently patchable when PostCSS 8.5.23 published. The
`overrides.postcss` pin moved `8.5.18 → 8.5.23` and `interface/package-lock.json` was
regenerated with npm 11.17.0 under Node 24.18.1 (single `node_modules/postcss` entry;
`nanoid` requirement follows to `^3.3.16`, already resolved at 3.3.16).

Re-verified 2026-08-04 on the same Node 24.18.1 / npm 11.17.0 pairing, from a removed
`node_modules`:

| Gate | Result |
| --- | --- |
| `npm ci` | pass (clean tree, 577 packages) |
| `npm ls postcss --all` | pass (`@tailwindcss/postcss` → 8.5.23 overridden; `next@16.3.0` → 8.5.23 deduped) |
| `npm audit` | pass (`found 0 vulnerabilities`; GHSA-fxqj-rqcc-2cmp no longer reported) |
| `npm audit --audit-level=high` | pass (exit 0, no high/critical) |
| `npm test` | pass (11/11) |
| `npm run typecheck` | pass |
| `npm run build` | pass (Next 16.3.0 standalone via `experimental.useTypeScriptCli`) |

Next.js stays at 16.3.0; the advisory range that motivated its adoption in #249 is unchanged.
The #248 lint blocker below is unaffected — `npm run lint` still exits 2 with the identical
`typescript-eslint does not support TS 7.0.` error (typescript-eslint 8.65.0 + TypeScript
7.0.2), and nothing was suppressed to hide it.

## Pre-existing issues reproduced on origin/main (not introduced here)

1. `npm run lint` fails with `typescript-eslint does not support TS 7.0.` The npm registry reports
   `typescript-eslint@8.66.0` as latest on 2026-08-04, with peer range `typescript >=4.8.4 <6.1.0`;
   TypeScript 7.0.2 remains outside that range. origin/main already resolves 8.65.0 with TypeScript
   7.0.2, so lint fails identically on main. No supported release currently covers TS 7; do not
   suppress the parser error or downgrade the project's TypeScript baseline merely for green lint.
   Tracked in GitHub issue [#248](https://github.com/Jstar269/nakagawa-recomp/issues/248); upstream
   tracking is typescript-eslint/typescript-eslint#10940. Re-verify the npm peer range before changing
   this statement.
2. The Next.js build failure that motivated `useTypeScriptCli` is likewise pre-existing on main
   (TypeScript 7.0.2 + Next 16.2.11). This branch fixes it with the documented config.

## Intentionally left unchanged

- `better-sqlite3` 12.11.1 — `@prisma/adapter-better-sqlite3@7.9.1` pins `^12.6.0`.
- PSPDEV pin in `assets/upstream/pspdev.NOTICE.md` — provenance-pinned audit input; refreshing
  requires re-verification of the new revision's contents.
- Vulkan SDK unpinned discovery — deliberate design (see `docs/SETUP.md`); 1.4.357.0 is the
  current reference.
- GitHub Actions pins — already current via Dependabot (checkout v7.0.1, setup-node v7.0.0,
  setup-python v7.0.0, msys2/setup-msys2 v2).
- Hosted-runner inventory note in `docs/CI.md` — factual external inventory, rechecked only when
  the image inventory changes.
- Node Current (26.x) — deliberately not adopted; Node 24 LTS is the tested baseline. The
  `engines` range `>=24.18.1` still admits newer Node lines by design; the tested and
  documented contract is Node 24 LTS 24.18.1.

## Sources

- Node.js — <https://nodejs.org/en/blog/release/v24.18.1> (2026-07-29); npm bundled 11.17.0
- npm registry — `npm view <pkg> version` (2026-08-03)
- LunarG Vulkan SDK 1.4.357.0 — <https://vulkan.lunarg.com/sdk/home> (2026-07-28)
- GitHub releases API — pspdev/pspdev `v20260801`; hrydgard/ppsspp `v1.20.4`;
  libsdl-org/SDL `release-3.4.12`; NationalSecurityAgency/ghidra `Ghidra_12.1.2_build`;
  powershell/PowerShell `v7.6.4`
- MSYS2 packages — <https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-gcc> (`16.1.0-6`),
  <https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-make> (`4.4.1-5`)
- Python — <https://www.python.org/downloads/> (3.14.6); Git for Windows —
  <https://git-scm.com/install/windows> (2.55.0(3))
