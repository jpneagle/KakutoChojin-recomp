// Applies KakutoChojin-recomp annotations (tools/gen_ghidra_annotations.py) to a program
// imported from tools/xbe2elf.py: XDK function names, calling conventions and
// parameters, kernel import pointer types, and C++ class namespaces from RTTI.
//
// Headless: -postScript KtApply.java <annotations.tsv>
// @category KakutoChojin-recomp
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.ParameterDefinitionImpl;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class KtApply extends GhidraScript {

    private int functions, kernel, classes, labels, failures;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File file = args.length > 0 ? new File(args[0]) : askFile("KakutoChojin-recomp annotations", "Apply");
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            // Classes first so functions can be placed into them.
            List<String[]> records = new ArrayList<>();
            String line;
            while ((line = r.readLine()) != null) {
                if (!line.isEmpty()) records.add(line.split("\t", -1));
            }
            for (String[] f : records) if (f[0].equals("CLASS")) applyClass(f[1]);
            for (String[] f : records) {
                if (monitor.isCancelled()) return;
                try {
                    switch (f[0]) {
                        case "FUNC": applyFunction(addr(f[1]), f[2], f[3], f.length > 4 ? f[4] : ""); break;
                        case "KIMP":
                            applyKernelImport(addr(f[1]), f[2], Integer.parseInt(f[3]), f[4], addr(f[5]));
                            break;
                        case "LABEL": applyLabel(addr(f[1]), f[2]); break;
                        default: break;
                    }
                } catch (Exception e) {
                    failures++;
                    if (failures < 20) println("KtApply: " + String.join(" ", f) + ": " + e.getMessage());
                }
            }
        }
        println(String.format("KtApply: %d functions, %d kernel imports, %d classes, %d labels, %d failures",
            functions, kernel, classes, labels, failures));
    }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(hex, 16));
    }

    // "a::b::C::name" -> namespace a::b::C (created) and simple name.
    private Namespace namespaceOf(String qualified) throws Exception {
        int i = qualified.lastIndexOf("::");
        if (i < 0) return currentProgram.getGlobalNamespace();
        return NamespaceUtils.createNamespaceHierarchy(qualified.substring(0, i), null, currentProgram,
            SourceType.IMPORTED);
    }

    private static String simpleName(String qualified) {
        int i = qualified.lastIndexOf("::");
        return i < 0 ? qualified : qualified.substring(i + 2);
    }

    private void applyClass(String name) {
        if (name.startsWith(".?A")) return;  // unmangled templates are left alone
        try {
            Namespace ns = NamespaceUtils.createNamespaceHierarchy(name, null, currentProgram, SourceType.IMPORTED);
            SymbolTable st = currentProgram.getSymbolTable();
            if (!(ns instanceof ghidra.program.model.listing.GhidraClass)) st.convertNamespaceToClass(ns);
            classes++;
        } catch (Exception e) {
            failures++;
        }
    }

    private static String convention(String conv) {
        switch (conv) {
            case "stdcall": return "__stdcall";
            case "fastcall": return "__fastcall";
            case "thiscall": return "__thiscall";
            case "cdecl": return "__cdecl";
            default: return null;
        }
    }

    private void applyFunction(Address a, String conv, String qualified, String params) throws Exception {
        Function f = getFunctionAt(a);
        if (f == null) {
            disassemble(a);
            f = createFunction(a, null);
        }
        if (f == null) throw new Exception("cannot create function");
        f.setParentNamespace(namespaceOf(qualified));
        f.setName(simpleName(qualified), SourceType.IMPORTED);
        String cc = convention(conv);
        if (cc != null) f.setCallingConvention(cc);
        if (!params.isEmpty()) {
            List<Parameter> ps = new ArrayList<>();
            for (String p : params.split(",")) {
                if (cc != null && cc.equals("__thiscall") && p.equals("this")) continue;  // implicit
                ps.add(new ParameterImpl(p, Undefined4DataType.dataType, currentProgram));
            }
            f.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.IMPORTED,
                ps.toArray(new Parameter[0]));
        }
        functions++;
    }

    // Kernel import: the thunk slot points at a stub in the synthetic
    // .xboxkrnl section (see xbe2elf.py). Function exports get a typed stub so
    // calls through the slot decompile as direct named calls; data exports
    // become labelled variables.
    private void applyKernelImport(Address slot, String name, int params, String conv, Address stub)
            throws Exception {
        createLabel(slot, "__imp__" + name, true, SourceType.IMPORTED);
        clearListing(slot, slot.add(3));
        createData(slot, new PointerDataType());
        if (conv.equals("data")) {
            createLabel(stub, name, true, SourceType.IMPORTED);
        } else {
            Function f = getFunctionAt(stub);
            if (f == null) {
                disassemble(stub);
                f = createFunction(stub, name);
            }
            f.setName(name, SourceType.IMPORTED);
            f.setCallingConvention(convention(conv));
            List<Parameter> ps = new ArrayList<>();
            for (int i = 0; i < Math.max(params, 0); i++) {
                ps.add(new ParameterImpl("arg" + (i + 1), Undefined4DataType.dataType, currentProgram));
            }
            f.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.IMPORTED,
                ps.toArray(new Parameter[0]));
            f.setReturnType(Undefined4DataType.dataType, SourceType.IMPORTED);
            if (conv.equals("cdecl")) f.setVarArgs(true);
        }
        kernel++;
    }

    private void applyLabel(Address a, String qualified) throws Exception {
        currentProgram.getSymbolTable().createLabel(a, simpleName(qualified), namespaceOf(qualified),
            SourceType.IMPORTED);
        labels++;
    }
}
