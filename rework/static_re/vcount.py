#!/usr/bin/env python3
"""Count vtable slots for XNU class hierarchies from raw headers.
Handles #if/#ifdef/#ifndef/#else with our define set; extracts per-class
virtual declarations in order, marking overrides."""
import re, sys

DEFINED = {'__arm64__', '__LP64__', 'APPLE_KEXT_VTABLE_PADDING',
           'XNU_TARGET_OS_OSX', 'XNU_KERNEL_PRIVATE'}

def eval_cond(expr):
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)',
                  lambda m: '1' if m.group(1) in DEFINED else '0', expr)
    for name in ('__arm64__', '__LP64__', 'APPLE_KEXT_VTABLE_PADDING',
                 'XNU_TARGET_OS_OSX', 'XNU_KERNEL_PRIVATE'):
        expr = re.sub(r'\b' + name + r'\b', '1', expr)
    expr = re.sub(r'[A-Za-z_]\w*', '0', expr)   # undefined macros -> 0
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    try:
        return bool(eval(expr))
    except Exception:
        return False

def preprocess(fname):
    """Return list of (lineno, line) for lines active under DEFINED."""
    out = []
    # stack entries: [parent_active, cond_taken]  (cond_taken = whether the
    # if-branch was taken; used to invert on #else)
    stack = []
    active = True
    with open(fname) as f:
        for lineno, line in enumerate(f, 1):
            s = line.strip()
            m = re.match(r'#\s*(if|ifdef|ifndef)\b\s*(.*)', s)
            if m:
                kind, arg = m.group(1), m.group(2).strip()
                if kind == 'if':
                    cond = eval_cond(arg)
                elif kind == 'ifdef':
                    cond = bool(arg) and arg.split()[0] in DEFINED
                else:
                    cond = not (bool(arg) and arg.split()[0] in DEFINED)
                stack.append([active, cond])
                active = active and cond
                continue
            if re.match(r'#\s*else\b', s):
                parent, taken = stack[-1]
                active = parent and not taken
                stack[-1][1] = not taken
                continue
            if re.match(r'#\s*endif\b', s):
                parent, _ = stack.pop()
                active = parent
                continue
            if active:
                out.append((lineno, line))
    return out

def class_body(lines, clsname):
    """Extract (start_idx, end_idx) of 'class <clsname> ... { ... }' body."""
    start = None
    for i, (_, l) in enumerate(lines):
        if re.match(r'\s*class\s+' + clsname + r'\b', l):
            # make sure it's a definition (has : public or { before next ;)
            j = i
            while j < len(lines) and '{' not in lines[j][1]:
                if ';' in lines[j][1] and 'class' not in lines[j][1].split(';')[0][-20:]:
                    break
                j += 1
            if j < len(lines) and '{' in lines[j][1]:
                start = i
                break
    if start is None:
        return None
    depth = 0
    for i in range(start, len(lines)):
        _, l = lines[i]
        # strip comments crudely
        lc = re.sub(r'/\*.*?\*/', '', l)
        depth += lc.count('{') - lc.count('}')
        if depth == 0 and i > start:
            return (start, i)
    return None

def virts_in(lines, s, e):
    """Yield (lineno, decl, is_override) for virtual declarations in [s,e)."""
    i = s
    while i < e:
        _, l = lines[i]
        m = re.match(r'\s*virtual\s+(.*)', l)
        if m:
            decl = m.group(1)
            j = i
            while ';' not in decl and j + 1 < e:
                j += 1
                decl += ' ' + lines[j][1].strip()
            decl = re.sub(r'\s+', ' ', decl).strip().rstrip(';')
            yield (lines[i][0], decl, 'OVERRIDE' in decl)
            i = j + 1
        else:
            i += 1

def main():
    # args: file:class pairs, in inheritance order (base first)
    BASE = 41   # 2 header + 21 OSMetaClassBase + 18 OSObject (verified)
    n = BASE
    for spec in sys.argv[1:]:
        f, cls = spec.split(':')
        lines = preprocess(f)
        body = class_body(lines, cls)
        if not body:
            print(f'{f}:{cls} NOT FOUND'); continue
        s, e = body
        new = 0
        for ln, d, ov in virts_in(lines, s, e):
            if ov:
                print(f'    [override] L{ln}: {d[:85]}')
            else:
                off = (n - 1) * 8
                print(f'    vf#{n} +0x{off:x}: L{ln}: {d[:85]}')
                n += 1
                new += 1
        print(f'{f}:{cls} adds {new} slots; next free = vf#{n} (+0x{(n-1)*8:x})')

if __name__ == '__main__':
    main()
