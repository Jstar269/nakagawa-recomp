// SPDX-License-Identifier: GPL-3.0-or-later
import { spawnSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const out = ".next/standalone";
mkdirSync(out + "/.next", { recursive: true });
cpSync(".next/static", out + "/.next/static", { recursive: true, force: true });
if (existsSync("public")) {
  cpSync("public", out + "/public", { recursive: true, force: true });
}
for (const name of readdirSync(out)) {
  if (name === ".env" || name.startsWith(".env.")) {
    rmSync(out + "/" + name, { force: true });
  }
}

// The standalone output redistributes the traced npm packages, so it must ship the
// generated third-party notices. Fail closed: a standalone tree without notices
// stops the build rather than shipping an unrecorded binary distribution.
// `out` is relative to this package directory (the npm script's working
// directory), so anchor it to the interface root, not the repository root.
const interfaceRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = join(interfaceRoot, "..");
const generator = join(repoRoot, "tools", "dashboard_notices.py");
const standalone = join(interfaceRoot, out);
const python = process.env.PYTHON || (process.platform === "win32" ? "python" : "python3");
const notices = spawnSync(python, [generator, "--standalone", standalone], {
  cwd: repoRoot,
  stdio: "inherit",
});
if (notices.error) {
  console.error(`DASHBOARD_NOTICES_UNAVAILABLE: could not run ${generator}: ${notices.error.message}`);
  process.exit(1);
}
if (notices.status !== 0) {
  console.error(`DASHBOARD_NOTICES_FAILED: ${generator} exited with status ${notices.status}`);
  process.exit(notices.status ?? 1);
}
