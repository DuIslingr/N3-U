// Decompile every function that calls a function whose fully qualified name matches one of
// the given regexes.
// Usage (headless): -postScript Callers.java <out_file> <regex> [<regex> ...]
//@category NOVA3

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Callers extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: Callers.java <out_file> <regex>...");
			return;
		}
		List<Pattern> patterns = new ArrayList<>();
		for (int i = 1; i < args.length; i++) {
			patterns.add(Pattern.compile(args[i]));
		}

		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);

		int count = 0;
		try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
			for (Function target : currentProgram.getFunctionManager().getFunctions(true)) {
				String name = target.getName(true);
				boolean match = false;
				for (Pattern p : patterns) {
					if (p.matcher(name).find()) {
						match = true;
						break;
					}
				}
				if (!match) {
					continue;
				}
				Set<Function> callers = new LinkedHashSet<>();
				for (Reference r : getReferencesTo(target.getEntryPoint())) {
					Function f = getFunctionContaining(r.getFromAddress());
					if (f != null) {
						callers.add(f);
					}
				}
				out.println("// \"" + name + "\": " + callers.size() + " callers");
				for (Function f : callers) {
					DecompileResults res = ifc.decompileFunction(f, 120, monitor);
					out.println("// ---- " + f.getName(true) + " @ " + f.getEntryPoint() + " (calls " + name + ")");
					if (res.decompileCompleted()) {
						out.println(res.getDecompiledFunction().getC());
					} else {
						out.println("// decompile failed: " + res.getErrorMessage());
					}
					count++;
				}
			}
		}
		println("Decompiled " + count + " callers to " + args[0]);
	}
}
