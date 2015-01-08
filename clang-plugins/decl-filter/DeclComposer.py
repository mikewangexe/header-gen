import sys, os
import re
import argparse
import sqlite3
import functools
import shutil
from termcolor import colored, cprint
from subprocess import Popen, PIPE

REMOVE_INLINE_DEFINITIONS = False

KIND_MACRO = 0
KIND_INCLUSION = 1
KIND_IDENTIFIER = 2

MSG_HANDLE_SUCCEEDED = 0
MSG_HANDLE_FAILED = 1
MSG_HANDLE_SKIPPED = 2

clang = os.environ['CLANG']
source_dir = ''
header_absdirs = []
prefetch_headers = []
cc_flags = ''
additional_include_dirs = []
platform_cc_flags = ''

def configure(mode):
    global source_dir
    global header_absdirs
    global prefetch_headers
    global cc_flags
    global additional_include_dirs
    global platform_cc_flags

    if mode == 'test':
        header_absdirs = ['include']
    elif mode == 'linux':
        __linux_dir = os.environ['LINUX_DIR']
        __linux_header_reldirs = ['arch/arm/mach-bcm2708/include', 'arch/arm/include', 'include']
        __linux_header_reldirs = map(lambda x:os.path.join(x, 'generated'), __linux_header_reldirs) + __linux_header_reldirs
        __headers = ['linux/kconfig.h']
        __macros = ['__KERNEL__', '__KERN__', '__LINUX_ARM_ARCH__', '\'KBUILD_BASENAME="dde"\'', '\'KBUILD_MODNAME="dde"\'']

        source_dir = __linux_dir
        header_absdirs = map(lambda x: os.path.join(__linux_dir, x), __linux_header_reldirs)
        prefetch_headers = [['generated', 'autoconf.h'], ['generated', 'bounds.h']]
        cc_flags = ' '.join(map(lambda x: '-D' + x, __macros)) + ' ' + ' '.join(map(lambda x: '-include ' + x, __headers))
        platform_cc_flags = '-target arm-eabi -marm -mfpu=vfp -mcpu=arm1176jzf-s -mtune=arm1176jzf-s -mfloat-abi=softfp'
        additional_include_dirs = ['uapi']
    else:
        print 'Undefined mode:', mode
        sys.exit(1)


def mkdir(d):
    if not os.path.isdir(d):
        os.makedirs(d)

def random_fixes(lines, relpath, source_range):
    def get_line(linum):
        return re.sub('/\*.*\*/', '', lines[linum - 1]).rstrip()

    start = source_range.start.line
    end = source_range.end.line

    # Fix 1:
    #   Multi-line macros with a single ')' at the last line, e.g.
    #     #define MASK(nbits)                                      \
    #     (                                                        \
    #         ((nbits) % 64) ? (1UL << ((nbits) % 64)) - 1 : ~0UL  \
    #     )
    #   Locations reported by MacroInfo in clang do not cover the last line.
    if source_range.kind == KIND_MACRO and get_line(end).endswith('\\'):
        if verbose:
            print '... Fix unterminated macro definition %s @ %s:%d' % (source_range.name, relpath, start)
        end += 1

    # Fix 2:
    #   Multi-line identifier declaration with a single ';' at the last line, e.g.
    #     void foo(void)
    #         __compiletime_error("Error");
    #   where __compiletime_error expands to nothing.
    #   Locations reported by MacroInfo in clang do not cover the last line.
    if source_range.kind == KIND_IDENTIFIER and not source_range.from_macro and not source_range.has_body:
        extended = 0
        line = get_line(end + extended)
        while not line.endswith(';'):
            extended += 1
            line = get_line(end + extended)
        if extended > 0 and verbose:
            print '... Fix unterminated ident definition %s @ %s:%d, +%d' % (source_range.name, relpath, start, extended)
        end += extended

    # Fix 3:
    #  Macros cover multiple lines and declare multiple things lead to
    #  unbalanced parentheses, e.g.
    #    HERE_IS_A_MACRO (
    #       xxx,
    #       yyy
    #       zzz)
    #  Try to include the following lines until parentheses are balanced
    if source_range.from_macro:
        left = right = 0
        extended = 0
        for i in range(start - 1, end):
            line = lines[i]
            left += line.count('(')
            right += line.count(')')
        if left != right:
            while left != right:
                line = lines[end + extended]
                left += line.count('(')
                right += line.count(')')
                extended += 1
            end += extended
            if verbose:
                print '... Fix unbalanced parentheses in macro expansion @ %s:%d, +%d' % (relpath, start, extended)

    # Fix 4
    #   Multi-line comments, e.g.
    #     #define XXX 16  /* this is a comment
    #                        another line follows */
    if get_line(end).find('/*') >= 0:
        extended = 1
        while get_line(end + extended).find('*/') < 0:
            extended += 1
        if verbose:
            print '... Fix unbalanced comments @ %s:%d, +%d' % (relpath, start, extended)
        end += extended

    return (start, end)


@functools.total_ordering
class SourcePosition:
    def __init__(self, line, col):
        self.line = line
        self.col = col

    def __eq__(self, other):
        return (self.line == other.line)

    def __le__(self, other):
        return (self.line < other.line)

    def __str__(self):
        return str(self.line) + ":" + str(self.col)


@functools.total_ordering
class SourceRange:
    def __init__(self, start, end, name, kind, from_macro = False, has_body = False):
        self.start = start
        self.end = end
        self.name = name
        self.kind = kind
        self.from_macro = from_macro
        self.has_body = has_body

    def __eq__(self, other):
        return (self.start == other.start) and (self.end == other.end)

    def __le__(self, other):
        return (self.start < other.start) or (self.start == other.start and self.end < other.end)

    def __str__(self):
        return str(self.start) + "-" + str(self.end)


class Header:
    headers = {}

    @staticmethod
    def dumpall():
        for k,v in Header.headers.items():
            v.dump(workdir)
        for h in prefetch_headers:
            target = os.path.join(workdir, *h)
            mkdir(os.path.dirname(target))
            shutil.copy2(os.path.join(source_dir, 'include', *h), target)

    def __init__(self, path):
        self.abspath = path
        self.relpath = ''
        for absdir in header_absdirs:
            if path.startswith(absdir):
                self.relpath = os.path.relpath(path, absdir)
                break
        self.__decls = []
        self.dumped = False

    def add_decl_range(self, r):
        if not r in self.__decls:
            self.__decls.append(r)
            self.dumped = False

    def dump(self, workdir):
        if self.relpath == "":
            return
        if self.dumped:
            return
        target = os.path.join(workdir, self.relpath)
        mkdir(os.path.dirname(target))
        fin = open(self.abspath, 'r')
        fout = open(target, 'w+')
        lines = fin.readlines()

        guard = '__%s__' % (self.relpath.replace('/', '_').replace('.', '_').replace('-', '_').upper())
        print >> fout, '#ifndef %s' % guard
        print >> fout, '#define %s' % guard
        previous_start_line = 0
        previous_end_line = 0
        for decl_range in sorted(self.__decls):
            if REMOVE_INLINE_DEFINITIONS and decl_range.has_body:
                cur.execute("SELECT * FROM prototypes WHERE name = '%s'" % decl_range.name)
                row = cur.fetchone()
                if row:
                    proto = row[1]
                    print >> fout, proto + ';'
                    continue

            start_line, end_line = random_fixes(lines, self.relpath, decl_range)

            if previous_end_line + 1 < start_line:
                print >> fout
            elif previous_end_line >= start_line:
                if verbose:
                    print "?!! Source range overlaped:", self.relpath, \
                        '%d-%d vs. %d-%d' % (previous_start_line, previous_end_line, start_line, end_line)
                continue

            for i in range(start_line - 1, end_line):
                print >> fout, lines[i].rstrip()

            previous_start_line = start_line
            previous_end_line = end_line
        print >> fout
        print >> fout, '#endif /* ! %s */' % guard
        self.dumped = True

    def __str__(self):
        return '[' + ', '.join(map(lambda x: str(x), sorted(self.__decls))) + ']'


parser = argparse.ArgumentParser()
parser.add_argument('-o', '--workdir', help='directory where generated headers should be placed', required=True)
parser.add_argument('-m', '--mode', help='generate headers for linux sources', default="test")
parser.add_argument('-v', '--verbose', action='store_true', help='print debug info')
parser.add_argument('--db', help='declaration database', required=True)
parser.add_argument('sources', nargs='?')
args = parser.parse_args()

workdir = args.workdir
mode = args.mode
verbose = args.verbose if args.verbose else False
if os.path.isdir(args.sources):
    sources = map(lambda x: os.path.join(args.sources, x), [f for f in os.listdir(args.sources) if f.endswith('.c')])
    linked = args.sources + '.o'
    dummy_out = args.sources + '.dummy.c'
else:
    sources = [args.sources]
    linked = os.path.splitext(args.sources)[0] + '.o'
    dummy_out = os.path.splitext(args.sources)[0] + '.dummy.c'

configure(mode)
conn = sqlite3.connect(args.db)
cur = conn.cursor()

# Phase 1
#     Generate the initial header set based on info from compiler
################################################################################
print 'Phase 1: Generate initial header set...'

cur.execute('SELECT * FROM decls')
rows = cur.fetchall()
for row in rows:
    f = row[0]
    name = row[1]
    start_line = int(row[2])
    start_col = int(row[3])
    end_line = int(row[4])
    end_col = int(row[5])
    from_macro = True if int(row[7]) == 1 else False
    has_body = True if int(row[8]) == 1 else False
    if not f:
        continue
    if not Header.headers.has_key(f):
        Header.headers[f] = Header(f)
    spos = SourcePosition(start_line, start_col)
    epos = SourcePosition(end_line, end_col)
    Header.headers[f].add_decl_range(SourceRange(spos, epos, name, KIND_IDENTIFIER, \
                                                 from_macro=from_macro, has_body=has_body))

cur.execute('SELECT * FROM macros')
rows = cur.fetchall()
for row in rows:
    f = row[0]
    name = row[1]
    start_line = int(row[2])
    start_col = int(row[3])
    end_line = int(row[4])
    end_col = int(row[5])
    if not Header.headers.has_key(f):
        Header.headers[f] = Header(f)
    spos = SourcePosition(start_line, start_col)
    epos = SourcePosition(end_line, end_col)
    Header.headers[f].add_decl_range(SourceRange(spos, epos, name, KIND_MACRO))

cur.execute('SELECT * FROM deps')
rows = cur.fetchall()
for row in rows:
    f = row[0]
    included = row[1]
    included_abspath = row[2]
    line = int(row[3])
    force_keep = int(row[4])
    if f.endswith('.c'):
        continue
    if force_keep:
        if not Header.headers.has_key(f):
            Header.headers[f] = Header(f)
        if not Header.headers.has_key(included_abspath) and not included_abspath.endswith('stdarg.h'):
            Header.headers[included_abspath] = Header(included_abspath)
    if not Header.headers.has_key(included_abspath) and not included_abspath.endswith('stdarg.h'):
        continue
    if not Header.headers.has_key(f):
        continue
    pos = SourcePosition(line, 0)
    Header.headers[f].add_decl_range(SourceRange(pos, pos, included, KIND_INCLUSION))

Header.dumpall()

# Phase 2
#     Fix compiling errors
################################################################################
def undecl_ident_handler(match):
    name = match.group(3)
    if name.startswith('struct '):
        name = name[7:]
    cur.execute("SELECT * FROM all_decls WHERE ident = '%s'" % (name))
    row = cur.fetchone()
    if not row:
        return MSG_HANDLE_FAILED
    f = row[0]
    name = row[1]
    start_line = int(row[2])
    start_col = int(row[3])
    end_line = int(row[4])
    end_col = int(row[5])
    if not f:
        return MSG_HANDLE_FAILED
    if not Header.headers.has_key(f):
        Header.headers[f] = Header(f)
    spos = SourcePosition(start_line, start_col)
    epos = SourcePosition(end_line, end_col)
    Header.headers[f].add_decl_range(SourceRange(spos, epos, name, KIND_IDENTIFIER, False))
    return MSG_HANDLE_SUCCEEDED

def skip_handler(match):
    return MSG_HANDLE_SKIPPED

error_types = []
error_types.append({
        'regex': re.compile('^([^:]+):([0-9]+):[0-9]+: error: use of undeclared identifier \'([^\']*)\'.*$'),
        'handler': undecl_ident_handler})
error_types.append({
        'regex': re.compile('([^:]+):([0-9]+):[0-9]+: note: .*'),
        'handler': skip_handler})
error_types.append({
        'regex': re.compile('^In file included from ([^:]+):([0-9]+):$'),
        'handler': skip_handler})
error_types.append({
        'regex': re.compile('^([^:]+):([0-9]+):[0-9]+: error: incomplete definition of type \'([^()]*)\'$'),
        'handler': undecl_ident_handler})
error_types.append({
        'regex': re.compile('^([^:]+):([0-9]+):[0-9]+: error: dereference of pointer to incomplete type \'([^()]*)\'$'),
        'handler': undecl_ident_handler})
error_types.append({
        'regex': re.compile('^([^:]+):([0-9]+):[0-9]+: error: offsetof of incomplete type \'([^()]*)\'$'),
        'handler': undecl_ident_handler})
error_types.append({
        'regex': re.compile('^([^:]+):([0-9]+):[0-9]+: error: variable has incomplete type \'([^()]*)\'$'),
        'handler': undecl_ident_handler})

def handle_error_msg(msg):
    for et in error_types:
        m = et['regex'].match(msg)
        if m:
            return et['handler'](m)

    return MSG_HANDLE_FAILED

print 'Phase 2: Fix compiling errors...'

logdir = os.path.splitext(workdir)[0] + '.log'
mkdir(logdir)
for f in os.listdir(logdir):
    os.remove(os.path.join(logdir, f))

clang_ignore_warnings = ['pointer-sign', 'incompatible-pointer-types', 'tautological-compare', 'return-type',
                         'shift-count-overflow', 'incompatible-library-redeclaration', 'asm-operand-widths']
clang_opts = '-c -I%s' % workdir
clang_opts += ' ' + ' '.join(map(lambda x: '-I' + os.path.join(workdir, x), additional_include_dirs))
clang_opts += ' -I%s/lib64/clang/3.3.1/include' % os.environ['TOP']
clang_opts += ' ' + cc_flags + ' ' + platform_cc_flags
clang_opts += ' -ferror-limit=100 -Werror -fno-color-diagnostics -fno-diagnostics-fixit-info -fno-caret-diagnostics'
clang_opts += ' ' + ' '.join(map(lambda x:'-Wno-'+x, clang_ignore_warnings))

max_rounds = 10
succeeded_files = 0
for source in sources:
    sys.stdout.write('%s ' % source)
    sys.stdout.flush()
    output_opts = '-o ' + os.path.splitext(source)[0] + '.o'
    compile_cmd = ' '.join([clang, clang_opts, output_opts, source])
    rounds = 1
    abort = False
    retcode = 1
    while rounds <= max_rounds and not abort:
        abort = True
        resolved = 0
        log = open(os.path.join(logdir, os.path.basename(source) + '.' + str(rounds)), 'w')
        p = Popen(compile_cmd, shell=True, stdin=None, stdout=None, stderr=PIPE, close_fds=True)
        errors = [line.strip() for line in p.stderr]
        p.communicate()
        retcode = p.returncode
        if retcode == 0:
            # @abort is already set to True. We'll get rid of here at the beginning of next loop
            continue
        for msg in errors:
            result = handle_error_msg(msg)
            if result == MSG_HANDLE_SUCCEEDED:
                log.write(msg + '\n')
                abort = False
                resolved += 1
            elif result == MSG_HANDLE_FAILED:
                log.write('*** ' + msg + '\n')
        p.stderr.close()
        log.close()
        Header.dumpall()
        if rounds % 2 == 1:
            sys.stdout.write('.')
            sys.stdout.flush()
        rounds += 1
    else:
        if not abort:
            print 'Max rounds exceeded. Double check error-resolving processes.'
        elif retcode == 0:
            cprint(' done in %d rounds' % (rounds - 1), 'green')
            succeeded_files += 1
        else:
            cprint(' failed after %d rounds' % (rounds - 1), 'red')

if not succeeded_files == len(sources):
    sys.exit(1)

# Phase 3
################################################################################
print 'Phase 3: Generate dummy implementations...'

class DummyFunc:
    def __init__(self, name):
        self.name = name
        cur = conn.cursor()
        cur.execute("SELECT * FROM prototypes WHERE name = '%s'" % name)
        row = cur.fetchone()
        if not row:
            cprint('Warning: cannot find symbol %s' % name, 'yellow')
            self.proto = ''
            self.header = ''
            return
        self.proto = row[1]
        self.header = row[2]
        self.is_function = True if row[3] == '1' else False

    def generate(self, out):
        if not self.proto:
            return
        if self.proto.startswith('extern'):
            impl = self.proto[7:] + ';\n\n'
            pass
        else:
            log = '\tdde_printf("%s not implemented\\n");\n' % self.name
            ret = '' if self.proto.startswith('void') and not self.proto.startswith('void *') else '\treturn 0;\n'
            impl = 'DDE_WEAK ' + self.proto + ' {\n' + log + ret + '}\n\n'
        out.write(impl)

if len(sources) > 1:
    link_cmd = 'ld -r -o %s %s' % (linked, ' '.join(map(lambda x:os.path.join(os.path.splitext(x)[0] + '.o'), sources)))
    p = Popen(link_cmd, shell=True, stdin=None, stdout=None, stderr=None, close_fds=True)
    p.communicate()
    if p.returncode != 0:
        print 'Error when linking'
        sys.exit(1)

nm_cmd = 'nm %s | grep " U " | sed "s/ *U //g"' % linked
p = Popen(nm_cmd, shell=True, stdin=None, stdout=PIPE, stderr=None, close_fds=True)
fns = [line.strip() for line in p.stdout]
p.communicate()
if p.returncode != 0:
    print 'Error when fetching undefined symbols'
    sys.exit(1)
p.stdout.close()

dummies = []
for fn in fns:
    dummies.append(DummyFunc(fn))

f = open(dummy_out, 'w')
for header in sorted(set([x for x in map(lambda x:x.header, dummies) if x])):
    f.write('#include <%s>\n' % Header.headers[header].relpath)
f.write('\n#define DDE_WEAK __attribute__((weak))\n\n')
f.write('#define dde_dummy_printf(...)\n#define dde_printf(...) dde_dummy_printf(__VA_ARGS__)\n\n')
for dummy in dummies:
    dummy.generate(f)
