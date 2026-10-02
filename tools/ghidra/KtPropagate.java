// Gives descriptive names to functions that are still unnamed (FUN_*).
//
// Rules, in order of confidence (each only touches default-named functions):
//   stl      references an MSVC STL exception string ("vector<T> too long",
//            "invalid map/set<T> iterator", ...)      -> stl_vector_<addr>
//   wrap     short function whose only call is to one named function
//                                                       -> <callee>_wrap
//   helper   non-member function called only from one class's methods
//                                                       -> Class::helper_<addr>
//   str      references identifier-like strings        -> ref_<STRING>_<addr>
//   module   lies between functions of the same class in the binary (the
//            linker keeps each object file's code together)
//                                                       -> Class::mod_<addr>
//   ns       callers (from several classes) all inside one namespace
//                                                       -> ns::fn_<addr>
// wrap and helper are repeated after every other rule until nothing changes,
// since each new name can make more functions attributable.
//
// Headless: -postScript KtPropagate.java
// @category KakutoChojin-recomp
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class KtPropagate extends GhidraScript {

    private static final Pattern STL = Pattern.compile("^(?:invalid )?(vector|deque|list|map/set|string)<T>");
    private static final Pattern IDENT = Pattern.compile("^[A-Za-z_][A-Za-z0-9_]{2,31}$");

    private final Map<String, Integer> counts = new HashMap<>();

    @Override
    protected void run() throws Exception {
        for (Function fn : functions()) {
            if (!unnamed(fn)) continue;
            List<String> strings = referencedStrings(fn);
            for (String s : strings) {
                Matcher m = STL.matcher(s);
                if (m.find()) {
                    rename(fn, null, "stl_" + m.group(1).replace("map/set", "tree") + "_" + addr(fn), "stl");
                    break;
                }
            }
        }

        propagate();

        for (Function fn : functions()) {
            if (!unnamed(fn)) continue;
            for (String s : referencedStrings(fn)) {
                if (IDENT.matcher(s).matches()) {
                    rename(fn, null, "ref_" + s + "_" + addr(fn), "str");
                    break;
                }
            }
        }

        // Module membership from neighbours in address order.
        List<Function> all = new ArrayList<>();
        for (Function fn : functions()) if (!fn.isThunk() && !fn.isExternal()) all.add(fn);
        Map<Function, GhidraClass> module = new HashMap<>();
        for (int i = 0; i < all.size(); i++) {
            if (!unnamed(all.get(i))) continue;
            GhidraClass prev = nearestNamedClass(all, i, -1), next = nearestNamedClass(all, i, 1);
            if (prev != null && prev == next) module.put(all.get(i), prev);
        }
        for (Map.Entry<Function, GhidraClass> e : module.entrySet()) {
            rename(e.getKey(), e.getValue(), "mod_" + addr(e.getKey()), "module");
        }
        propagate();

        for (Function fn : functions()) {
            if (!unnamed(fn)) continue;
            Namespace ns = commonCallerNamespace(fn);
            if (ns != null) rename(fn, ns, "fn_" + addr(fn), "ns");
        }
        propagate();

        StringBuilder sb = new StringBuilder("KtPropagate:");
        for (String k : new String[] {"stl", "wrap", "helper", "str", "module", "ns"}) {
            sb.append(' ').append(k).append('=').append(counts.getOrDefault(k, 0));
        }
        int left = 0;
        for (Function fn : functions()) if (unnamed(fn)) left++;
        sb.append(", still unnamed ").append(left);
        println(sb.toString());
    }

    private void propagate() {
        for (int pass = 0; pass < 10; pass++) {
            int before = total();
            for (Function fn : functions()) {
                if (!unnamed(fn)) continue;
                Function callee = soleCallee(fn);
                if (callee != null && !unnamed(callee) && fn.getBody().getNumAddresses() <= 48) {
                    rename(fn, null, callee.getName() + "_wrap", "wrap");
                    continue;
                }
                GhidraClass owner = soleCallerClass(fn);
                if (owner != null) rename(fn, owner, "helper_" + addr(fn), "helper");
            }
            if (total() == before) break;
        }
    }

    // Deepest non-global namespace enclosing every caller of fn, or null.
    private Namespace commonCallerNamespace(Function fn) {
        Namespace common = null;
        for (Function c : fn.getCallingFunctions(monitor)) {
            Namespace ns = c.getParentNamespace();
            if (ns.isGlobal()) return null;
            if (common == null) {
                common = ns;
                continue;
            }
            while (common != null && !common.isGlobal() && !encloses(common, ns)) common = common.getParentNamespace();
            if (common == null || common.isGlobal()) return null;
        }
        return common;
    }

    private static boolean encloses(Namespace outer, Namespace ns) {
        for (Namespace n = ns; n != null && !n.isGlobal(); n = n.getParentNamespace()) {
            if (n.equals(outer)) return true;
        }
        return false;
    }

    // Class of the nearest named function from index i in direction dir, or
    // null if that function is not a class member.
    private static GhidraClass nearestNamedClass(List<Function> all, int i, int dir) {
        for (int j = i + dir; j >= 0 && j < all.size(); j += dir) {
            if (!unnamed(all.get(j))) return cls(all.get(j));
        }
        return null;
    }

    private Iterable<Function> functions() {
        return currentProgram.getFunctionManager().getFunctions(true);
    }

    private static boolean unnamed(Function fn) {
        return fn.getSymbol().getSource() == SourceType.DEFAULT;
    }

    private static GhidraClass cls(Function fn) {
        Namespace ns = fn.getParentNamespace();
        return ns instanceof GhidraClass ? (GhidraClass) ns : null;
    }

    private static String addr(Function fn) {
        return String.format("%08x", fn.getEntryPoint().getOffset());
    }

    private int total() {
        int t = 0;
        for (int v : counts.values()) t += v;
        return t;
    }

    private void rename(Function fn, Namespace ns, String name, String rule) {
        try {
            if (ns != null) fn.setParentNamespace(ns);
            String n = name;
            for (int k = 2; ; k++) {
                try {
                    fn.setName(n, SourceType.ANALYSIS);
                    break;
                } catch (ghidra.util.exception.DuplicateNameException e) {
                    n = name + "_" + k;
                }
            }
            counts.merge(rule, 1, Integer::sum);
        } catch (Exception e) {
            printerr("rename " + fn.getEntryPoint() + " -> " + name + ": " + e.getMessage());
        }
    }

    // The only function called (CALL or tail JMP) by fn, or null.
    private Function soleCallee(Function fn) {
        Set<Function> callees = fn.getCalledFunctions(monitor);
        return callees.size() == 1 ? callees.iterator().next() : null;
    }

    // The class whose methods are fn's only callers, or null.
    private GhidraClass soleCallerClass(Function fn) {
        Set<Function> callers = fn.getCallingFunctions(monitor);
        GhidraClass owner = null;
        for (Function c : callers) {
            GhidraClass k = cls(c);
            if (k == null || (owner != null && owner != k)) return null;
            owner = k;
        }
        return owner;
    }

    private List<String> referencedStrings(Function fn) {
        List<String> out = new ArrayList<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (Reference r : ins.getReferencesFrom()) {
                Address to = r.getToAddress();
                Data d = getDataAt(to);
                if (d != null && d.hasStringValue() && d.getValue() != null) out.add(d.getValue().toString());
            }
        }
        return out;
    }
}
