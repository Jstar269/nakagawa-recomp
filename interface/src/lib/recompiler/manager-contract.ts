export const DASHBOARD_MANAGER_ACTIONS = ["BuildFull", "BuildFast", "Run", "Test"] as const;
export const RUN_PROFILES = ["Standard", "Performance", "Benchmark", "Diagnostics", "Software"] as const;

export type DashboardManagerAction = (typeof DASHBOARD_MANAGER_ACTIONS)[number];
export type RunProfile = (typeof RUN_PROFILES)[number];

export interface ManagerRunOptions {
  profile: RunProfile;
  durationSeconds: number;
  noGui: boolean;
  softwareRender: boolean;
  snapshotInterval: number | null;
}

export interface ManagerLaunchRequest {
  action: DashboardManagerAction;
  run?: ManagerRunOptions;
  titleManifest?: string;
}

const ACTION_SET = new Set<string>(DASHBOARD_MANAGER_ACTIONS);
const PROFILE_SET = new Set<string>(RUN_PROFILES);

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function rejectUnknownKeys(value: Record<string, unknown>, allowed: Set<string>, scope: string) {
  const unknown = Object.keys(value).filter((key) => !allowed.has(key));
  if (unknown.length > 0) throw new Error(`${scope} contains unsupported field(s): ${unknown.join(", ")}`);
}

export function parseManagerLaunchRequest(value: unknown): ManagerLaunchRequest {
  if (!isRecord(value)) throw new Error("request body must be a JSON object");
  rejectUnknownKeys(value, new Set(["action", "run", "titleManifest"]), "request");

  if (typeof value.action !== "string" || !ACTION_SET.has(value.action)) {
    throw new Error(`action must be one of: ${DASHBOARD_MANAGER_ACTIONS.join(", ")}`);
  }
  const action = value.action as DashboardManagerAction;

  let titleManifest: string | undefined = undefined;
  if (value.titleManifest !== undefined) {
    if (typeof value.titleManifest !== "string" || value.titleManifest.length === 0 || value.titleManifest.length > 512) {
      throw new Error("titleManifest must be a non-empty string under 512 characters");
    }
    if (/[\r\n\t\0]|\.\.[\\/]/.test(value.titleManifest)) {
      throw new Error("titleManifest contains invalid characters or traversal");
    }
    titleManifest = value.titleManifest;
  }

  if (action !== "Run") {
    if (value.run !== undefined) throw new Error("run options are only valid for the Run action");
    return titleManifest ? { action, titleManifest } : { action };
  }

  const rawRun = value.run === undefined ? {} : value.run;
  if (!isRecord(rawRun)) throw new Error("run must be a JSON object");
  rejectUnknownKeys(rawRun, new Set(["profile", "durationSeconds", "noGui", "softwareRender", "snapshotInterval"]), "run");

  const profile = rawRun.profile ?? "Standard";
  const durationSeconds = rawRun.durationSeconds ?? 0;
  const noGui = rawRun.noGui ?? false;
  const softwareRender = rawRun.softwareRender ?? false;
  const snapshotInterval = rawRun.snapshotInterval ?? null;

  if (typeof profile !== "string" || !PROFILE_SET.has(profile)) {
    throw new Error(`run.profile must be one of: ${RUN_PROFILES.join(", ")}`);
  }
  if (!Number.isSafeInteger(durationSeconds) || Number(durationSeconds) < 0 || Number(durationSeconds) > 3600) {
    throw new Error("run.durationSeconds must be an integer from 0 to 3600");
  }
  if (typeof noGui !== "boolean" || typeof softwareRender !== "boolean") {
    throw new Error("run.noGui and run.softwareRender must be booleans");
  }
  if (
    snapshotInterval !== null &&
    (!Number.isSafeInteger(snapshotInterval) || Number(snapshotInterval) < 1 || Number(snapshotInterval) > 3600)
  ) {
    throw new Error("run.snapshotInterval must be null or an integer from 1 to 3600");
  }

  const result: ManagerLaunchRequest = {
    action,
    run: {
      profile: profile as RunProfile,
      durationSeconds: Number(durationSeconds),
      noGui,
      softwareRender,
      snapshotInterval: snapshotInterval === null ? null : Number(snapshotInterval),
    },
  };
  if (titleManifest) result.titleManifest = titleManifest;
  return result;
}

export function managerPowerShellParameters(
  request: ManagerLaunchRequest,
): Record<string, string | number | boolean> {
  const parameters: Record<string, string | number | boolean> = { Action: request.action };
  if (request.titleManifest) {
    parameters.TitleManifest = request.titleManifest;
  }
  if (request.action === "Run" && request.run) {
    parameters.Profile = request.run.profile;
    parameters.Duration = request.run.durationSeconds;
    parameters.NoGui = request.run.noGui;
    parameters.SoftwareRender = request.run.softwareRender;
  }
  return parameters;
}
