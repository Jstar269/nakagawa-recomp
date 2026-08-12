"use client";

import { useEffect, useState, useCallback } from "react";
import { AnimatePresence, motion } from "framer-motion";
import { X, ChevronRight, ChevronLeft, Sparkles } from "lucide-react";
import { useStudio, type SectionId } from "./studio-context";
import { Button } from "@/components/ui/button";

const TOUR_KEY = "hst-tour-completed";

interface TourStep {
  section: SectionId;
  title: string;
  body: string;
  highlight: string; // text to find in the sidebar nav
}

const STEPS: TourStep[] = [
  {
    section: "iso",
    title: "1. Load your game ISO",
    body: "Drop your Hot Shots Tennis: Get a Grip UMD image here. The studio parses the real ISO9660 volume descriptor and PARAM.SFO to verify the DISC_ID — only the sectors it needs are read.",
    highlight: "Game ISO",
  },
  {
    section: "graphics",
    title: "2. Preview graphics concepts",
    body: "Explore proposed resolution, frame-rate, MSAA, and filtering controls in the court mock-up. These settings are design targets and are not connected to the native renderer.",
    highlight: "Graphics",
  },
  {
    section: "performance",
    title: "3. Preview performance concepts",
    body: "Explore proposed CPU, cache, block-linking, and memory controls. These settings currently document possible optimization work and do not change the static recompiler.",
    highlight: "Performance",
  },
  {
    section: "limitations",
    title: "4. Review PSP constraints",
    body: "Compare native PSP limits with potential host-side targets. The switches are an implementation backlog, not active limit-removal patches.",
    highlight: "PSP Limits",
  },
  {
    section: "controllers",
    title: "5. Bind a modern pad",
    body: "Preview mappings for the PSP's virtual inputs. Native SDL gamepad input is available; adaptive-trigger and gyro controls remain unimplemented concepts.",
    highlight: "Controllers",
  },
  {
    section: "patches",
    title: "6. Review patch concepts",
    body: "Review proposed game-specific patches such as texture, camera, widescreen, and save changes. None of these prototype switches currently rewrites code or game data.",
    highlight: "Game Patches",
  },
  {
    section: "build",
    title: "7. Build and run",
    body: "Use the native manager to run the real pipeline or launch the current build. Prototype packaging profiles are planning aids and do not create the previously proposed self-extracting bundles.",
    highlight: "Recompile",
  },
];

export function OnboardingTour() {
  const { setSection, section } = useStudio();
  const [open, setOpen] = useState(false);
  const [step, setStep] = useState(0);

  // Show on first visit (no localStorage flag).
  useEffect(() => {
    try {
      if (!localStorage.getItem(TOUR_KEY)) {
        const t = setTimeout(() => setOpen(true), 800);
        return () => clearTimeout(t);
      }
    } catch {
      /* ignore */
    }
  }, []);

  // Listen for a "restart tour" request (from the shortcuts dialog button).
  useEffect(() => {
    function onRestart() {
      setStep(0);
      setOpen(true);
    }
    window.addEventListener("hst-restart-tour", onRestart);
    return () => window.removeEventListener("hst-restart-tour", onRestart);
  }, []);

  const close = useCallback(() => {
    setOpen(false);
    try {
      localStorage.setItem(TOUR_KEY, "1");
    } catch {
      /* ignore */
    }
  }, []);

  const next = useCallback(() => {
    if (step >= STEPS.length - 1) {
      close();
      return;
    }
    const nextStep = step + 1;
    setStep(nextStep);
    setSection(STEPS[nextStep].section);
  }, [step, setSection, close]);

  const prev = useCallback(() => {
    if (step <= 0) return;
    const prevStep = step - 1;
    setStep(prevStep);
    setSection(STEPS[prevStep].section);
  }, [step, setSection]);

  // When the tour is open, sync the section to the current step.
  useEffect(() => {
    if (open && section !== STEPS[step].section) {
      setSection(STEPS[step].section);
    }
  }, [open, step, section, setSection]);

  const current = STEPS[step];
  const isLast = step === STEPS.length - 1;

  return (
    <AnimatePresence>
      {open ? (
        <motion.div
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          className="fixed inset-0 z-[100] flex items-end sm:items-center justify-center p-4 bg-black/60 backdrop-blur-sm"
          onClick={close}
        >
          <motion.div
            initial={{ y: 40, opacity: 0, scale: 0.96 }}
            animate={{ y: 0, opacity: 1, scale: 1 }}
            exit={{ y: 20, opacity: 0, scale: 0.96 }}
            transition={{ type: "spring", stiffness: 300, damping: 28 }}
            className="relative w-full max-w-md rounded-xl border border-border/60 bg-card glass shadow-2xl overflow-hidden"
            onClick={(e) => e.stopPropagation()}
          >
            {/* Header with gradient */}
            <div className="relative px-5 pt-4 pb-3 bg-gradient-to-br from-primary/15 via-card to-card border-b border-border/40">
              <div className="absolute -right-4 -top-4 size-20 rounded-full bg-primary/10 blur-2xl" />
              <div className="relative flex items-center gap-2">
                <div className="size-7 rounded-lg bg-primary/20 border border-primary/30 grid place-items-center">
                  <Sparkles className="size-3.5 text-primary" />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="text-[10px] uppercase tracking-wide text-muted-foreground">
                    Nakagawa Recomp · Tour
                  </div>
                  <h3 className="text-sm font-semibold leading-tight">{current.title}</h3>
                </div>
                <Button
                  variant="ghost"
                  size="sm"
                  className="size-7 p-0 shrink-0"
                  onClick={close}
                >
                  <X className="size-3.5" />
                </Button>
              </div>
            </div>

            {/* Body */}
            <div className="px-5 py-4">
              <p className="text-xs text-muted-foreground leading-relaxed">{current.body}</p>

              {/* Progress dots */}
              <div className="flex items-center gap-1.5 mt-4">
                {STEPS.map((s, i) => (
                  <button
                    key={i}
                    onClick={() => {
                      setStep(i);
                      setSection(STEPS[i].section);
                    }}
                    className={`h-1.5 rounded-full transition-all ${
                      i === step
                        ? "w-6 bg-primary"
                        : i < step
                          ? "w-1.5 bg-primary/50"
                          : "w-1.5 bg-muted-foreground/30"
                    }`}
                    aria-label={`Go to step ${i + 1}`}
                  />
                ))}
              </div>
            </div>

            {/* Footer */}
            <div className="flex items-center justify-between px-5 py-3 border-t border-border/40 bg-background/30">
              <span className="text-[10px] font-mono text-muted-foreground">
                {step + 1} / {STEPS.length}
              </span>
              <div className="flex items-center gap-1.5">
                <Button
                  variant="ghost"
                  size="sm"
                  className="h-7 gap-1 text-xs"
                  onClick={prev}
                  disabled={step === 0}
                >
                  <ChevronLeft className="size-3.5" /> Back
                </Button>
                <Button size="sm" className="h-7 gap-1 text-xs" onClick={next}>
                  {isLast ? "Finish" : "Next"}
                  {!isLast ? <ChevronRight className="size-3.5" /> : null}
                </Button>
              </div>
            </div>
          </motion.div>
        </motion.div>
      ) : null}
    </AnimatePresence>
  );
}
