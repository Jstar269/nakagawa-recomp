export interface RunIdentity {
  sourceCommit: string;
  binarySha256: string;
  profileSha256: string;
  generatedAt: string;
}

export interface EvidenceSummary {
  contentValidated: number;
  executed: number;
  heuristic: number;
  unknown: number;
  stale: number;
}

export interface ProgressPhase {
  id: string;
  title: string;
  earned: number;
  pending: number;
  regressed: number;
}

export interface ProgressSnapshot {
  total: number;
  earned: number;
  regressed: number;
  percent: number;
  phases: ProgressPhase[];
  generatedAt: number;
  fresh: boolean;
  evidenceGrade: string | null;
  staleVsBuild: boolean | null;
  runIdentity: RunIdentity | null;
  evidenceSummary: EvidenceSummary;
  itemsCarryEvidence: boolean;
}

export function parseProgressSnapshot(
  raw: unknown,
  opts?: { nowMs?: number; fileMtimeMs?: number },
): ProgressSnapshot;
