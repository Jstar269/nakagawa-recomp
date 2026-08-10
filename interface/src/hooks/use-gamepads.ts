"use client";

import { useCallback, useEffect, useRef, useState } from "react";

export interface GamepadInfo {
  index: number;
  id: string;
  vendor: string;
  product: string;
  displayName: string;
  buttonCount: number;
  axesCount: number;
  hasGyro: boolean;
  lastPressed: string | null;
  lastPressedAt: number;
  // live button state for the visualizer
  pressedButtons: boolean[];
}

// Try to map raw gamepad.id into a friendly device name + vendor/product.
function identify(id: string): {
  displayName: string;
  vendor: string;
  product: string;
  hasGyro: boolean;
} {
  const lower = id.toLowerCase();
  if (lower.includes("054c")) {
    // Sony
    if (lower.includes("0ce6") || lower.includes("dualsense"))
      return { displayName: "DualSense (PS5)", vendor: "054c", product: "0ce6", hasGyro: true };
    if (lower.includes("09cc") || lower.includes("ds4"))
      return { displayName: "DualShock 4", vendor: "054c", product: "09cc", hasGyro: true };
    return { displayName: "Sony Controller", vendor: "054c", product: "????", hasGyro: true };
  }
  if (lower.includes("045e")) {
    // Microsoft
    if (lower.includes("0b00") || lower.includes("series"))
      return { displayName: "Xbox Series Controller", vendor: "045e", product: "0b00", hasGyro: false };
    if (lower.includes("02fd") || lower.includes("elite"))
      return { displayName: "Xbox Elite Series 2", vendor: "045e", product: "02fd", hasGyro: false };
    if (lower.includes("02ea") || lower.includes("one"))
      return { displayName: "Xbox One Controller", vendor: "045e", product: "02ea", hasGyro: false };
    return { displayName: "Xbox Controller", vendor: "045e", product: "????", hasGyro: false };
  }
  if (lower.includes("057e")) {
    // Nintendo
    if (lower.includes("2009") || lower.includes("pro"))
      return { displayName: "Switch Pro Controller", vendor: "057e", product: "2009", hasGyro: true };
    return { displayName: "Nintendo Controller", vendor: "057e", product: "????", hasGyro: true };
  }
  return { displayName: id.slice(0, 40) || "Unknown Gamepad", vendor: "????", product: "????", hasGyro: false };
}

export function useGamepads() {
  const [gamepads, setGamepads] = useState<GamepadInfo[]>([]);
  // Lazy initial state: determine support once on mount without a setState-in-effect.
  const [supported] = useState<boolean>(() =>
    typeof navigator !== "undefined" && typeof navigator.getGamepads === "function",
  );
  const rafRef = useRef<number | null>(null);
  const lastPressedRef = useRef<Record<number, { button: string; at: number }>>({});

  const scan = useCallback(() => {
    if (typeof navigator === "undefined" || !navigator.getGamepads) return;
    const pads = navigator.getGamepads();
    const list: GamepadInfo[] = [];
    for (const pad of pads) {
      if (!pad) continue;
      const meta = identify(pad.id);
      const pressed = pad.buttons.map((b) => b.pressed);
      // detect newly pressed button
      for (let i = 0; i < pad.buttons.length; i++) {
        if (pad.buttons[i].pressed) {
          const prev = lastPressedRef.current[pad.index];
          if (!prev || prev.button !== `BTN_${i}` || Date.now() - prev.at > 200) {
            lastPressedRef.current[pad.index] = { button: `BTN_${i}`, at: Date.now() };
          }
        }
      }
      const lp = lastPressedRef.current[pad.index];
      list.push({
        index: pad.index,
        id: pad.id,
        vendor: meta.vendor,
        product: meta.product,
        displayName: meta.displayName,
        buttonCount: pad.buttons.length,
        axesCount: pad.axes.length,
        hasGyro: meta.hasGyro,
        lastPressed: lp ? lp.button : null,
        lastPressedAt: lp ? lp.at : 0,
        pressedButtons: pressed,
      });
    }
    setGamepads(list);
  }, []);

  useEffect(() => {
    if (!supported) return;

    const onConnect = () => scan();
    const onDisconnect = () => scan();
    window.addEventListener("gamepadconnected", onConnect);
    window.addEventListener("gamepaddisconnected", onDisconnect);

    const loop = () => {
      scan();
      rafRef.current = requestAnimationFrame(loop);
    };
    rafRef.current = requestAnimationFrame(loop);

    return () => {
      window.removeEventListener("gamepadconnected", onConnect);
      window.removeEventListener("gamepaddisconnected", onDisconnect);
      if (rafRef.current) cancelAnimationFrame(rafRef.current);
    };
  }, [scan, supported]);

  return { gamepads, supported };
}

// Standard button index -> friendly name (W3C standard gamepad mapping).
export const STD_BUTTON_NAMES = [
  "A", // 0
  "B", // 1
  "X", // 2
  "Y", // 3
  "LB", // 4
  "RB", // 5
  "LT", // 6
  "RT", // 7
  "BACK", // 8
  "START", // 9
  "L3", // 10
  "R3", // 11
  "DPAD_UP", // 12
  "DPAD_DOWN", // 13
  "DPAD_LEFT", // 14
  "DPAD_RIGHT", // 15
  "HOME", // 16
];
