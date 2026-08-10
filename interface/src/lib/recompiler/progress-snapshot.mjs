/* Pure parser for the repo-root progress.json emitted by tools/progress_tracker.py.
 *
 * Split out of runner.ts so the #181 provenance surface (run identity, evidence
 * grades) is unit-testable without touching the filesystem: the caller supplies
 * the parsed JSON plus file-mtime/now for the freshness flag. Every field is
 * read defensively; malformed or legacy files degrade to the pre-#181 shape
 * instead of throwing. */
// Non-finite or non-numeric input degrades to 0 so a corrupt progress.json can
// never render as "NaN%" or poison later arithmetic with NaN.
function toFinite(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : 0;
}

export function parseProgressSnapshot(raw, opts = {}) {
  const nowMs = typeof opts.nowMs === "number" ? opts.nowMs : Date.now();
  const fileMtimeMs = typeof opts.fileMtimeMs === "number" ? opts.fileMtimeMs : 0;

  const total = toFinite(raw?.total_units ?? raw?.total ?? 0);
  const earned = toFinite(raw?.units_earned ?? raw?.earned ?? 0);
  const regressed = toFinite(raw?.units_regressed ?? raw?.regressed ?? 0);
  const percent =
    raw?.completion_pct !== undefined
      ? toFinite(raw.completion_pct)
      : total
        ? Math.round(((earned - regressed) / total) * 10000) / 100
        : 0;

  let phases = [];
  if (Array.isArray(raw?.phases)) {
    phases = raw.phases.map((ph) => ({
      id: String(ph?.id ?? 0),
      title: String(ph?.title ?? ""),
      earned: toFinite(ph?.earned ?? 0),
      pending: toFinite(ph?.pending ?? 0),
      regressed: toFinite(ph?.regressed ?? 0),
    }));
  } else if (raw?.by_phase && typeof raw.by_phase === "object") {
    phases = Object.entries(raw.by_phase).map(([id, ph]) => {
      const phEarned = toFinite(ph?.earned ?? 0);
      const phRegressed = toFinite(ph?.regressed ?? 0);
      const phTotal = toFinite(ph?.total ?? 0);
      return {
        id,
        title: `Phase ${id}`,
        earned: phEarned,
        pending: Math.max(0, phTotal - phEarned - phRegressed),
        regressed: phRegressed,
      };
    });
  }

  // #181: run-level provenance emitted by progress_tracker.py.
  const runRaw = raw?.run && typeof raw.run === "object" ? raw.run : null;
  const identityRaw = runRaw?.identity && typeof runRaw.identity === "object" ? runRaw.identity : null;
  const runIdentity = identityRaw
    ? {
        sourceCommit: String(identityRaw.source_commit ?? ""),
        binarySha256: String(identityRaw.binary_sha256 ?? ""),
        profileSha256: String(identityRaw.profile_sha256 ?? ""),
        generatedAt: String(identityRaw.generated_at ?? ""),
      }
    : null;
  const evidenceGrade = typeof runRaw?.evidence_grade === "string" ? runRaw.evidence_grade : null;
  const staleVsBuild = typeof runRaw?.stale_vs_build === "boolean" ? runRaw.stale_vs_build : null;

  // #181: per-item observation grades. Unknown labels are ignored rather than
  // counted, so a future grade name degrades safely on older dashboards.
  const evidenceSummary = { contentValidated: 0, executed: 0, heuristic: 0, unknown: 0, stale: 0 };
  let itemsCarryEvidence = false;
  if (Array.isArray(raw?.items)) {
    for (const item of raw.items) {
      const grade = item && typeof item.evidence === "string" ? item.evidence : null;
      if (!grade) continue;
      itemsCarryEvidence = true;
      if (grade === "content-validated") evidenceSummary.contentValidated += 1;
      else if (grade === "executed") evidenceSummary.executed += 1;
      else if (grade === "heuristic") evidenceSummary.heuristic += 1;
      else if (grade === "unknown") evidenceSummary.unknown += 1;
      else if (grade === "stale") evidenceSummary.stale += 1;
    }
  }

  return {
    total,
    earned,
    regressed,
    percent,
    phases,
    generatedAt: nowMs,
    fresh: fileMtimeMs > 0 && nowMs - fileMtimeMs < 60_000,
    evidenceGrade,
    staleVsBuild,
    runIdentity,
    evidenceSummary,
    itemsCarryEvidence,
  };
}
