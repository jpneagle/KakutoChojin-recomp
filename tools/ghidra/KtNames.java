// Names constructors and destructors recovered from RTTI vtables.
//
// MSVC constructors and destructors store the class's vtable pointer into
// the object. An unnamed function doing that becomes Class::Class, or
// Class::~Class when it is called from the class's vtable slot 0 (the scalar
// deleting destructor, which is renamed accordingly).
//
// Headless: -postScript KtNames.java <annotations.tsv>
// @category KakutoChojin-recomp
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class KtNames extends GhidraScript {

    private final Map<Long, String> vtableClass = new HashMap<>();
    private final Map<String, Address> slot0 = new HashMap<>();  // class -> vtable[0] method

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File file = args.length > 0 ? new File(args[0]) : askFile("KakutoChojin-recomp annotations", "Use");
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split("\t", -1);
                if (!f[0].equals("VTABLE") || f[1].startsWith(".?A")) continue;
                vtableClass.put(Long.parseLong(f[2], 16), f[1]);
                if (f[3].equals("0") && !f[4].isEmpty()) slot0.put(f[1], toAddr(Long.parseLong(f[4].split(",")[0], 16)));
            }
        }

        // Function ID names CRT functions with MSVC-mangled names; demangle them.
        int demangled = 0;
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            String name = fn.getName();
            if (!name.startsWith("?")) continue;
            try {
                // The program-aware lookup picks demanglers by format (ELF -> GNU);
                // the generic one tries all, including Microsoft's.
                ghidra.app.util.demangler.DemangledObject d = ghidra.app.util.demangler.DemanglerUtil.demangle(name);
                if (d != null && d.applyTo(currentProgram, fn.getEntryPoint(),
                                           new ghidra.app.util.demangler.DemanglerOptions(), monitor)) {
                    demangled++;
                }
            } catch (Exception e) {
                // leave the mangled name
            }
        }
        println("KtNames: demangled " + demangled + " library function names");

        // Which unnamed functions store which vtables, in order.
        Map<Function, List<String>> writers = new HashMap<>();
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) return;
            InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
            List<String> stored = new ArrayList<>();
            // Registers holding `this` (ECX on entry, and copies of it). Only
            // stores through them count, which excludes functions that merely
            // construct some other object inline (e.g. factories after new).
            Set<String> self = new HashSet<>();
            self.add("ECX");
            while (it.hasNext()) {
                Instruction ins = it.next();
                String text = ins.toString();
                if (text.startsWith("MOV ") && ins.getNumOperands() == 2) {
                    String[] ops = text.substring(4).split(",");
                    if (ops.length == 2 && self.contains(ops[1].trim()) && !ops[0].contains("[")) self.add(ops[0].trim());
                }
                // MOV dword ptr [<this reg>], <vtable address>
                if (!text.startsWith("MOV dword ptr [") || ins.getNumOperands() != 2) continue;
                String base = text.substring("MOV dword ptr [".length(), text.indexOf(']'));
                if (!self.contains(base)) continue;
                Object[] src = ins.getOpObjects(1);
                if (src.length != 1 || !(src[0] instanceof Scalar)) continue;
                String cls = vtableClass.get(((Scalar) src[0]).getUnsignedValue());
                if (cls != null) stored.add(cls);
            }
            if (!stored.isEmpty()) writers.put(fn, stored);
        }

        // Destructors: called from some class's vtable slot 0.
        Map<Function, String> dtorOf = new HashMap<>();
        for (Map.Entry<String, Address> e : slot0.entrySet()) {
            Function s0 = getFunctionAt(e.getValue());
            if (s0 == null) continue;
            for (Function callee : s0.getCalledFunctions(monitor)) {
                List<String> stored = writers.get(callee);
                if (stored != null && stored.get(0).equals(e.getKey())) dtorOf.put(callee, e.getKey());
            }
        }

        int ctors = 0, dtors = 0, deleting = 0;
        Set<String> used = new HashSet<>();
        for (Map.Entry<Function, List<String>> e : writers.entrySet()) {
            Function fn = e.getKey();
            if (fn.getSymbol().getSource() != SourceType.DEFAULT) continue;  // keep real names
            String cls = dtorOf.get(fn);
            boolean dtor = cls != null;
            if (!dtor) cls = e.getValue().get(e.getValue().size() - 1);  // the derived ctor writes last
            int sep = cls.lastIndexOf("::");
            String simple = sep < 0 ? cls : cls.substring(sep + 2);
            String name = (dtor ? "~" : "") + simple;
            String key = cls + "::" + name;
            for (int n = 2; used.contains(key); n++) key = cls + "::" + name + "_" + n;
            used.add(key);
            Namespace ns = NamespaceUtils.createNamespaceHierarchy(cls, null, currentProgram, SourceType.ANALYSIS);
            fn.setParentNamespace(ns);
            fn.setName(key.substring(cls.length() + 2), SourceType.ANALYSIS);
            fn.setCallingConvention("__thiscall");
            if (dtor) dtors++;
            else ctors++;
        }
        // Slot-0 functions that call a destructor are scalar deleting destructors.
        for (Map.Entry<String, Address> e : slot0.entrySet()) {
            Function s0 = getFunctionAt(e.getValue());
            if (s0 == null || !s0.getName().matches("vf0")) continue;
            for (Function callee : s0.getCalledFunctions(monitor)) {
                if (e.getKey().equals(dtorOf.get(callee))) {
                    s0.setName("scalar_deleting_destructor", SourceType.ANALYSIS);
                    deleting++;
                    break;
                }
            }
        }
        println(String.format("KtNames: %d constructors, %d destructors, %d scalar deleting destructors", ctors, dtors,
            deleting));
    }
}
