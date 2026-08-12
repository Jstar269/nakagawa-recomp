export type PowerShellParameterValue = string | number | boolean | null | undefined;

export function buildPowerShellArgs(
  scriptPath: string,
  parameters?: Record<string, PowerShellParameterValue>,
): string[];
