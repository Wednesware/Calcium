"""Static lowering coverage; never imports or executes the analyzed application."""

import ast
from collections import Counter

LABELS = {
    "annotations": "Annotations",
    "async": "Async",
    "classes": "Classes",
    "closures": "Closures / nonlocal",
    "decorators": "Decorators",
    "dynamic_features": "Dynamic features / reflection",
    "exception_groups": "Exception groups",
    "future_flags": "Future compiler flags",
    "generators": "Generators",
    "generics": "Generics",
    "lambdas": "Lambdas",
    "nested_functions": "Nested functions",
    "pattern_matching": "Pattern matching",
    "unsupported_syntax": "Unsupported syntax",
}


def module_report(name, source, analysis):
    path = source.path
    source_lines = source.text.splitlines()
    reasons = analysis.fallbacks
    module_reasons = [r for r in reasons if r["boundary"] == "module"]
    mode = "fallback" if module_reasons else "mixed" if reasons else "native"
    functions, classes = [], []
    by_line = {}
    for reason in reasons:
        if reason["boundary"] == "definition":
            by_line.setdefault(reason["definition_line"], []).append(reason)
    module_boundary = (
        {"id": "module", "kind": "module", "name": name, "line": 1, "reasons": module_reasons}
        if module_reasons
        else None
    )
    boundaries_by_id = {"module": module_boundary} if module_boundary else {}

    def walk(
        node,
        scope="",
        function_scope=False,
        inherited=(() if module_boundary is None else (module_boundary,)),
    ):
        is_function = isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda))
        is_class = isinstance(node, ast.ClassDef)
        if is_function or is_class:
            short = "<lambda>" if isinstance(node, ast.Lambda) else node.name
            qualname = scope + (".<locals>." if function_scope else ".") + short if scope else short
            own = None
            if id(node) in analysis.definition_fallbacks:
                own = {
                    "id": f"definition:{node.lineno}",
                    "kind": "definition",
                    "name": qualname,
                    "line": node.lineno,
                    "reasons": by_line.get(node.lineno, []),
                }
                boundaries_by_id[own["id"]] = own
            boundaries = inherited + ((own,) if own else ())
            boundary = boundaries[0] if boundaries else None
            row = {
                "qualname": qualname,
                "line": node.lineno,
                "column": len(
                    source_lines[node.lineno - 1].encode("utf-8")[: node.col_offset].decode("utf-8")
                )
                + 1,
                "kind": "class"
                if is_class
                else "lambda"
                if isinstance(node, ast.Lambda)
                else "async_function"
                if isinstance(node, ast.AsyncFunctionDef)
                else "function",
                "lowering": "fallback" if boundary else "native",
                "fallback_boundary": {
                    key: value for key, value in boundary.items() if key != "reasons"
                }
                if boundary
                else None,
                "blocking_boundaries": [b["id"] for b in boundaries],
            }
            (classes if is_class else functions).append(row)
            # Defaults, decorators, bases and annotations belong to the enclosing
            # lexical scope, while compilation may still be blocked by this def.
            for field, value in ast.iter_fields(node):
                children = value if isinstance(value, list) else [value]
                for child in children:
                    if isinstance(child, ast.AST):
                        if field == "body":
                            walk(child, qualname, is_function, boundaries)
                        else:
                            walk(child, scope, function_scope, boundaries)
            return
        for child in ast.iter_child_nodes(node):
            walk(child, scope, function_scope, inherited)

    walk(analysis.tree)
    return {
        "name": name,
        "path": str(path),
        "source_sha256": source.digest,
        "mode": mode,
        "fallbacks": reasons,
        "boundaries": boundaries_by_id,
        "functions": functions,
        "classes": classes,
    }


def optimization_report(modules, decisions, *, complete, discovered_modules):
    functions = [f for m in modules for f in m["functions"]]
    fallback = [f for f in functions if f["lowering"] == "fallback"]
    primary, overlapping = Counter(), Counter()
    for module in modules:
        boundaries = module.get("boundaries", {})
        for function in module["functions"]:
            if function["lowering"] != "fallback":
                continue
            reasons = boundaries[function["fallback_boundary"]["id"]]["reasons"]
            categories = [r["category"] for r in reasons] or ["unsupported_syntax"]
            primary[categories[0]] += 1
            overlapping.update(
                {
                    r["category"]
                    for boundary in function["blocking_boundaries"]
                    for r in boundaries[boundary]["reasons"]
                }
            )
    return {
        "schema_version": 1,
        "measurement": "static source definitions and selected lowering; not runtime hotness or dynamic fallback percentages",
        "scope": "discovered project modules only; external packages and unresolved dynamic imports excluded",
        "complete": complete,
        "discovered_modules": discovered_modules,
        "summary": {
            "modules_analyzed": len(modules),
            "module_modes": dict(sorted(Counter(m["mode"] for m in modules).items())),
            "functions_analyzed": len(functions),
            "functions_native": len(functions) - len(fallback),
            "functions_fallback": len(fallback),
            "classes_analyzed": sum(len(m["classes"]) for m in modules),
        },
        "counting": "Each named function, async function, method, nested function and lambda is counted once, including conditional/redefined definitions. Classes are separate. Primary categories use the first blocker of the selected enclosing boundary; reason categories may overlap.",
        "primary_fallback_reasons": dict(sorted(primary.items())),
        "fallback_reason_function_counts": dict(sorted(overlapping.items())),
        "modules": modules,
        "optimization_decisions": list(decisions),
    }


def format_optimization_report(report):
    summary = report["summary"]
    lines = [
        "Calcium optimization report",
        "",
        f"Modules analyzed:   {summary['modules_analyzed']} / {report['discovered_modules']}",
        f"Functions analyzed: {summary['functions_analyzed']}",
        f"Native lowering:    {summary['functions_native']}",
        f"CPython fallback:   {summary['functions_fallback']}",
        f"Classes analyzed:   {summary['classes_analyzed']}",
        "",
        "Primary fallback reasons (one per fallback function):",
    ]
    for category, count in sorted(
        report["primary_fallback_reasons"].items(), key=lambda p: (-p[1], p[0])
    ):
        lines.append(f"  {LABELS.get(category, category)}: {count}")
    if not report["primary_fallback_reasons"]:
        lines.append("  None")
    lines += ["", "Known blocker categories (functions; categories may overlap):"]
    for category, count in sorted(
        report["fallback_reason_function_counts"].items(), key=lambda p: (-p[1], p[0])
    ):
        lines.append(f"  {LABELS.get(category, category)}: {count}")
    lines += ["", "Module and function decisions:"]
    for module in report["modules"]:
        lines.append(f"  {module['name']}: {module['mode']} ({module['path']})")
        for reason in module["fallbacks"]:
            boundary = (
                f"definition {reason['definition']}"
                if reason["boundary"] == "definition"
                else "whole module"
            )
            lines.append(
                f"    {module['path']}:{reason['line']}:{reason['column']} [{boundary}; {reason['category']}] {reason['reason']}"
            )
        for function in module["functions"]:
            boundary = function["fallback_boundary"]
            detail = (
                f"; {boundary['kind']} {boundary['name']}:{boundary['line']}" if boundary else ""
            )
            lines.append(
                f"    {function['qualname']}:{function['line']}:{function['column']} → {function['lowering']}{detail}"
            )
    lines += ["", f"Recorded optimizer decisions: {len(report['optimization_decisions'])}"]
    for decision in report["optimization_decisions"]:
        lines.append(
            f"  {decision['module']}.{decision['function']}: {decision['pass']}"
            + (f" ({decision['operations']} operations)" if "operations" in decision else "")
        )
    lines += ["", "Static coverage only; not runtime hotness or dynamic fallback percentages."]
    if not report["complete"]:
        lines.append(
            "Partial report: analysis stopped before all discovered modules were classified."
        )
    return "\n".join(lines)
