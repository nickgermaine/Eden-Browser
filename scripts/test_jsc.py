import argparse
import ctypes
import json
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('tests', type=Path, nargs='+')
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    library = runtime / 'lib/libWPEWebKit-2.0.so'
    jsc = ctypes.CDLL(str(library))
    signatures = {
        'jsc_context_new_with_virtual_machine': (ctypes.c_void_p, [ctypes.c_void_p]),
        'jsc_context_get_virtual_machine': (ctypes.c_void_p, [ctypes.c_void_p]),
        'jsc_context_get_global_object': (ctypes.c_void_p, [ctypes.c_void_p]),
        'jsc_context_set_value': (None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        'jsc_context_new': (ctypes.c_void_p, []),
        'jsc_context_evaluate': (ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_ssize_t]),
        'jsc_context_get_exception': (ctypes.c_void_p, [ctypes.c_void_p]),
        'jsc_exception_to_string': (ctypes.c_void_p, [ctypes.c_void_p]),
        'jsc_value_to_string': (ctypes.c_void_p, [ctypes.c_void_p]),
        'g_object_unref': (None, [ctypes.c_void_p]),
        'g_free': (None, [ctypes.c_void_p]),
    }
    for name, (result, arguments) in signatures.items():
        fn = getattr(jsc, name)
        fn.restype = result
        fn.argtypes = arguments
    results = []
    failed = False
    for path in args.tests:
        context = jsc.jsc_context_new()
        other = jsc.jsc_context_new_with_virtual_machine(jsc.jsc_context_get_virtual_machine(context))
        global_object = jsc.jsc_context_get_global_object(other)
        jsc.jsc_context_set_value(context, b'otherTestRealm', global_object)
        source = ('var testLoopCount = 1000; function createGlobalObject() { return otherTestRealm; }\n' + path.read_text()).encode()
        start = time.monotonic()
        value = jsc.jsc_context_evaluate(context, source, len(source))
        exception = jsc.jsc_context_get_exception(context)
        message = jsc.jsc_exception_to_string(exception) if exception else jsc.jsc_value_to_string(value)
        result = {'test': str(path), 'passed': not bool(exception), 'result': ctypes.string_at(message).decode(), 'seconds': time.monotonic() - start}
        failed |= bool(exception)
        results.append(result)
        print(json.dumps(result), flush=True)
        jsc.g_free(message)
        if value:
            jsc.g_object_unref(value)
        jsc.g_object_unref(global_object)
        jsc.g_object_unref(other)
        jsc.g_object_unref(context)
    if args.output:
        args.output.write_text(json.dumps({'runtime': str(runtime), 'environment': {k: v for k, v in os.environ.items() if k.startswith(('JSC_', 'WEBKIT_EDEN_JSC_'))}, 'tests': results}, indent=2) + '\n')
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
