# instrument_kernel.py <spurs_kernel.c>: add a [kt] trace to each lifted kernel function.
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

def repl(m):
    addr = m.group(1)
    trace = '\n        if (g_kt < 300) { fprintf(stderr, "[kt] %s\\n"); g_kt++; }' % addr
    return m.group(0) + trace

s2 = re.sub(r'void spK_func_([0-9A-F]+)\(spu_context\* ctx\) \{', repl, s)
open(p, 'w', encoding='utf-8').write(s2)
print('instrumented functions:', s2.count('[kt]'))
