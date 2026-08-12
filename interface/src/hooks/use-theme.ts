"use client";

import { useCallback, useEffect, useState } from "react";

export type Theme = "light" | "dark";

export function useTheme() {
  // Lazy init: read the DOM class set by the inline script in layout.tsx.
  const [theme, setThemeState] = useState<Theme>(() => {
    if (typeof document === "undefined") return "dark";
    return document.documentElement.classList.contains("dark") ? "dark" : "light";
  });

  const applyTheme = useCallback((t: Theme) => {
    setThemeState(t);
    if (t === "dark") {
      document.documentElement.classList.add("dark");
    } else {
      document.documentElement.classList.remove("dark");
    }
    try {
      localStorage.setItem("hst-theme", t);
    } catch {
      /* ignore */
    }
  }, []);

  const setTheme = useCallback(
    (t: Theme) => {
      applyTheme(t);
    },
    [applyTheme],
  );

  // Cross-tab sync: when another tab changes the theme in localStorage, apply
  // it here too. The `storage` event only fires in *other* tabs, not the one
  // that made the change — so there's no double-apply risk.
  useEffect(() => {
    function onStorage(e: StorageEvent) {
      if (e.key === "hst-theme" && e.newValue) {
        applyTheme(e.newValue as Theme);
      }
    }
    window.addEventListener("storage", onStorage);
    return () => window.removeEventListener("storage", onStorage);
  }, [applyTheme]);

  const toggle = useCallback(() => {
    applyTheme(theme === "dark" ? "light" : "dark");
  }, [theme, applyTheme]);

  return { theme, setTheme, toggle };
}
