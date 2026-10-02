// Recovers C++ class layouts.
//
//  1. For every method of every RTTI class, the decompiler's structure
//     recovery (FillOutStructureHelper) is run on `this`, growing the class
//     structure with the fields the method accesses (it also follows calls
//     that receive `this`).
//  2. Each primary vtable becomes a <Class>_vtbl structure of typed function
//     pointers, referenced by a `vftable` field at offset 0.
//  3. Fields of a primary base class (at offset 0) are copied into derived
//     classes where those are still undefined.
//
// Headless: -postScript KtTypes.java <annotations.tsv> [decompile timeout seconds]
// @category KakutoChojin-recomp
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.util.FillOutStructureHelper;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.VariableUtilities;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolType;

public class KtTypes extends GhidraScript {

    private static class ClassInfo {
        String name;
        String primaryBase;  // base at offset 0, if any
        List<Long> primaryVtable = new ArrayList<>();
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File file = args.length > 0 ? new File(args[0]) : askFile("KakutoChojin-recomp annotations", "Use");
        int timeout = args.length > 1 ? Integer.parseInt(args[1]) : 30;
        Map<String, ClassInfo> classes = new LinkedHashMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split("\t", -1);
                if (f[0].equals("CLASS") && !f[1].startsWith(".?A")) {
                    ClassInfo c = classes.computeIfAbsent(f[1], k -> new ClassInfo());
                    c.name = f[1];
                    for (String b : f[2].split(";")) {
                        if (b.endsWith(":0") && c.primaryBase == null) c.primaryBase = b.substring(0, b.length() - 2);
                    }
                } else if (f[0].equals("VTABLE") && f[3].equals("0") && classes.containsKey(f[1])) {
                    for (String m : f[4].split(",")) if (!m.isEmpty()) classes.get(f[1]).primaryVtable.add(Long.parseLong(m, 16));
                }
            }
        }

        DataTypeManager dtm = currentProgram.getDataTypeManager();
        FillOutStructureHelper helper = new FillOutStructureHelper(currentProgram, monitor);
        DecompileOptions opts = new DecompileOptions();
        DecompInterface dec = helper.setUpDecompiler(opts);

        int filled = 0, methods = 0, vtables = 0, inherited = 0;
        Map<String, Structure> structs = new HashMap<>();
        for (ClassInfo c : classes.values()) {
            if (monitor.isCancelled()) break;
            Namespace ns = getNamespace(c.name);
            if (!(ns instanceof GhidraClass)) continue;
            Structure st = VariableUtilities.findOrCreateClassStruct((GhidraClass) ns, dtm);
            structs.put(c.name, st);
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(ns)) {
                if (s.getSymbolType() != SymbolType.FUNCTION) continue;
                Function fn = getFunctionAt(s.getAddress());
                if (fn == null || !"__thiscall".equals(fn.getCallingConventionName())) continue;
                methods++;
                try {
                    DecompileResults res = dec.decompileFunction(fn, timeout, monitor);
                    HighFunction hf = res == null ? null : res.getHighFunction();
                    if (hf == null || hf.getLocalSymbolMap().getNumParams() == 0) continue;
                    HighSymbol self = hf.getLocalSymbolMap().getParamSymbol(0);
                    if (self == null || self.getHighVariable() == null) continue;
                    if (helper.processStructure(self.getHighVariable(), fn, false, false, dec) != null) filled++;
                } catch (Exception e) {
                    // structure recovery is best effort per method
                }
            }
        }

        // Typed vtables.
        for (ClassInfo c : classes.values()) {
            Structure st = structs.get(c.name);
            if (st == null || c.primaryVtable.isEmpty()) continue;
            CategoryPath cat = st.getCategoryPath();
            StructureDataType vt = new StructureDataType(cat, st.getName() + "_vtbl", 0, dtm);
            for (int i = 0; i < c.primaryVtable.size(); i++) {
                Function m = getFunctionAt(toAddr(c.primaryVtable.get(i)));
                DataType fp = m != null ? new PointerDataType(new FunctionDefinitionDataType(m, true), dtm)
                                        : new PointerDataType(dtm);
                vt.add(fp, 4, m != null ? m.getName() : "vf" + i, null);
            }
            DataType vtResolved = dtm.addDataType(vt, DataTypeConflictHandler.REPLACE_HANDLER);
            if (st.isZeroLength() || st.getLength() < 4) st.growStructure(4 - (st.isZeroLength() ? 0 : st.getLength()));
            DataTypeComponent c0 = st.getComponentAt(0);
            if (c0 == null || Undefined.isUndefined(c0.getDataType()) || c0.getDataType() instanceof ghidra.program.model.data.Pointer) {
                st.replaceAtOffset(0, new PointerDataType(vtResolved, dtm), 4, "vftable", null);
                vtables++;
            }
        }

        // Inherit fields from primary bases (bases before derived classes).
        boolean changed = true;
        for (int pass = 0; pass < 8 && changed; pass++) {
            changed = false;
            for (ClassInfo c : classes.values()) {
                Structure st = structs.get(c.name), base = c.primaryBase == null ? null : structs.get(c.primaryBase);
                if (st == null || base == null || base.isZeroLength()) continue;
                if (st.isZeroLength() || st.getLength() < base.getLength()) {
                    st.growStructure(base.getLength() - (st.isZeroLength() ? 0 : st.getLength()));
                }
                for (DataTypeComponent bc : base.getDefinedComponents()) {
                    if (bc.getOffset() == 0) continue;  // keep the derived vftable type
                    // Only scalar/pointer fields: embedded composites may still change size.
                    if (bc.getDataType() instanceof ghidra.program.model.data.Composite) continue;
                    DataTypeComponent dc = st.getComponentAt(bc.getOffset());
                    if (dc != null && Undefined.isUndefined(dc.getDataType()) && dc.getLength() <= bc.getLength()) {
                        try {
                            st.replaceAtOffset(bc.getOffset(), bc.getDataType(), bc.getLength(), bc.getFieldName(),
                                "from " + c.primaryBase);
                            inherited++;
                            changed = true;
                        } catch (Exception e) {
                            // overlapping fields: leave the derived class's view
                        }
                    }
                }
            }
        }
        // Embedded composites whose size changed after placement no longer fit
        // and break decompilation of every user of the structure: clear them.
        int cleared = 0;
        for (Structure st : structs.values()) {
            for (DataTypeComponent dc : st.getDefinedComponents()) {
                DataType dt = dc.getDataType();
                if (dt instanceof ghidra.program.model.data.Composite && dt.getLength() != dc.getLength()) {
                    st.clearAtOffset(dc.getOffset());
                    cleared++;
                }
            }
        }
        if (cleared > 0) println("KtTypes: cleared " + cleared + " stale embedded fields");
        dec.dispose();
        println(String.format("KtTypes: %d classes, %d methods, %d structure passes, %d vtables, %d inherited fields",
            structs.size(), methods, filled, vtables, inherited));
    }

    private Namespace getNamespace(String qualified) {
        try {
            for (Namespace ns : ghidra.app.util.NamespaceUtils.getNamespaceByPath(currentProgram, null, qualified)) {
                if (ns instanceof GhidraClass) return ns;
            }
        } catch (Exception e) {
            // fall through
        }
        return null;
    }
}
