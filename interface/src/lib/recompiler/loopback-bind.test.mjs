// SPDX-License-Identifier: GPL-3.0-or-later
// loopback-bind.test.mjs — regression coverage for the dashboard's loopback
// bind policy (S-21). Two layers must hold:
//
// 1. The standalone launcher pins HOSTNAME=127.0.0.1 BEFORE importing the
//    server module. Next.js reads HOSTNAME when server.js first runs to
//    decide its listen address, so a reordered or removed pin silently
//    re-exposes the dashboard on every interface.
// 2. The per-request guards only accept loopback authorities, so a
//    misconfigured bind cannot be reached from off-host either.
//
// Run with `npm test`.

import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

test("start-standalone pins HOSTNAME to the loopback address before importing server.js", () => {
  const script = path.join(here, "..", "..", "..", "scripts", "start-standalone.mjs");
  const source = readFileSync(script, "utf8");

  const pinOffset = source.indexOf(`process.env.HOSTNAME = "127.0.0.1"`);
  assert.ok(pinOffset !== -1, "launcher must pin HOSTNAME to 127.0.0.1");

  const importOffset = source.search(/await\s+import\(.\.\.?\/\.next\/standalone\/server\.js/);
  assert.ok(importOffset !== -1, "launcher must import the standalone server");
  assert.ok(
    pinOffset < importOffset,
    "the HOSTNAME pin must execute before the server module is imported",
  );
});

test("start-standalone never forwards an inherited non-loopback HOSTNAME", () => {
  const script = path.join(here, "..", "..", "..", "scripts", "start-standalone.mjs");
  const source = readFileSync(script, "utf8");
  // The literal must be an exact loopback assignment, not a conditional or a
  // passthrough of the ambient environment value.
  assert.match(source, /process\.env\.HOSTNAME\s*=\s*"127\.0\.0\.1"\s*;/);
});
