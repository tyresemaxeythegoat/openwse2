#!/usr/bin/env python3
"""Extract the mbStatement::executeWSE opcode dispatch map.

Inputs are analysis artifacts kept outside the repository:

  * Ghidra pseudocode for ?executeWSE@mbStatement@@QEAA_NPEA_J0PEAH1H@Z
  * public WSE SDK header_operations_addon.py

The output is a JSON map of opcode -> SDK name, handler line, fallthrough
group, validation checks, direct calls, and vtable offsets. Every entry cites
the pseudocode line where the handler begins; no semantics are guessed here.
"""

import argparse
import json
import re
from pathlib import Path

CASE_RE = re.compile(r'^(\s*)case\s+(0x[0-9a-fA-F]+|\d+)\s*:')
SWITCH_RE = re.compile(r'^(\s*)switch\s*\(\s*uVar43\s*\)\s*\{?\s*$')
IF_EQ_RES = [
    re.compile(r'if\s*\(\s*uVar43\s*==\s*(0x[0-9a-fA-F]+|\d+)\s*\)'),
    re.compile(r'if\s*\(\s*(0x[0-9a-fA-F]+|\d+)\s*==\s*uVar43\s*\)'),
]
FUN_RE = re.compile(r'\b(?:FUN_|func_0x000)(140[0-9a-fA-F]{6})\b')
MANGLED_RE = re.compile(r'\b_([A-Za-z][A-Za-z0-9_]*?)__[A-Za-z0-9_]+')
MB_CHECK_RE = re.compile(r'\b(?:_)?(mbCheck[A-Za-z0-9_]*?)__')
VTABLE_RE = re.compile(r'\(\*plVar\w*\s*\+\s*(0x[0-9a-fA-F]+)\)')
SDK_OP_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+)\s*(#.*)?$')


def strip_code(line: str) -> str:
    """Remove comments and string literals so brace counting stays honest."""
    out = []
    index = 0
    in_string = False
    while index < len(line):
        char = line[index]
        if in_string:
            if char == '\\' and index + 1 < len(line):
                index += 2
                continue
            if char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            index += 1
            continue
        if line.startswith('//', index):
            break
        if line.startswith('/*', index):
            end = line.find('*/', index + 2)
            if end < 0:
                break
            index = end + 2
            continue
        out.append(char)
        index += 1
    return ''.join(out)


def brace_delta(line: str) -> int:
    code = strip_code(line)
    return code.count('{') - code.count('}')


def match_block_end(lines, start):
    """Return the last line index of a `{...}` block that starts at/after start."""
    depth = 0
    opened = False
    for index in range(start, len(lines)):
        delta = brace_delta(lines[index])
        if delta > 0:
            opened = True
        depth += delta
        if opened and depth == 0:
            return index
    return len(lines) - 1


def collect(body_lines):
    checks = set()
    calls = set()
    names = set()
    offsets = set()
    for line in body_lines:
        for match in MB_CHECK_RE.finditer(line):
            checks.add(match.group(1))
        for match in FUN_RE.finditer(line):
            calls.add('0x' + match.group(1).lower())
        for match in MANGLED_RE.finditer(line):
            names.add(match.group(1))
        for match in VTABLE_RE.finditer(line):
            offsets.add(match.group(1).lower())
    return {
        'checks': sorted(checks),
        'calls': sorted(calls),
        'symbols': sorted(names),
        'vtable_offsets': sorted(offsets),
    }


def parse_sdk(path):
    ops = {}
    for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
        match = SDK_OP_RE.match(raw)
        if not match:
            continue
        comment = (match.group(3) or '').lstrip('#').strip()
        ops[int(match.group(2))] = {'name': match.group(1), 'comment': comment}
    return ops


def parse_pseudocode(path):
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    entries = {}
    switch_report = []

    # Switch(uVar43) blocks: top-level case labels share the switch indent.
    for index, line in enumerate(lines):
        match = SWITCH_RE.match(line)
        if not match:
            continue
        switch_indent = len(match.group(1))
        end = match_block_end(lines, index)
        seen = []
        group = []  # [(value, line_no)]
        body_start = None
        last_label_line = None

        def flush(body_last):
            if not group or body_start is None or body_last < body_start:
                return
            body = lines[body_start:body_last + 1]
            data = collect(body)
            labels = [value for value, _ in group]
            for value, line_no in group:
                entries[value] = {
                    'opcode': value,
                    'hex': hex(value),
                    'line': line_no,
                    'handler': 'switch',
                    'switch_line': index + 1,
                    'group': labels,
                    **data,
                }

        for number in range(index + 1, end + 1):
            candidate = lines[number]
            case = CASE_RE.match(candidate)
            if case and len(case.group(1)) == switch_indent:
                value = int(case.group(2), 0)
                if group:
                    # Only blank lines between labels means fallthrough.
                    between = lines[last_label_line + 1:number]
                    if any(text.strip() for text in between):
                        flush(number - 1)
                        group = []
                group.append((value, number + 1))
                body_start = number + 1
                last_label_line = number
                seen.append(value)
        if group:
            flush(end - 1)
        switch_report.append({'line': index + 1, 'end': end + 1, 'cases': len(seen)})

    # Every switch body (dispatch or nested state switch) is excluded from the
    # equality-handler pass: uVar43 is reused as a temporary inside handlers.
    switch_ranges = []
    for index, line in enumerate(lines):
        if re.match(r'^\s*switch\s*\(', line):
            switch_ranges.append((index, match_block_end(lines, index)))

    # Equality-tested handlers outside the switch blocks.
    candidates = []
    for index, line in enumerate(lines):
        if any(start <= index <= end for start, end in switch_ranges):
            continue
        value = None
        for pattern in IF_EQ_RES:
            match = pattern.search(line)
            if match:
                value = int(match.group(1), 0)
                break
        if value is None or not (2768 <= value <= 5202) or value in entries:
            continue
        candidates.append((index, value))

    # Drop matches nested inside another equality handler's body (uVar43 is
    # reused as a scratch variable inside some handlers).
    kept = []
    for index, value in sorted(candidates):
        if any(start < index <= end for start, end in kept):
            continue
        end = match_block_end(lines, index)
        kept.append((index, end))
        body = lines[index + 1:end]
        entries[value] = {
            'opcode': value,
            'hex': hex(value),
            'line': index + 1,
            'handler': 'if-equality',
            'switch_line': None,
            'group': [value],
            **collect(body),
        }

    return entries, switch_report


def write_markdown(document, path):
    entries = document['entries']
    lines = []
    lines.append('# WSE2 `executeWSE` opcode map')
    lines.append('')
    lines.append('Generated by `tools/extract_wse_opcodes.py` from the Ghidra 12.1.4 pseudocode of')
    lines.append('`?executeWSE@mbStatement@@QEAA_NPEA_J0PEAH1H@Z` (`0x1401e0cf0`) and the public')
    lines.append('`header_operations_addon.py`. The machine-readable copy is `docs/wse_opcodes.json`.')
    lines.append('')
    lines.append('`executeWSE` receives the statement, the result destination, three decoded operand')
    lines.append('arrays (`int64`, `int32`, and tags), and the caller operand. It masks the opcode with')
    lines.append('`0x0FFFFFFF`. Unmatched values reach `"Unrecognized opcode %d"` (`0x1401f0525`).')
    lines.append('')
    lines.append('## Coverage')
    lines.append('')
    lines.append('| Metric | Value |')
    lines.append('| --- | ---: |')
    lines.append('| Handler opcodes | %d |' % document['summary']['handled_opcodes'])
    lines.append('| Opcode range | %d–%d |' % tuple(document['summary']['range']))
    lines.append('| Switch-block handlers | %d |' % document['summary']['switch_handlers'])
    lines.append('| Equality-test handlers | %d |' % document['summary']['if_equality_handlers'])
    lines.append('| SDK operations in range | %d |' % document['summary']['sdk_ops_in_range'])
    lines.append('| SDK operations without handler | %d |' %
                 document['summary']['sdk_ops_without_handler'])
    lines.append('')
    lines.append('## Switch blocks')
    lines.append('')
    lines.append('| Switch line | End line | Case labels |')
    lines.append('| ---: | ---: | ---: |')
    for block in document['summary']['switch_blocks']:
        lines.append('| %d | %d | %d |' % (block['line'], block['end'], block['cases']))
    lines.append('')
    lines.append('## Handlers')
    lines.append('')
    lines.append('| Opcode | Hex | Operation | Dispatch | Line | Checks | Symbols |')
    lines.append('| ---: | --- | --- | --- | ---: | --- | --- |')
    for entry in entries:
        checks = ', '.join(entry['checks'][:2])
        if len(entry['checks']) > 2:
            checks += ', …'
        symbols = ', '.join(entry['symbols'][:2])
        if len(entry['symbols']) > 2:
            symbols += ', …'
        shared = ' (shared)' if len(entry['group']) > 1 else ''
        lines.append('| %d | `%s` | `%s`%s | %s | %d | %s | %s |' % (
            entry['opcode'], entry['hex'], entry['name'], shared, entry['handler'],
            entry['line'], checks, symbols))
    lines.append('')
    text = '\n'.join(lines)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    print('wrote', path)


def write_header(document, path):
    entries = document['entries']
    reserved = {
        'alignas', 'alignof', 'and', 'and_eq', 'asm', 'auto', 'bitand', 'bitor',
        'bool', 'break', 'case', 'catch', 'char', 'char8_t', 'char16_t',
        'char32_t', 'class', 'compl', 'concept', 'const', 'consteval',
        'constexpr', 'constinit', 'const_cast', 'continue', 'co_await',
        'co_return', 'co_yield', 'decltype', 'default', 'delete', 'do',
        'double', 'dynamic_cast', 'else', 'enum', 'explicit', 'export',
        'extern', 'false', 'float', 'for', 'friend', 'goto', 'if', 'inline',
        'int', 'long', 'mutable', 'namespace', 'new', 'noexcept', 'not',
        'not_eq', 'nullptr', 'operator', 'or', 'or_eq', 'private', 'protected',
        'public', 'register', 'reinterpret_cast', 'requires', 'return',
        'short', 'signed', 'sizeof', 'static', 'static_assert', 'static_cast',
        'struct', 'switch', 'template', 'this', 'thread_local', 'throw', 'true',
        'try', 'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using',
        'virtual', 'void', 'volatile', 'wchar_t', 'while', 'xor', 'xor_eq',
    }
    lines = []
    for entry in entries:
        name = entry['name']
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name) or name in reserved:
            raise SystemExit('operation name is not a C++ identifier: ' + name)
    lines.append('// Generated by tools/extract_wse_opcodes.py from docs/wse_opcodes.json.')
    lines.append('// Do not edit by hand.')
    lines.append('//')
    lines.append('// Names come from the public WSE SDK header_operations_addon.py; the')
    lines.append('// numeric mapping is the mbStatement::executeWSE dispatch evidence at')
    lines.append('// 0x1401e0cf0. Values are the unmasked operation numbers.')
    lines.append('')
    lines.append('#pragma once')
    lines.append('')
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('namespace wse::vm {')
    lines.append('')
    lines.append('enum class WseOpcode : std::uint32_t {')
    for entry in entries:
        lines.append('    %s = %d,' % (entry['name'], entry['opcode']))
    lines.append('};')
    lines.append('')
    lines.append('}  // namespace wse::vm')
    lines.append('')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines), encoding='utf-8')
    print('wrote', path)


def main():
    here = Path(__file__).resolve().parent
    default_root = here.parent
    parser = argparse.ArgumentParser()
    parser.add_argument('--pseudocode', required=True,
                        help='Ghidra pseudocode for mbStatement::executeWSE')
    parser.add_argument('--sdk', required=True,
                        help='public header_operations_addon.py')
    parser.add_argument('--json', default=str(default_root / 'docs' / 'wse_opcodes.json'))
    parser.add_argument('--markdown',
                        default=str(default_root / 'docs' / 'WSE_OPCODES.md'))
    parser.add_argument('--header',
                        default=str(default_root / 'engine' / 'include' / 'wse' / 'vm' /
                                    'wse_opcodes.h'))
    args = parser.parse_args()

    sdk = parse_sdk(Path(args.sdk))
    entries, switches = parse_pseudocode(Path(args.pseudocode))

    handled = set(entries)
    sdk_range = {n: info for n, info in sdk.items() if 2768 <= n <= 5202}
    for value, entry in entries.items():
        info = sdk.get(value)
        entry['name'] = info['name'] if info else None
        entry['sdk_comment'] = info['comment'] if info else None

    missing = sorted(set(sdk_range) - handled)
    unknown = sorted(value for value in entries if value not in sdk_range)
    opcodes = sorted(handled)
    document = {
        'schema': 'wse2-opcode-map/1',
        'target': 'mb_warband_wse2_x64.exe',
        'function': {
            'symbol': '?executeWSE@mbStatement@@QEAA_NPEA_J0PEAH1H@Z',
            'va': '0x1401e0cf0',
            'opcode_mask': '0x0fffffff',
            'operand_arrays': {
                'int64': 'param_3',
                'int32': 'param_4',
                'tags': 'param_5',
            },
        },
        'summary': {
            'switch_blocks': switches,
            'handled_opcodes': len(handled),
            'range': [min(opcodes), max(opcodes)],
            'switch_handlers': sum(1 for e in entries.values() if e['handler'] == 'switch'),
            'if_equality_handlers': sum(
                1 for e in entries.values() if e['handler'] == 'if-equality'),
            'sdk_ops_in_range': len(sdk_range),
            'sdk_ops_without_handler': len(missing),
            'unknown_opcodes': unknown,
        },
        'entries': [entries[value] for value in opcodes],
        'sdk_ops_without_handler': [
            {'opcode': value, **sdk_range[value]} for value in missing
        ],
    }

    output = Path(args.json)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=1) + '\n', encoding='utf-8')
    print('wrote', output)
    write_markdown(document, Path(args.markdown))
    write_header(document, Path(args.header))
    print(json.dumps(document['summary'], indent=1))


if __name__ == '__main__':
    main()
