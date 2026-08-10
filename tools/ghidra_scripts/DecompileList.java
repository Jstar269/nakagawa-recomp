/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * Decompile a list of addresses to <outdir>/<addr>.c, one file each.
 * Args: <outdir> <addr> [<addr> ...]  (addresses in Ghidra's address space,
 * i.e. including the image base — tools/ghidra_headless.py handles the
 * base-0 -> imageBase translation for you).
 *
 * Output is a local analysis aid derived from the game binary: never commit
 * it. It lands under third_party/ghidra/exports/ which is gitignored.
 */
//@category HSTRecomp

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.PrintWriter;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileList <outdir> <addr> [<addr> ...]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try {
            for (int i = 1; i < args.length; i++) {
                long a = Long.decode(args[i]);
                Address addr = toAddr(a);
                Function f = getFunctionAt(addr);
                if (f == null) {
                    f = getFunctionContaining(addr);
                }
                String tag = String.format("0x%08x", a);
                PrintWriter w = new PrintWriter(new File(outDir, tag + ".c"), "UTF-8");
                if (f == null) {
                    w.println("/* DecompileList: no function at or containing " + tag + " */");
                    println("DecompileList: NO FUNCTION at " + tag);
                } else {
                    DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                    if (res != null && res.decompileCompleted()) {
                        w.println("/* " + f.getName() + " @ " + f.getEntryPoint()
                                + " (requested " + tag + ") */");
                        w.print(res.getDecompiledFunction().getC());
                        println("DecompileList: " + tag + " -> " + f.getName());
                    } else {
                        String err = (res != null) ? res.getErrorMessage() : "no result";
                        w.println("/* DecompileList: decompile FAILED for " + f.getName()
                                + ": " + err + " */");
                        println("DecompileList: FAILED " + tag + ": " + err);
                    }
                }
                w.close();
            }
        } finally {
            ifc.dispose();
        }
    }
}
