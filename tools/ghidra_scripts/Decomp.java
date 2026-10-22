// Decompile every function whose fully qualified name matches one of the given regexes.
// Usage (headless): -postScript Decomp.java <out_file> <regex> [<regex> ...]
//@category NOVA3

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: Decomp.java <out_file> <regex>...");
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
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				String name = f.getName(true);
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
				DecompileResults res = ifc.decompileFunction(f, 120, monitor);
				out.println("// ===== " + name + " @ " + f.getEntryPoint());
				if (res.decompileCompleted()) {
					out.println(res.getDecompiledFunction().getC());
				} else {
					out.println("// decompile failed: " + res.getErrorMessage());
				}
				count++;
			}
		}
		println("Decompiled " + count + " functions to " + args[0]);
	}
}
