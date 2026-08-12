"use client";

import { AnimatePresence, motion } from "framer-motion";
import { StudioProvider, useStudio } from "@/components/studio/studio-context";
import { Topbar } from "@/components/studio/topbar";
import { Sidebar, MobileNav } from "@/components/studio/sidebar";
import { Footer } from "@/components/studio/footer";
import { SummaryRail } from "@/components/studio/summary-rail";
import { PresetBar } from "@/components/studio/preset-bar";
import { IsoLoader } from "@/components/studio/iso-loader";
import { GraphicsPanel } from "@/components/studio/graphics-panel";
import { PerformancePanel } from "@/components/studio/performance-panel";
import { LimitationsPanel } from "@/components/studio/limitations-panel";
import { ControllersPanel } from "@/components/studio/controllers-panel";
import { PatchesPanel } from "@/components/studio/patches-panel";
import { BuildPanel } from "@/components/studio/build-panel";
import { InternalsPanel } from "@/components/studio/internals-panel";
import { ProgressPanel } from "@/components/studio/progress-panel";
import { PortingPanel } from "@/components/studio/porting-panel";
import { TroubleshootingPanel } from "@/components/studio/troubleshooting-panel";
import { AssetsPanel } from "@/components/studio/assets-panel";
import { VisualRegressionPanel } from "@/components/studio/visual-regression-panel";
import { TestLabPanel } from "@/components/studio/test-lab-panel";
import { BuildHealthPanel } from "@/components/studio/build-health-panel";
import { ProfilerPanel } from "@/components/studio/profiler-panel";

function MainContent() {
  const { section } = useStudio();
  const panels: Record<string, React.ReactNode> = {
    iso: <IsoLoader />,
    graphics: <GraphicsPanel />,
    performance: <PerformancePanel />,
    limitations: <LimitationsPanel />,
    controllers: <ControllersPanel />,
    patches: <PatchesPanel />,
    build: (
      <div className="space-y-4">
        <BuildPanel />
      </div>
    ),
    internals: <InternalsPanel />,
    assets: <AssetsPanel />,
    progress: <ProgressPanel />,
    porting: <PortingPanel />,
    troubleshooting: <TroubleshootingPanel />,
    "visual-regression": <VisualRegressionPanel />,
    "test-lab": <TestLabPanel />,
    "build-health": <BuildHealthPanel />,
    profiler: <ProfilerPanel />,
  };
  return (
    <AnimatePresence mode="wait">
      <motion.div
        key={section}
        initial={{ opacity: 0, y: 8 }}
        animate={{ opacity: 1, y: 0 }}
        exit={{ opacity: 0, y: -6 }}
        transition={{ duration: 0.18, ease: "easeOut" }}
      >
        {panels[section] ?? <IsoLoader />}
      </motion.div>
    </AnimatePresence>
  );
}

function StudioShell() {
  const { setSection, section } = useStudio();
  // Presets describe planned enhancement controls, so keep them off the live
  // diagnostics/build pages where they would imply that those settings apply.
  const showPresets = ["graphics", "performance", "limitations", "controllers", "patches"].includes(section);

  return (
    <div className="min-h-screen flex flex-col">
      <Topbar />
      <div className="flex-1 w-full">
        <div className="mx-auto max-w-[1500px] px-4 py-4">
          <MobileNav />
          <div className="grid grid-cols-1 lg:grid-cols-[248px_minmax(0,1fr)_320px] gap-4 mt-3 lg:mt-4">
            <div className="hidden lg:block sticky top-[72px] self-start max-h-[calc(100vh-88px)]">
              <Sidebar />
            </div>
            <main className="min-w-0 space-y-4">
              {showPresets ? <PresetBar /> : null}
              <MainContent />
            </main>
            <div className="hidden lg:block sticky top-[72px] self-start max-h-[calc(100vh-88px)] overflow-y-auto thin-scroll">
              <SummaryRail />
            </div>
          </div>
        </div>
      </div>
      <Footer onRecompile={() => setSection("build")} />
    </div>
  );
}

export default function Home() {
  return (
    <StudioProvider>
      <StudioShell />
    </StudioProvider>
  );
}
