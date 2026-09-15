import nextCoreWebVitals from "eslint-config-next/core-web-vitals";
import nextTypescript from "eslint-config-next/typescript";

const eslintConfig = [...nextCoreWebVitals, ...nextTypescript, {
  // Keep only intentional project-wide exceptions. TypeScript handles the
  // remaining type-safety checks; the framework presets provide the default
  // React, Next.js, and JavaScript rules without a blanket disable list.
  rules: {
    "react-hooks/set-state-in-effect": "off",
  },
}, {
  ignores: [
    "node_modules/**",
    ".next/**",
    "out/**",
    "build/**",
    "next-env.d.ts",
    "src/generated/prisma/**",
  ],
}];

export default eslintConfig;
