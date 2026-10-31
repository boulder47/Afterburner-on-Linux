#!/usr/bin/env python3
"""Count IOMemoryDescriptor-family virtuals for arm64 by simulating #ifdefs."""
import re, sys

def preprocess(path, defines):
    out = []
    stack = [True]  # active state
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            s = line.strip()
            m = re.match(r'#\s*(if|ifdef|ifndef)\b(.*)', s)
            if m:
                kind, arg = m.group(1), m.group(2).strip()
                if kind == 'if':
                    # evaluate simple macro presence/absence
                    val = eval_cond(arg, defines)
                    stack.append(stack[-1] and val)
                else:
                    present = arg in defines
                    val = present if kind == 'ifdef' else (arg not in defines)
                    stack.append(stack[-1] and val)
            elif re.match(r'#\s*else\b', s):
                # flip the top
                stack[-1] = not stack[-2] if len(stack) > 1 else True
                # careful: else flips only if parent active
                if len(stack) >= 2 and not stack[-2]:
                    stack[-1] = False
            elif re.match(r'#\s*endif\b', s):
                stack.pop()
            elif stack[-1]:
                out.append((ln, line.rstrip('\n')))
    return out

def eval_cond(arg, defines):
    # handle "defined(X)", "X", "!defined(X)"
    arg = arg.strip().rstrip(')') if arg.endswith(')') else arg
    if 'defined' in arg:
        neg = arg.startswith('!') or '!' in arg.split('defined')[0]
        m = re.search(r'defined\s*[\(]?\s*(\w+)', arg)
        name = m.group(1) if m else ''
        return (name not in defines) if neg else (name in defines)
    return arg in defines

# arm64 kernel kext build: __LP64__ defined; XNU_KERNEL_PRIVATE likely NOT for kexts? try both
for xkp in ('XNU_KERNEL_PRIVATE',):
    defines = {'__LP64__', '__arm64__', 'KERNEL'}
    if xkp: defines.add(xkp)
    lines = preprocess('IOMemoryDescriptor_xnu.h', defines)
    print(f"=== with {sorted(defines)} ===")
    n = 0
    in_class = None
    for ln, line in lines:
        cm = re.match(r'class\s+(\w+)', line)
        if cm and ('OSMetaClass' in line or 'public' in line):
            pass
        vm = re.search(r'\bvirtual\b', line)
        if vm:
            # skip pure comments already stripped? keep all
            n += 1
            txt = line.strip()[:90]
            print(f"  v{n:02d} L{ln}: {txt}")
