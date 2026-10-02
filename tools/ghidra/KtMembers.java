// Assigns unnamed functions to C++ classes.
//
// A function that takes `this` in ECX (reads ECX before writing it) and is
// only ever called from methods of one class is almost certainly a
// non-virtual member of that class. It is moved into the class namespace as
// Class::m_<address> with __thiscall. Repeats until nothing changes, since
// each assignment can make further callees attributable.
//
// Headless: -postScript KtMembers.java
// @category KakutoChojin-recomp
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class KtMembers extends GhidraScript {

    @Override
    protected void run() throws Exception {
        int total = 0;
        for (int pass = 1; pass <= 10; pass++) {
            int moved = 0;
            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) return;
                if (fn.getSymbol().getSource() != SourceType.DEFAULT || !fn.getParentNamespace().isGlobal()) continue;
                if (!readsEcxFirst(fn)) continue;
                GhidraClass owner = null;
                boolean single = true;
                Set<Function> callers = fn.getCallingFunctions(monitor);
                for (Function caller : callers) {
                    Namespace ns = caller.getParentNamespace();
                    if (!(ns instanceof GhidraClass) || (owner != null && owner != ns)) {
                        single = false;
                        break;
                    }
                    owner = (GhidraClass) ns;
                }
                if (!single || owner == null) continue;
                fn.setParentNamespace(owner);
                fn.setName(String.format("m_%08x", fn.getEntryPoint().getOffset()), SourceType.ANALYSIS);
                fn.setCallingConvention("__thiscall");
                moved++;
            }
            total += moved;
            println(String.format("KtMembers: pass %d assigned %d functions", pass, moved));
            if (moved == 0) break;
        }
        println("KtMembers: " + total + " functions assigned to classes");
    }

    // True if ECX is read before being written within the first instructions
    // of the function's entry block (i.e. it is an input).
    private boolean readsEcxFirst(Function fn) {
        InstructionIterator it = currentProgram.getListing().getInstructions(fn.getEntryPoint(), true);
        for (int i = 0; i < 12 && it.hasNext(); i++) {
            Instruction ins = it.next();
            if (!fn.getBody().contains(ins.getAddress())) break;
            String text = ins.toString();
            String mnem = ins.getMnemonicString();
            if (mnem.startsWith("J") || mnem.equals("CALL") || mnem.equals("RET")) break;
            int comma = text.indexOf(',');
            String dst = comma < 0 ? "" : text.substring(text.indexOf(' ') + 1, comma);
            String src = comma < 0 ? text.substring(text.indexOf(' ') + 1) : text.substring(comma + 1);
            if ((mnem.equals("XOR") || mnem.equals("SUB")) && dst.equals("ECX") && src.trim().equals("ECX")) {
                return false;  // zeroing idiom: a write, not a read
            }
            if (src.contains("ECX") || (dst.contains("[") && dst.contains("ECX"))) return true;
            if (mnem.equals("PUSH") && text.contains("ECX")) return true;
            if (dst.equals("ECX") || dst.equals("CX") || dst.equals("CL")) return false;  // overwritten first
        }
        return false;
    }
}
