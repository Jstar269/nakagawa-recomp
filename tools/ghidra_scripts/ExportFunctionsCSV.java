/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * Export every function Ghidra knows about as CSV: entry,size,name,thunk.
 * Header comment lines carry the image base and memory-block map so consumers
 * (tools/ghidra_crosscheck.py) can normalize Ghidra's address space against
 * the recomp pipeline's base-0 view of the same ELF.
 *
 * Run headlessly via tools/ghidra_headless.py after installing the optional
 * local Ghidra extension, or:
 *   analyzeHeadless <proj> HST -process EBOOT.elf -noanalysis \
 *     -scriptPath tools/ghidra_scripts -postScript ExportFunctionsCSV.java <out.csv>
 */
//@category HSTRecomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import java.io.File;
import java.io.PrintWriter;

public class ExportFunctionsCSV extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "functions.csv");
        PrintWriter w = new PrintWriter(out, "UTF-8");
        w.println(String.format("# imageBase=0x%08x", currentProgram.getImageBase().getOffset()));
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            w.println(String.format("# block name=%s start=0x%08x end=0x%08x exec=%d initialized=%d",
                    b.getName().replace(" ", "_"),
                    b.getStart().getOffset(), b.getEnd().getOffset(),
                    b.isExecute() ? 1 : 0, b.isInitialized() ? 1 : 0));
        }
        w.println("entry,size,name,thunk");
        int n = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            w.println(String.format("0x%08x,%d,%s,%d",
                    f.getEntryPoint().getOffset(),
                    f.getBody().getNumAddresses(),
                    f.getName().replace(",", "_"),
                    f.isThunk() ? 1 : 0));
            n++;
        }
        w.close();
        println("ExportFunctionsCSV: wrote " + n + " functions to " + out.getAbsolutePath());
    }
}
