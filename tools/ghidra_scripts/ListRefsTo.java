/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * For each address argument, print every reference Ghidra knows that points
 * AT it (from-address, reference type, and the containing function of the
 * source), plus the first instruction at the target. Used to triage how a
 * function analyze.py missed is actually reached (direct call, data pointer /
 * vtable, or nothing = likely dead code).
 *
 * Args: <addr> [<addr> ...]   (Ghidra address space)
 */
//@category HSTRecomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ListRefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address addr = toAddr(Long.decode(arg));
            Instruction ins = getInstructionAt(addr);
            println(String.format("TARGET 0x%08x  first-insn=%s",
                    addr.getOffset(), ins != null ? ins.toString() : "(none)"));
            ReferenceIterator it =
                    currentProgram.getReferenceManager().getReferencesTo(addr);
            int n = 0;
            while (it.hasNext()) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function ff = getFunctionContaining(from);
                println(String.format("  REF from=0x%08x type=%s in=%s",
                        from.getOffset(), r.getReferenceType(),
                        ff != null ? ff.getName() : from.isMemoryAddress() ? "(no function)" : "(non-mem)"));
                n++;
            }
            if (n == 0) {
                println("  (no references)");
            }
        }
    }
}
