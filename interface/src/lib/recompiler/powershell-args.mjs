const PARAMETER_NAME = /^[A-Za-z][A-Za-z0-9]*$/;

/**
 * Build argv for a PowerShell script invocation.
 *
 * Every parameter name and value is a distinct argv token. Quoting is the
 * responsibility of Node's spawn implementation, not this function; embedding
 * quotes or spaces in a combined token makes PowerShell see the wrong argument.
 *
 * @param {string} scriptPath
 * @param {Record<string, string | number | boolean | null | undefined>} parameters
 * @returns {string[]}
 */
export function buildPowerShellArgs(scriptPath, parameters = {}) {
  if (typeof scriptPath !== "string" || scriptPath.length === 0) {
    throw new TypeError("scriptPath must be a non-empty string");
  }

  const args = [
    "-NoProfile",
    "-NonInteractive",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    scriptPath,
  ];

  for (const [name, value] of Object.entries(parameters)) {
    if (!PARAMETER_NAME.test(name)) {
      throw new TypeError(`invalid PowerShell parameter name: ${name}`);
    }
    if (value === false || value === null || value === undefined) continue;

    args.push(`-${name}`);
    if (value !== true) args.push(String(value));
  }

  return args;
}
