// Name functions from a match list (tools/match_symbols.py): "entry<TAB>name<TAB>score"
// lines. Names with a namespace ("CLevel::Init") go into that namespace; a function
// already named by the binary's own symbols keeps its name. Run without -readOnly so the
// names are saved.
// Usage (headless): -postScript ApplyNames.java <matches.txt>
//@category NOVA3

import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int named = 0, skipped = 0;
		for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
			String[] p = line.split("\t");
			if (p.length < 2) continue;
			Address at = toAddr(p[0]);
			Function f = getFunctionAt(at);
			if (f == null) f = createFunction(at, null);
			if (f == null || f.getSymbol().getSource() == SourceType.IMPORTED) {
				++skipped;
				continue;
			}
			String full = p[1];
			// A comment keeps the match's strength.
			Namespace ns = currentProgram.getGlobalNamespace();
			String name = full;
			int cut = full.lastIndexOf("::");
			if (cut > 0 && !full.contains("(")) {
				String[] parts = full.substring(0, cut).split("::");
				for (String part : parts) {
					Namespace next = currentProgram.getSymbolTable().getNamespace(part, ns);
					if (next == null) next = currentProgram.getSymbolTable().createNameSpace(ns, part, SourceType.ANALYSIS);
					ns = next;
				}
				name = full.substring(cut + 2);
			}
			try {
				f.setParentNamespace(ns);
				f.setName(name, SourceType.ANALYSIS);
				f.setComment("name from libNOVA3_neon.so (match score " + (p.length > 2 ? p[2] : "?") + ")");
				++named;
			} catch (Exception ex) {
				++skipped;
			}
		}
		println("named " + named + ", skipped " + skipped);
	}
}
