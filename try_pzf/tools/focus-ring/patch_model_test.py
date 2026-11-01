from pathlib import Path
p=Path('tools/focus-ring/test_model_stability.py');s=p.read_text(encoding='utf8');s=s.replace("    ns = {'uuid': uuid, 'time':", "    fn=next(node for node in source.body if isinstance(node,ast.FunctionDef) and node.name=='orientation_from_landmarks')\n    ns = {'uuid': uuid, 'time':")
s=s.replace("tree=ast.fix_missing_locations(ast.Module(body=[cls],type_ignores=[]))", "tree=ast.fix_missing_locations(ast.Module(body=[fn,cls],type_ignores=[]))")
p.write_text(s,encoding='utf8')
