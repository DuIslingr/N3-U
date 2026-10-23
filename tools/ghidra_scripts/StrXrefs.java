// Decompile every function that references one of the given NUL-terminated strings.
// Usage (headless): -postScript StrXrefs.java <out_file> <string> [<string> ...]
//@category NOVA3

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class StrXrefs extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
			for (int i = 1; i < args.length; i++) {
				byte[] needle = (args[i] + "\0").getBytes("UTF-8");
				Set<Function> funcs = new LinkedHashSet<>();
				Address a = currentProgram.getMinAddress();
				while ((a = find(a, needle)) != null) {
					// The string itself, and any pointers to it (literal pools, GOT-relative tables).
					Set<Address> targets = new LinkedHashSet<>();
					targets.add(a);
					for (Reference r : getReferencesTo(a)) targets.add(r.getFromAddress());
					for (Address t : targets) {
						for (Reference r : getReferencesTo(t)) {
							Function f = getFunctionContaining(r.getFromAddress());
							if (f != null) funcs.add(f);
						}
						Function f = getFunctionContaining(t);
						if (f != null) funcs.add(f);
					}
					out.println("// \"" + args[i] + "\" at " + a + ": " + targets.size() + " targets");
					a = a.add(1);
				}
				for (Function f : funcs) {
					out.println("// ---- " + f.getName(true) + " @ " + f.getEntryPoint() + " (refs \"" + args[i] + "\")");
					DecompileResults res = ifc.decompileFunction(f, 120, monitor);
					if (res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
				}
			}
		}
	}
}
