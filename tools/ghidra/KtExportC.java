// Decompiles every function and writes C sources grouped by namespace:
//   <out>/classes/<Namespace>.c   members of each C++ class / namespace
//   <out>/xdk/<lib>.c             (not separated here; library code lands in global)
//   <out>/global/<addr-range>.c   free functions, 64 KB of address space per file
//   <out>/index.tsv               address, namespace, name, file, status
//
// Headless: -postScript KtExportC.java <output directory> [timeout seconds]
// @category KakutoChojin-recomp
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Namespace;

public class KtExportC extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : askDirectory("Output", "Export").getAbsolutePath());
        int timeout = args.length > 1 ? Integer.parseInt(args[1]) : 60;
        new File(out, "classes").mkdirs();
        new File(out, "global").mkdirs();

        DecompInterface dec = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        dec.setOptions(opts);
        dec.toggleCCode(true);
        dec.toggleSyntaxTree(false);
        if (!dec.openProgram(currentProgram)) throw new Exception("decompiler: " + dec.getLastMessage());

        Map<String, PrintWriter> files = new HashMap<>();
        int ok = 0, failed = 0, total = currentProgram.getFunctionManager().getFunctionCount();
        try (PrintWriter index = new PrintWriter(new FileWriter(new File(out, "index.tsv")))) {
            index.println("address\tnamespace\tname\tfile\tstatus");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            int n = 0;
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (f.isThunk() || f.isExternal()) continue;
                n++;
                if (n % 500 == 0) println(String.format("KtExportC: %d / %d", n, total));
                Namespace ns = f.getParentNamespace();
                String rel;
                if (ns == null || ns.isGlobal()) {
                    long a = f.getEntryPoint().getOffset();
                    rel = String.format("global/%08x.c", a & ~0xFFFFL);
                } else {
                    rel = "classes/" + ns.getName(true).replaceAll("[^A-Za-z0-9_.-]+", "_") + ".c";
                }
                PrintWriter w = files.get(rel);
                if (w == null) {
                    w = new PrintWriter(new FileWriter(new File(out, rel)));
                    w.println("// Decompiled by Ghidra via KakutoChojin-recomp (tools/ghidra/KtExportC.java).");
                    w.println("// Generated from the user's own XBE; do not redistribute.");
                    w.println();
                    files.put(rel, w);
                }
                DecompileResults r = dec.decompileFunction(f, timeout, monitor);
                String status;
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    w.printf("// %s @ %s%n", f.getName(true), f.getEntryPoint());
                    w.println(r.getDecompiledFunction().getC());
                    status = "ok";
                    ok++;
                } else {
                    String msg = r == null ? "no result" : r.getErrorMessage();
                    w.printf("// %s @ %s: decompilation failed: %s%n%n", f.getName(true), f.getEntryPoint(),
                        msg == null ? "" : msg.replace('\n', ' '));
                    status = "failed";
                    failed++;
                }
                index.printf("%s\t%s\t%s\t%s\t%s%n", f.getEntryPoint(), ns == null ? "" : ns.getName(true),
                    f.getName(), rel, status);
            }
        } finally {
            for (PrintWriter w : files.values()) w.close();
            dec.dispose();
        }
        println(String.format("KtExportC: %d decompiled, %d failed, %d files in %s", ok, failed, files.size(), out));
    }
}
