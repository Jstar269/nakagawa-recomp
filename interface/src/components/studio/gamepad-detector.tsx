"use client";

import { useEffect } from "react";
import { Gamepad2, Usb, Crosshair, CircleCheck, CircleAlert, Radio } from "lucide-react";
import { useGamepads, STD_BUTTON_NAMES } from "@/hooks/use-gamepads";
import { useStudio } from "./studio-context";
import { Panel } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import type { ControllerDevice } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";

// Map a connected gamepad's vendor to our device scheme.
function detectDevice(displayName: string): ControllerDevice | null {
  const n = displayName.toLowerCase();
  if (n.includes("dualsense")) return "dualsense";
  if (n.includes("dualshock")) return "dualsense";
  if (n.includes("elite")) return "xbox-elite";
  if (n.includes("xbox") || n.includes("series")) return "xbox-series";
  if (n.includes("switch") || n.includes("pro")) return "switch-pro";
  if (n.includes("xinput")) return "generic-xinput";
  return null;
}

export function GamepadDetector() {
  const { gamepads, supported } = useGamepads();
  const {
    config,
    updateControllers,
    captureTarget,
    capturePadIdx,
    startCapture,
    stopCapture,
  } = useStudio();
  const { bindings } = config.controllers;

  // Capture loop: when a target is set, poll the chosen pad for the next
  // pressed button and assign it.
  useEffect(() => {
    if (capturePadIdx === null || !captureTarget) return;
    let cancelled = false;
    const id = window.setInterval(() => {
      if (cancelled) return;
      if (typeof navigator === "undefined" || !navigator.getGamepads) return;
      const pad = navigator.getGamepads()[capturePadIdx];
      if (!pad) return;
      for (let i = 0; i < pad.buttons.length; i++) {
        if (pad.buttons[i].pressed) {
          const btnName = STD_BUTTON_NAMES[i] ?? `BTN_${i}`;
          const next = bindings.map((b) =>
            b.pspAction === captureTarget ? { ...b, mappedTo: btnName } : b,
          );
          updateControllers({ bindings: next });
          stopCapture();
          window.clearInterval(id);
          return;
        }
      }
    }, 60);
    return () => {
      cancelled = true;
      window.clearInterval(id);
    };
  }, [capturePadIdx, captureTarget, bindings, updateControllers, stopCapture]);

  return (
    <Panel
      title="Live gamepad detection"
      description="Connect a controller and press a button to bind it"
      icon={<Usb className="size-4" />}
    >
      {!supported ? (
        <div className="flex items-center gap-2 rounded-lg bg-amber-500/10 border border-amber-500/20 px-3 py-2.5">
          <CircleAlert className="size-4 text-amber-300 shrink-0" />
          <p className="text-xs text-amber-200/90">
            The Gamepad API isn&apos;t available in this browser. Use Chrome/Edge over HTTPS to
            enable live detection and one-press binding.
          </p>
        </div>
      ) : gamepads.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-6 text-center">
          <div className="size-10 rounded-xl bg-muted/40 border border-border grid place-items-center mb-2">
            <Gamepad2 className="size-5 text-muted-foreground" />
          </div>
          <p className="text-xs font-medium">No controller detected</p>
          <p className="text-[10px] text-muted-foreground mt-0.5 max-w-xs">
            Press any button on a connected gamepad to wake it up — browsers only expose pads after
            a button press.
          </p>
        </div>
      ) : (
        <div className="space-y-2">
          {gamepads.map((gp) => {
            const dev = detectDevice(gp.displayName);
            const isCurrent = config.controllers.device === dev;
            return (
              <div
                key={gp.index}
                className={cn(
                  "rounded-lg border p-3 transition-colors",
                  isCurrent
                    ? "border-primary/40 bg-primary/5"
                    : "border-border/60 bg-background/30",
                )}
              >
                <div className="flex items-center gap-2 flex-wrap">
                  <div
                    className={cn(
                      "size-8 rounded-md grid place-items-center border shrink-0",
                      gp.pressedButtons.some(Boolean)
                        ? "bg-primary/20 border-primary/50 text-primary animate-pulse"
                        : "bg-muted/40 border-border text-muted-foreground",
                    )}
                  >
                    <Gamepad2 className="size-4" />
                  </div>
                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2 flex-wrap">
                      <span className="text-xs font-semibold truncate">{gp.displayName}</span>
                      {isCurrent ? (
                        <Badge className="bg-primary/15 text-primary border border-primary/30 hover:bg-primary/15">
                          <CircleCheck className="size-3 mr-1" /> active
                        </Badge>
                      ) : null}
                      {dev && !isCurrent ? (
                        <Button
                          size="sm"
                          variant="outline"
                          className="h-6 text-[10px] gap-1"
                          onClick={() => updateControllers({ device: dev })}
                        >
                          <Radio className="size-3" /> Use this
                        </Button>
                      ) : null}
                    </div>
                    <div className="flex items-center gap-1.5 mt-1 flex-wrap">
                      <Badge variant="outline" className="text-[9px] h-4 px-1 font-mono">
                        VID:{gp.vendor}
                      </Badge>
                      <Badge variant="outline" className="text-[9px] h-4 px-1 font-mono">
                        PID:{gp.product}
                      </Badge>
                      <Badge variant="outline" className="text-[9px] h-4 px-1 font-mono">
                        {gp.buttonCount} btns
                      </Badge>
                      <Badge variant="outline" className="text-[9px] h-4 px-1 font-mono">
                        {gp.axesCount} axes
                      </Badge>
                      {gp.hasGyro ? (
                        <Badge
                          variant="outline"
                          className="text-[9px] h-4 px-1 text-emerald-300 border-emerald-500/30"
                        >
                          gyro
                        </Badge>
                      ) : null}
                    </div>
                  </div>
                </div>

                {/* live button grid */}
                <div className="mt-2.5 flex flex-wrap gap-1">
                  {gp.pressedButtons.slice(0, 17).map((on, i) => (
                    <span
                      key={i}
                      className={cn(
                        "size-6 rounded grid place-items-center text-[8px] font-mono border transition-colors",
                        on
                          ? "bg-primary text-primary-foreground border-primary"
                          : "bg-muted/30 border-border text-muted-foreground",
                      )}
                      title={STD_BUTTON_NAMES[i] ?? `BTN_${i}`}
                    >
                      {STD_BUTTON_NAMES[i]?.slice(0, 2) ?? i}
                    </span>
                  ))}
                </div>
                {gp.lastPressed ? (
                  <p className="text-[10px] text-muted-foreground mt-2 font-mono">
                    Last pressed: <span className="text-ball">{gp.lastPressed}</span>
                  </p>
                ) : null}
              </div>
            );
          })}

          {/* Capture-mode banner */}
          {captureTarget ? (
            <div className="rounded-lg bg-primary/10 border border-primary/30 px-3 py-2.5 flex items-center gap-2">
              <Crosshair className="size-4 text-primary animate-pulse" />
              <span className="text-xs">
                Press any button on gamepad{" "}
                <span className="font-mono text-ball">#{capturePadIdx}</span> to bind it to{" "}
                <span className="font-mono text-ball">{captureTarget}</span>…
              </span>
              <Button
                size="sm"
                variant="ghost"
                className="h-6 ml-auto text-[10px]"
                onClick={stopCapture}
              >
                Cancel
              </Button>
            </div>
          ) : null}
        </div>
      )}

      {supported && gamepads.length > 0 && captureTarget === null ? (
        <p className="text-[10px] text-muted-foreground mt-2">
          Tip: click a binding&apos;s <span className="font-mono text-ball">capture</span> button in
          the table below, then press a button on your pad.
        </p>
      ) : null}
    </Panel>
  );
}
