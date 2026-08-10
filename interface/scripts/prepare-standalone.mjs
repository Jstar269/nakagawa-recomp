import { cpSync, existsSync, mkdirSync, readdirSync, rmSync } from "node:fs";

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
