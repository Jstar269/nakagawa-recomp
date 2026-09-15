// SPDX-License-Identifier: GPL-3.0-or-later
// manager-sse.test.ts — regression coverage for the manager SSE route's
// lifecycle behavior (issue O-08): idle streams stay open and emit a status
// event instead of closing, so dashboard panels can refresh on real state
// changes rather than polling. Run with `npm run test:db`.

import { test } from "node:test";
import assert from "node:assert/strict";

let managerRoute: typeof import("@/app/api/recompiler/manager/route");
let managerProcess: import("./manager-process").ManagerProcessState;

test("idle manager SSE stream stays open and reports status", async () => {
  managerRoute = await import("@/app/api/recompiler/manager/route");
  managerProcess = (await import("./manager-process")).managerProcess;

  const req = new Request("http://localhost/api/recompiler/manager", {
    headers: { "x-forwarded-for": "127.0.0.1", host: "localhost:3000" },
  });

  const res = await managerRoute.GET(req as never);
  assert.equal(res.status, 200);
  assert.match(res.headers.get("content-type") ?? "", /text\/event-stream/);

  const reader = res.body!.getReader();
  const { value } = await reader.read();
  const chunk = new TextDecoder().decode(value);
  assert.match(chunk, /event: status/, "idle stream must emit a status event");
  assert.match(chunk, /"phase":/, "status event must carry the lifecycle phase");
  // The stream must NOT be closed for an idle manager: reading again blocks
  // (still readable) rather than resolving with done:true.
  const race = await Promise.race([
    reader.read().then((r) => ({ done: r.done })),
    new Promise((resolve) => setTimeout(() => resolve({ pending: true }), 250)),
  ]);
  assert.ok(
    (race as { pending?: boolean }).pending,
    "stream must remain open while the manager is idle",
  );
  await reader.cancel();
});

test("status listener registered by the SSE route receives forwarded events", async () => {
  managerRoute = await import("@/app/api/recompiler/manager/route");
  const { managerProcess: mp } = await import("./manager-process");

  const req = new Request("http://localhost/api/recompiler/manager", {
    headers: { "x-forwarded-for": "127.0.0.1", host: "localhost:3000" },
  });
  const res = await managerRoute.GET(req as never);
  const reader = res.body!.getReader();
  // Drain the initial status frame.
  await reader.read();

  // Simulate a lifecycle transition via the process registry's listeners.
  const statuses: unknown[] = [];
  const forward = (m: unknown) => statuses.push(m);
  mp.listeners.add(forward);
  const { createManagerProcess } = await import("./manager-process");
  void createManagerProcess;
  // Emit a status-shaped message the way the registry does.
  for (const listener of mp.listeners) {
    (listener as (m: unknown) => void)({
      type: "status",
      phase: "exited",
      action: null,
    });
  }
  mp.listeners.delete(forward);

  const { value } = await reader.read();
  const chunk = new TextDecoder().decode(value);
  assert.match(chunk, /"phase":"exited"/, "status event must be forwarded to the stream");
  await reader.cancel();
  void managerProcess;
});
