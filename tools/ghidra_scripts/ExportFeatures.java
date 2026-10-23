// Export per-function features for cross-binary matching (tools/match_symbols.py): each
// function's entry, name, size, the string literals it references (directly or through a
// literal pool / pointer), its callees in address order (externals by name) and the
// numeric constants it loads.
// Usage (headless): -postScript ExportFeatures.java <out.jsonl>
//@category NOVA3

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.scalar.Scalar;
import java.util.TreeSet;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

public class ExportFeatures extends GhidraScript {
	private static String esc(String s) {
		StringBuilder b = new StringBuilder();
		for (char c : s.toCharArray()) {
			if (c == '"' || c == '\\') b.append('\\').append(c);
			else if (c < 0x20 || c > 0x7e) b.append(String.format("\\u%04x", (int) c));
			else b.append(c);
		}
		return b.toString();
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		// Strings by the functions that reach them.
		Map<Function, Set<String>> strings = new HashMap<>();
		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Data d = it.next();
			if (!d.hasStringValue()) continue;
			Object v = d.getValue();
			if (v == null) continue;
			String s = v.toString();
			if (s.length() < 4) continue;
			Set<Address> targets = new LinkedHashSet<>();
			targets.add(d.getAddress());
			for (Reference r : getReferencesTo(d.getAddress())) targets.add(r.getFromAddress());
			for (Address t : targets) {
				Function f = getFunctionContaining(t);
				if (f != null) strings.computeIfAbsent(f, k -> new LinkedHashSet<>()).add(s);
				for (Reference r : getReferencesTo(t)) {
					Function g = getFunctionContaining(r.getFromAddress());
					if (g != null) strings.computeIfAbsent(g, k -> new LinkedHashSet<>()).add(s);
				}
			}
		}
		try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
			FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
			while (fi.hasNext() && !monitor.isCancelled()) {
				Function f = fi.next();
				if (f.isExternal()) continue;
				List<String> calls = new ArrayList<>();
				TreeSet<Long> consts = new TreeSet<>();
				InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
				int count = 0;
				while (ii.hasNext()) {
					Instruction ins = ii.next();
					++count;
					// Immediates (assert line numbers, sizes, ids) that survive any compiler.
					for (int op = 0; op < ins.getNumOperands(); ++op)
						for (Object o : ins.getOpObjects(op))
							if (o instanceof Scalar) {
								long v = ((Scalar) o).getUnsignedValue();
								if (v >= 2 && v <= 100000) consts.add(v);
							}
					if (!ins.getFlowType().isCall()) continue;
					for (Address t : ins.getFlows()) {
						Function g = getFunctionAt(t);
						if (g == null) continue;
						if (g.isThunk()) {
							Function th = g.getThunkedFunction(true);
							if (th != null) g = th;
						}
						calls.add(g.isExternal() ? "x:" + g.getName() : g.getEntryPoint().toString());
					}
				}
				StringBuilder b = new StringBuilder();
				b.append("{\"entry\":\"").append(f.getEntryPoint()).append("\",\"name\":\"").append(esc(f.getName(true)))
						.append("\",\"size\":").append(f.getBody().getNumAddresses()).append(",\"ins\":").append(count)
						.append(",\"thunk\":").append(f.isThunk()).append(",\"strings\":[");
				boolean first = true;
				for (String s : strings.getOrDefault(f, Set.of())) {
					if (!first) b.append(',');
					b.append('"').append(esc(s)).append('"');
					first = false;
				}
				b.append("],\"consts\":[");
				boolean firstc = true;
				for (Long v : consts) {
					if (!firstc) b.append(',');
					b.append(v);
					firstc = false;
				}
				b.append("],\"calls\":[");
				for (int k = 0; k < calls.size(); ++k) {
					if (k > 0) b.append(',');
					b.append('"').append(esc(calls.get(k))).append('"');
				}
				b.append("]}");
				out.println(b);
			}
		}
	}
}
