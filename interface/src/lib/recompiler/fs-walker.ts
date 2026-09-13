import { existsSync, readdirSync, realpathSync, statSync, lstatSync } from "node:fs";
import path from "node:path";

export interface SafeWalkOptions {
  maxDepth?: number;
  maxFiles?: number;
  targetFileName?: string;
}

/**
 * Traverses a directory tree fail-closed with path-traversal resistance:
 * - Uses path.relative containment check (both lexical and realpath) rather than string prefix.
 * - Tracks visited real directory paths to prevent recursion loops from junctions or symlinks.
 * - Enforces hard depth and total file count caps.
 */
export function safeWalkDirectory(
  rootDir: string,
  options: SafeWalkOptions = {},
): string[] {
  const maxDepth = options.maxDepth ?? 16;
  const maxFiles = options.maxFiles ?? 5000;
  const targetFileName = options.targetFileName;

  if (!existsSync(rootDir)) return [];

  const normalizedRoot = path.resolve(rootDir);
  let realRoot: string;
  try {
    realRoot = realpathSync.native(normalizedRoot);
  } catch {
    return [];
  }

  const results: string[] = [];
  const visitedDirs = new Set<string>();

  function walk(currentDir: string, currentDepth: number): void {
    if (currentDepth > maxDepth || results.length >= maxFiles) return;

    let realCurrent: string;
    try {
      realCurrent = realpathSync.native(currentDir);
    } catch {
      return;
    }

    // Check directory containment relative to realRoot
    const rel = path.relative(realRoot, realCurrent);
    if (rel !== "" && (rel.startsWith("..") || path.isAbsolute(rel))) {
      return;
    }

    // Loop detection via canonical realpath
    if (visitedDirs.has(realCurrent)) return;
    visitedDirs.add(realCurrent);

    let entries: string[];
    try {
      entries = readdirSync(currentDir);
    } catch {
      return;
    }

    for (const entry of entries) {
      if (results.length >= maxFiles) break;

      const entryPath = path.join(currentDir, entry);
      try {
        const lst = lstatSync(entryPath);
        if (lst.isSymbolicLink()) continue;

        const realEntry = realpathSync.native(entryPath);
        const relEntry = path.relative(realRoot, realEntry);
        if (relEntry.startsWith("..") || path.isAbsolute(relEntry)) {
          continue;
        }

        const st = statSync(entryPath);
        if (st.isDirectory()) {
          walk(entryPath, currentDepth + 1);
        } else if (!targetFileName || entry === targetFileName) {
          results.push(entryPath);
        }
      } catch {
        // Skip inaccessible entries
      }
    }
  }

  walk(rootDir, 0);
  return results;
}
