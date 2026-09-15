// loopback-guard.test.ts — regression coverage for the request-side half of
// the dashboard's loopback policy (S-21): the control-route guard must accept
// every loopback authority form and reject anything that could be reached
// from off-host, so a misconfigured bind cannot be talked to either.
// Run with `npm run test:db`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { NextRequest } from "next/server";
import { rejectNonLocalControlRequest } from "./local-request";

function requestFor(host: string | null): NextRequest {
  const headers = new Headers();
  if (host !== null) headers.set("host", host);
  return new NextRequest("http://127.0.0.1:3000/api/recompiler/boot", { headers });
}

test("loopback authorities are accepted in every valid form", () => {
  for (const host of ["127.0.0.1:3000", "localhost:3000", "[::1]:3000", "localhost"]) {
    assert.equal(
      rejectNonLocalControlRequest(requestFor(host)),
      null,
      `loopback authority ${host} must be accepted`,
    );
  }
});

test("non-loopback and malformed authorities are rejected with 403", () => {
  for (const host of ["0.0.0.0:3000", "192.168.1.10:3000", "example.com", "[fe80::1]:3000", "", null]) {
    const rejection = rejectNonLocalControlRequest(requestFor(host));
    assert.ok(rejection !== null, `authority ${JSON.stringify(host)} must be rejected`);
    assert.equal(rejection.status, 403);
  }
});

test("credentials smuggled into the host header never widen the acceptance set", () => {
  // The guard rejects userinfo outright; a parser confusion must not turn
  // "user@localhost" into an accepted loopback authority.
  const rejection = rejectNonLocalControlRequest(requestFor("user@localhost"));
  assert.ok(rejection !== null, "userinfo authority must be rejected");
  assert.equal(rejection.status, 403);
});
