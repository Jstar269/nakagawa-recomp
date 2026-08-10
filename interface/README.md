# Nakagawa Recomp dashboard

This is an optional Next.js 16/TypeScript dashboard for inspecting and driving a local Nakagawa Recomp workspace. It is a separate project and is not compiled into `hst.exe`.

## Current scope

The visible dashboard controls operate on the local workspace: builds and tests use
`hst_manager.ps1`, runs use the required image/base/entry contract, debug views attach to
the live `hst.exe`, and boot health is parsed from native `BOOT_EVENT` milestones. There is
no simulated fallback and the dashboard does not manufacture a substitute game binary.

The source tree retains enhancement mock-ups for graphics, performance, controller,
patch, and preset ideas. They are design references only: their configuration values are
not mapped to native runtime switches, and the UI labels these concepts as unimplemented.
There is no synthetic build-job API, placeholder binary generator, or bundle download.

Do not expose it to an untrusted network. Routes can inspect local files, invoke repository
tooling, and attach to the game process. The supplied `dev` and `start` scripts bind to
`127.0.0.1`. The request boundary also rejects non-loopback Host headers, cross-site requests,
and non-local Origin headers for mutations. Process-control endpoints return 501 off Windows.

The runtime diagnostics console is read-only by default. To opt into pause/resume and
memory/register writes for a local debugging session, set
`HST_DASHBOARD_LIVE_CONTROL=1` before starting the server. The console API validates every
action and argument, applies bounded reads, and does not expose the script's standalone
simulation flag.

## Develop

Use Node.js 24 LTS (24.18.1 or newer) and npm 11 (11.17.0 or newer):

```powershell
cd interface
npm ci
Copy-Item .env.example .env
npm run db:push
npm run dev
```

The app listens on `http://localhost:3000`.

Profiles and debug profiles carry a `schemaVersion` column (added in the #188
dashboard-integrity work). After pulling changes that touched `prisma/schema.prisma`,
re-run `npm run db:push` once so the local database gains the new column; rows are
then validated on read and reported as `corrupt` / `unsupported-version` instead of
being silently replaced with defaults.

## Validate

```powershell
npm test
npm run lint
npm run typecheck
npm run build
```

`next.config.ts` intentionally keeps `outputFileTracingRoot` scoped to this directory. Removing it can copy the multi-gigabyte parent workspace into `.next/standalone`.

SQLite state, environment files, tool results, dependencies, and Next.js output are ignored. Never place game files or secrets under `public/`.
