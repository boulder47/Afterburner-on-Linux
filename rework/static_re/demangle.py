#!/usr/bin/env python3
"""Minimal Itanium C++ demangler for the subset used by AppleAfterburner kext symbols.

Handles: __Z prefix, N...E nested names, function params (P/R/K pointers/refs),
basic types, class type names, S_ back-references, cv-qualifiers.
Usage: python3 demangle.py 'name' ['name2' ...]   or  echo name | python3 demangle.py
"""
import sys

BASIC = {
    'b': 'bool', 'c': 'char', 's': 'signed char', 'h': 'unsigned char',
    'a': 'wchar_t', 'w': 'char16_t', 'z': 'char32_t',
    'i': 'int', 'l': 'long', 'm': 'long long',
    'j': 'unsigned int', 'u': 'unsigned long', 't': 'unsigned long long',
    'f': 'float', 'd': 'double', 'e': 'long double', 'g': '__float80',
    'n': '__int128', 'o': 'unsigned __int128', 'v': 'void',
}

class P:
    def __init__(self, s):
        self.s = s
        self.i = 0
        self.refs = []  # S_ back-refs (type names)

    def peek(self, n=1):
        return self.s[self.i:self.i + n]

    def eat(self, c):
        assert self.peek() == c, f'expected {c!r} at {self.i}: {self.s[self.i:self.i+20]}'
        self.i += 1

    def num(self):
        j = self.i
        while self.s[j].isdigit():
            j += 1
        v = int(self.s[self.i:j])
        self.i = j
        return v

    def ident(self):
        n = self.num()
        name = self.s[self.i:self.i + n]
        self.i += n
        self.refs.append(name)
        return name

    # returns (name, is_class_type) — a bare <len><ident> in type position is a class
    def type(self):
        c = self.peek()
        if c == 'P':
            self.eat('P')
            inner = self.type()
            return inner + '*', False
        if c == 'R':
            self.eat('R')
            inner = self.type()
            return inner + '&', False
        if c in ('K', 'V'):  # const / volatile (suffix form handled below)
            pass
        if c in BASIC:
            self.i += 1
            t = BASIC[c]
            while self.peek() in ('K', 'V'):
                q = self.s[self.i]; self.i += 1
                t += ' const' if q == 'K' else ' volatile'
            return t, False
        if c == 'S':  # S_ or S<digits> back-ref
            if self.peek(2) == 'S_':
                self.i += 2
                idx = len(self.refs) - 1
            elif c == 'S' and self.s[self.i+1].isdigit():
                self.i += 1
                idx = int(self.num())
            else:
                # actually an identifier starting with S (e.g. class named S...)
                return self.classname()
            if 0 <= idx < len(self.refs):
                return self.refs[idx], False
            return f'S{idx}', False
        if c.isdigit():
            return self.classname()
        # fallback: consume as identifier
        n = self.num()
        name = self.s[self.i:self.i + n]
        self.i += n
        self.refs.append(name)
        return name, False

    def classname(self):
        n = self.num()
        name = self.s[self.i:self.i + n]
        self.i += n
        self.refs.append(name)
        # trailing cv qualifiers on class type
        while self.peek() in ('K', 'V'):
            q = self.s[self.i]; self.i += 1
            name += ' const' if q == 'K' else ' volatile'
        return name, True

    def nested(self):
        """Returns (joined_name, kind) where kind is None/'ctor'/'dtor'."""
        parts = []
        kind = None
        while self.peek() != 'E':
            c = self.peek()
            if c.isdigit():
                n = self.num()
                name = self.s[self.i:self.i + n]
                self.i += n
                self.refs.append(name)
                parts.append(name)
            elif c == 'C':  # constructor: base class already in parts, then C<1|2>
                self.eat('C')
                kind = 'ctor'
                if self.peek().isdigit():
                    self.i += 1
            elif c == 'D':  # destructor: D0/D1/D2 after base class name
                self.eat('D')
                kind = 'dtor'
                if self.peek().isdigit():
                    self.i += 1
            elif c == 'I':  # template args — skip to matching E
                self.eat('I')
                depth = 1
                while depth:
                    if self.peek() == 'E':
                        depth -= 1; self.i += 1
                    elif self.peek().isdigit():
                        n = self.num(); self.i += n
                    else:
                        self.i += 1
            else:
                raise ValueError(f'unexpected {c!r} in nested at {self.i}: {self.s[self.i:self.i+30]}')
        self.eat('E')
        return '::'.join(parts), kind

    def params(self):
        """Parse function parameter list; returns (param_str, is_ctor_or_dtor)."""
        if self.peek() == 'v':
            self.i += 1
            return '', False
        ps = []
        while self.peek() != 'E' and self.i < len(self.s):
            t, _ = self.type()
            ps.append(t)
        self.eat('E')
        return ', '.join(ps), False

def demangle(name):
    if not name.startswith('__Z'):
        # local symbol: strip leading underscore(s) and try
        if name.startswith('_') and len(name) > 1:
            core = name[1:]
            if core.startswith('Z'):
                return 'local:' + demangle('__' + core)
        return name
    p = P(name[2:])
    c = p.peek()
    try:
        if c == 'N':
            p.eat('N')
            cls, kind = p.nested()
            base = cls.split('::')[-1]
            # after E of nested: function params (member fn) or type end
            if p.i < len(p.s):
                params, _ = p.params()
                if kind == 'ctor':
                    return f'{cls}::{base}({params})'
                if kind == 'dtor':
                    return f'{cls}::~{base}({params})'
                return f'{cls}({params})'
            if kind == 'ctor':
                return f'{cls}::{base}(implicit ctor)'
            if kind == 'dtor':
                return f'{cls}::~{base}(implicit dtor)'
            return cls
        if c == 'C':  # global ctor
            p.eat('C')
            n = p.num(); name2 = p.s[p.i:p.i+n]; p.i += n
            while p.peek() != 'E':
                if p.peek().isdigit():
                    n = p.num(); p.i += n
                else:
                    p.i += 1
            return f'{name2}(global ctor)'
        if c == 'D':  # global dtor
            p.eat('D'); p.i += 1
            n = p.num(); name2 = p.s[p.i:p.i+n]; p.i += n
            while p.peek() != 'E':
                if p.peek().isdigit():
                    n = p.num(); p.i += n
                else:
                    p.i += 1
            return f'~{name2}(global dtor)'
        # global function
        n = p.num()
        fn = p.s[p.i:p.i+n]; p.i += n
        params, _ = p.params()
        return f'{fn}({params})'
    except Exception as e:
        return f'<undemangled {name}: {e}>'

if __name__ == '__main__':
    names = sys.argv[1:] or [l.strip() for l in sys.stdin if l.strip()]
    for n in names:
        print(demangle(n))
