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
			// Split on "::" outside template arguments ("std::vector<a::B>::push_back").
			java.util.List<String> parts = new java.util.ArrayList<>();
			int depth = 0, start = 0;
			for (int i = 0; i < full.length(); ++i) {
				char c = full.charAt(i);
				if (c == '<' || c == '(') ++depth;
				else if (c == '>' || c == ')') --depth;
				else if (depth == 0 && c == ':' && i + 1 < full.length() && full.charAt(i + 1) == ':') {
					parts.add(full.substring(start, i));
					start = i + 2;
					++i;
				}
			}
			parts.add(full.substring(start));
			int cut = parts.size() > 1 ? 1 : 0;
			if (cut > 0) {
				for (String part : parts.subList(0, parts.size() - 1)) {
					Namespace next = currentProgram.getSymbolTable().getNamespace(part, ns);
					if (next == null) next = currentProgram.getSymbolTable().createNameSpace(ns, part, SourceType.ANALYSIS);
					ns = next;
				}
				name = parts.get(parts.size() - 1);
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
