"""Actual frozen Python loader, output protocol and force-aware service checks."""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow import piezo_excitation as pe
from range_workflow import piezo_external_time as ext
from range_workflow.piezo_native_backend import NativeBackend
from test_piezo_native_backend import configuration, forced_configuration, readouts


class ExternalTimeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)/"force.py"
        self.catalog = (pe.ForceChannel("force:7:X", 7, "X", "N"),)

    def record(self, body=None, dependencies=()):
        if body is None:
            body = 'def load(t):\n    return {"force:7:X": {"value": 2+3*t, "unit": "N"}}\n'
        self.path.write_text(body, encoding="utf-8")
        return {"sourcePath": str(self.path), "functionName": "load",
                "sourceSha256": hashlib.sha256(self.path.read_bytes()).hexdigest(),
                "dependencyFiles": [{"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                                    for p in dependencies], "environment": ext.environment(),
                "channels": [{"channelId": "force:7:X", "unit": "N"}],
                "missingPolicy": "error", "cacheExactTime": False}

    def compile(self, record):
        return pe.compile_excitation(self.catalog, {"channels": [], "externalFunctions": [record]})

    def wrapper(self, record):
        return self.compile(record)._external[0]

    def stdio(self, record, preload="", probe=""):
        message = configuration("none")
        message["configuration"]["excitation"] = {"channels": [], "externalFunctions": [record]}
        requests = [message, {"op":"force", "time":1.}, {"op":"force", "time":2.}]
        bootstrap = (preload+"\nimport sys,runpy;sys.path.insert(0,"+
            repr(str(Path(__file__).resolve().parents[1]/"python"))+
            ");sys.argv=['range_workflow.piezo_native_backend','--stdio'];"
            "\ntry:\n    runpy.run_module('range_workflow.piezo_native_backend',run_name='__main__')"
            "\nexcept SystemExit as exc:\n    assert exc.code == 0\n"+probe)
        return subprocess.run([sys.executable,"-I","-B","-c",bootstrap],
            input="\n".join(json.dumps(r) for r in requests)+"\n",text=True,capture_output=True,timeout=30)

    def initialize(self, record, message=None):
        message = forced_configuration("MPC") if message is None else message
        message["configuration"]["excitation"] = {"channels": [], "externalFunctions": [record]}
        backend = NativeBackend()
        result = backend.handle(message)
        self.assertTrue(result["ok"], result)
        return backend, result["result"], message

    def test_real_source_endpoints_and_reproducible_loaded_identity(self):
        record = self.record()
        a, b = self.compile(record), self.compile(record)
        self.assertEqual(a.source_hash, b.source_hash)
        self.assertEqual(a._external[0].loaded_code_hash, b._external[0].loaded_code_hash)
        self.assertEqual(a.evaluate(0)["forces"]["force:7:X"], {"value": 2., "unit": "N"})
        self.assertEqual(a.evaluate(.5)["forces"]["force:7:X"]["value"], 3.5)
        with self.assertRaises(pe.ExcitationError):
            a.evaluate_frame({"time": .5, "q": [0.]})

    def test_schema_rejects_unknown_duplicate_and_wrong_units(self):
        record = self.record()
        variants = [{"sourcePath": "relative.py"}, {"sourceSha256": "x"*64},
                    {"sourceSha256": "0"*64}, {"functionName": "load.q"},
                    {"functionName": "missing"}, {"cacheExactTime": 1}, {"missingPolicy": "hold"},
                    {"environment": {**record["environment"], "pythonVersion": "wrong"}},
                    {"channels": [{"channelId": "unknown", "unit": "N"}]},
                    {"channels": [{"channelId": "force:7:X", "unit": "N m"}]},
                    {"channels": record["channels"]*2}, {"channels": []},
                    {"dependencyFiles": [{"path": str(self.path), "sha256": record["sourceSha256"]}]}]
        for update in variants:
            with self.subTest(update=update), self.assertRaises(pe.ExcitationError):
                self.compile({**record, **update})
        for records in ([record, record], [record, {**record, "functionName": "other"}]):
            with self.assertRaises(pe.ExcitationError):
                pe.compile_excitation(self.catalog, {"channels": [], "externalFunctions": records})

    def test_required_t_only_signature(self):
        for signature in ("t, q", "t=0", "*t", "**t", "time"):
            with self.subTest(signature=signature), self.assertRaises(pe.ExcitationError):
                self.compile(self.record(f'def load({signature}):\n    return {{}}\n'))
        self.wrapper(self.record('def load(t, /):\n    return {}\n'))

    def test_output_policy_unknown_unit_nonfinite_and_failure(self):
        for output in ('{}', '{"unknown": {"value": 1, "unit": "N"}}',
                       '{"force:7:X": {"value": 1, "unit": "V"}}',
                       '{"force:7:X": {"value": float("inf"), "unit": "N"}}',
                       '{"force:7:X": {"value": True, "unit": "N"}}',
                       '{"force:7:X": {"value": 1, "unit": "N", "extra": 0}}', '[]'):
            record = self.record('def load(t):\n    return '+output+'\n')
            with self.subTest(output=output), self.assertRaises(pe.ExcitationError):
                self.wrapper(record).evaluate(0.)
        record = self.record('def load(t):\n    return {}\n')
        record["missingPolicy"] = "zero"
        self.assertEqual(self.wrapper(record).evaluate(0.), {"force:7:X": 0.})
        record = self.record('def load(t):\n    raise RuntimeError("failed")\n')
        with self.assertRaisesRegex(pe.ExcitationError, "failed"):
            self.wrapper(record).evaluate(0.)

    def test_source_dependency_and_environment_revision_even_on_cache(self):
        dependency = Path(self.temp.name)/"payload.txt"
        dependency.write_text("frozen", encoding="utf-8")
        record = self.record(dependencies=(dependency,))
        record["cacheExactTime"] = True
        wrapper = self.wrapper(record)
        wrapper.evaluate(1.)
        dependency.write_text("revised", encoding="utf-8")
        with self.assertRaisesRegex(pe.ExcitationError, "SHA256"):
            wrapper.evaluate(1.)
        dependency.write_text("frozen", encoding="utf-8")
        with patch.object(ext.np, "__version__", "revised"):
            with self.assertRaisesRegex(pe.ExcitationError, "environment"):
                wrapper.evaluate(1.)
        self.path.write_text(self.path.read_text()+"\n# changed\n", encoding="utf-8")
        with self.assertRaisesRegex(pe.ExcitationError, "SHA256"):
            wrapper.evaluate(1.)

    def test_dependency_code_is_loaded_from_frozen_bytes_and_checked(self):
        dependency = Path(self.temp.name)/"range_test_force_helper.py"
        dependency.write_text('def force(t):\n    return 4*t\n', encoding="utf-8")
        record = self.record('from range_test_force_helper import force\ndef load(t):\n    return {"force:7:X": {"value": force(t), "unit": "N"}}\n', (dependency,))
        # A stale global import must not override the declared source.
        from types import ModuleType
        stale = ModuleType("range_test_force_helper")
        stale.force = lambda t: 999.
        with patch.dict(sys.modules, {"range_test_force_helper": stale}):
            wrapper = self.wrapper(record)
            self.assertEqual(wrapper.evaluate(.25)["force:7:X"], 1.)
        def replacement(t):
            return 999.
        wrapper.function.__globals__["force"].__code__ = replacement.__code__
        with self.assertRaisesRegex(pe.ExcitationError, "loaded dependency"):
            wrapper.evaluate(.25)

    def test_undeclared_local_helper_and_module_name_collision_rejected(self):
        dependency = Path(self.temp.name)/"local_helper.py"
        dependency.write_text('value=1\n', encoding="utf-8")
        for body in ('import local_helper\ndef load(t):\n    return {}\n',
                     'def load(t):\n    import local_helper\n    return {}\n'):
            record = self.record(body)
            with self.subTest(body=body), self.assertRaisesRegex(pe.ExcitationError, "must be declared"):
                self.wrapper(record).evaluate(0.)
        collision = Path(self.temp.name)/"numpy.py"
        collision.write_text('value=1\n', encoding="utf-8")
        with self.assertRaisesRegex(pe.ExcitationError, "unique module"):
            self.wrapper(self.record(dependencies=(collision,)))

    def test_standard_dataclass_decorator_in_frozen_helper_restores_global_import(self):
        dependency = Path(self.temp.name)/"declared_dataclass_helper.py"
        dependency.write_text('from __future__ import annotations\nfrom dataclasses import dataclass\n'
            '@dataclass\nclass Law:\n    gain: float = 4.\n    def force(self, t):\n        return self.gain*t\n', encoding="utf-8")
        record = self.record('from declared_dataclass_helper import Law\n'
            'def load(t):\n    return {"force:7:X": {"value": Law().force(t), "unit": "N"}}\n', (dependency,))
        wrapper = self.wrapper(record)
        self.assertNotIn("declared_dataclass_helper", sys.modules)
        self.assertNotIn("_range_external_time", sys.modules)
        self.assertEqual(wrapper.evaluate(.5)["force:7:X"], 2.)

    def test_declared_helper_named_like_internal_main_module_is_scoped_and_reproducible(self):
        dependency = Path(self.temp.name)/"_range_external_time.py"
        dependency.write_text('from __future__ import annotations\nfrom dataclasses import dataclass\n'
            '@dataclass\nclass Law:\n    gain: float = 4.\n'
            '    def force(self, t):\n        return self.gain*t\n', encoding="utf-8")
        record = self.record('import _range_external_time\ndef load(t):\n'
            '    return {"force:7:X": {"value": _range_external_time.Law().force(t), "unit": "N"}}\n', (dependency,))
        from types import ModuleType
        stale = ModuleType("_range_external_time")
        with patch.dict(sys.modules, {"_range_external_time": stale}):
            first, second = self.compile(record), self.compile(record)
            self.assertIs(sys.modules["_range_external_time"], stale)
            self.assertEqual(first.evaluate(.5)["forces"]["force:7:X"]["value"], 2.)
            self.assertEqual(first.source_hash, second.source_hash)
            self.assertEqual(first._external[0].loaded_code_hash, second._external[0].loaded_code_hash)
            main_name = first._external[0].function.__module__
            self.assertNotEqual(main_name, "_range_external_time")
            self.assertNotIn(main_name, sys.modules)
            with patch.dict(sys.modules, {main_name: stale}):
                third = self.compile(record)
                self.assertIs(sys.modules[main_name], stale)
                self.assertEqual(first._external[0].loaded_code_hash, third._external[0].loaded_code_hash)

    @unittest.skipUnless(os.name == "nt", "Windows handle ownership")
    def test_windows_partial_setup_restores_streams_and_repeated_guards_do_not_leak_handles(self):
        import ctypes
        libraries = []
        for name in ("msvcr100", "msvcr110", "msvcr120"):
            try:
                libraries.append(ctypes.CDLL(name))
            except OSError:
                pass
        windows = ext._WINDOWS_STREAMS
        duplicate = windows.kernel.DuplicateHandle
        calls = 0
        def fail_second_duplicate(*args):
            nonlocal calls
            calls += 1
            return 0 if calls == 2 else duplicate(*args)
        with patch.object(windows.kernel, "DuplicateHandle", side_effect=fail_second_duplicate):
            with self.assertRaisesRegex(pe.ExcitationError, "duplication failed"):
                with ext.guarded_output():
                    self.fail("Partial output setup must reject before calling user code")
        wrapper = self.wrapper(self.record())
        self.assertEqual(wrapper.evaluate(0.)["force:7:X"], 2.)
        get_count = windows.kernel.GetProcessHandleCount
        get_count.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        get_count.restype = ctypes.c_int
        def handles():
            count = ctypes.c_ulong()
            self.assertTrue(get_count(windows.kernel.GetCurrentProcess(), ctypes.byref(count)))
            return count.value
        before = handles()
        for k in range(256):
            self.assertEqual(wrapper.evaluate(float(k))["force:7:X"], 2.+3*k)
        self.assertLessEqual(handles(), before+2)

    @unittest.skipUnless(os.name == "nt", "Windows CRT and process standard handles")
    def test_stdio_common_crt_and_win32_output_are_quarantined_and_restored(self):
        crts = {
            "msvcrt": ('import ctypes\n        crt=ctypes.CDLL("msvcrt")\n'
                '        crt.printf.argtypes=[ctypes.c_char_p]\n'
                '        crt.printf(b"MSVCRT_LOG\\n")\n'),
            "ucrtbase": ('import ctypes\n        crt=ctypes.CDLL("ucrtbase")\n'
                '        crt.__acrt_iob_func.argtypes=[ctypes.c_uint]\n'
                '        crt.__acrt_iob_func.restype=ctypes.c_void_p\n'
                '        crt.__stdio_common_vfprintf.argtypes=[ctypes.c_uint64,ctypes.c_void_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p]\n'
                '        crt.__stdio_common_vfprintf(0,crt.__acrt_iob_func(1),b"UCRT_LOG\\n",None,None)\n')}
        instructions = {name+"-buffered": text for name,text in crts.items()}
        instructions.update({name+"-immediate": text+'        crt.fflush(None)\n' for name,text in crts.items()})
        for number in (-11,-12):
            instructions["win32-"+str(number)] = ('import ctypes\n        kernel=ctypes.WinDLL("kernel32",use_last_error=True)\n'
                '        kernel.GetStdHandle.argtypes=[ctypes.c_ulong]\n'
                '        kernel.GetStdHandle.restype=ctypes.c_void_p\n'
                '        kernel.WriteFile.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.c_void_p]\n'
                '        written=ctypes.c_ulong()\n'
                '        assert kernel.WriteFile(kernel.GetStdHandle('+str(number)+'),b"WIN32_LOG\\n",10,ctypes.byref(written),None)\n')
        restore_probe = ('\nimport ctypes,os\nos.write(2,b"PY_RESTORED\\n")\n'
            'for name in ("msvcrt","ucrtbase"):\n'
            '    crt=ctypes.CDLL(name)\n    crt._write.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]\n'
            '    assert crt._write(2,b"CRT_RESTORED\\n",13)==13\n'
            'kernel=ctypes.WinDLL("kernel32",use_last_error=True)\n'
            'kernel.GetStdHandle.argtypes=[ctypes.c_ulong]\nkernel.GetStdHandle.restype=ctypes.c_void_p\n'
            'kernel.WriteFile.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.c_void_p]\n'
            'written=ctypes.c_ulong()\nassert kernel.WriteFile(kernel.GetStdHandle(-12),b"WIN32_RESTORED\\n",15,ctypes.byref(written),None)\n')
        for label,instruction in instructions.items():
            with self.subTest(output=label):
                record = self.record('def load(t):\n    if t == 1:\n        '+instruction+
                    '    return {"force:7:X": {"value": t, "unit": "N"}}\n')
                result = self.stdio(record,probe=restore_probe)
                self.assertEqual(result.returncode,0,result.stderr)
                lines = result.stdout.splitlines()
                self.assertEqual(len(lines),3,(label,result.stdout,result.stderr))
                responses = [json.loads(line) for line in lines]
                self.assertEqual([r["ok"] for r in responses],[True,False,True])
                self.assertIn("stdout/stderr",responses[1]["error"])
                self.assertEqual(responses[2]["result"]["forces"],[2.]+[0.]*11)
                self.assertEqual(result.stderr,"PY_RESTORED\nCRT_RESTORED\nCRT_RESTORED\nWIN32_RESTORED\n")

    @unittest.skipUnless(os.name == "nt", "Windows legacy dynamic CRT tables")
    def test_stdio_preloaded_and_late_loaded_legacy_crts_reject_buffered_and_immediate_logs(self):
        import ctypes
        available = []
        for name in ("msvcr100", "msvcr110", "msvcr120"):
            try:
                ctypes.CDLL(name)
            except OSError:
                continue
            available.append(name)
        if not available:
            self.skipTest("No legacy dynamic MSVCR runtime is installed")
        for name in available:
            setup = ('import ctypes\ncrt=ctypes.CDLL('+repr(name)+')\n'
                'crt.printf.argtypes=[ctypes.c_char_p]\ncrt.printf(b"")\ncrt.fflush(None)\n')
            probe = ('\nimport ctypes,os\nos.write(2,b"PY_RESTORED\\n")\n'
                'crt=ctypes.CDLL('+repr(name)+')\n'
                'crt._write.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]\n'
                'assert crt._write(2,b"CRT_RESTORED\\n",13)==13\n'
                'kernel=ctypes.WinDLL("kernel32",use_last_error=True)\n'
                'kernel.GetStdHandle.argtypes=[ctypes.c_ulong]\nkernel.GetStdHandle.restype=ctypes.c_void_p\n'
                'kernel.WriteFile.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.c_void_p]\n'
                'written=ctypes.c_ulong()\nassert kernel.WriteFile(kernel.GetStdHandle(-12),b"WIN32_RESTORED\\n",15,ctypes.byref(written),None)\n')
            for preloaded in (True,False):
                for stream in (1,2):
                    for immediate in (False,True):
                        with self.subTest(crt=name,preloaded=preloaded,stream=stream,immediate=immediate):
                            # These legacy runtimes publish the standard _iobuf
                            # layout. Verify the selected FILE's descriptor before
                            # using it, rather than treating an address as stderr.
                            body = ('def load(t):\n    if t == 1:\n'
                                '        import ctypes\n        crt=ctypes.CDLL('+repr(name)+')\n'
                                '        class Iobuf(ctypes.Structure):\n'
                                '            _fields_=[("ptr",ctypes.c_void_p),("cnt",ctypes.c_int),'
                                '("base",ctypes.c_void_p),("flag",ctypes.c_int),("file",ctypes.c_int),'
                                '("charbuf",ctypes.c_int),("bufsiz",ctypes.c_int),("tmpfname",ctypes.c_void_p)]\n'
                                '        crt.__iob_func.restype=ctypes.c_void_p\n'
                                '        pointer=crt.__iob_func()+ctypes.sizeof(Iobuf)*'+str(stream)+'\n'
                                '        crt._fileno.argtypes=[ctypes.c_void_p]\n'
                                '        assert crt._fileno(pointer)=='+str(stream)+'\n'
                                '        crt.fprintf.argtypes=[ctypes.c_void_p,ctypes.c_char_p]\n'
                                '        assert crt.fprintf(pointer,b"LEGACY_CRT_LOG\\n")>0\n'+
                                ('        crt.fflush(None)\n' if immediate else '')+
                                '    return {"force:7:X": {"value": t, "unit": "N"}}\n')
                            result = self.stdio(self.record(body),preload=setup if preloaded else "",probe=probe)
                            self.assertEqual(result.returncode,0,result.stderr)
                            lines = result.stdout.splitlines()
                            self.assertEqual(len(lines),3,(len(lines),result.stdout[-500:],result.stderr))
                            responses = [json.loads(line) for line in lines]
                            self.assertEqual([r["ok"] for r in responses],[True,False,True],responses[-2:])
                            self.assertIn("stdout/stderr",responses[1]["error"])
                            self.assertEqual(responses[2]["result"]["forces"],[2.]+[0.]*11)
                            self.assertEqual(result.stderr,"PY_RESTORED\nCRT_RESTORED\nWIN32_RESTORED\n")

    @unittest.skipUnless(os.name == "nt", "Windows late CRT adoption rollback")
    def test_late_crt_pair_allocation_faults_restore_both_streams_or_exit_fatally(self):
        import ctypes
        available=[]
        for name in ("msvcr100","msvcr110","msvcr120"):
            try:ctypes.CDLL(name)
            except OSError:continue
            available.append(name)
        if not available:self.skipTest("No legacy dynamic MSVCR runtime is installed")
        for name in available:
            for fault in ("first","second","after-pair","persistent"):
                with self.subTest(crt=name,fault=fault):
                    # Patch the actual CRT descriptor allocator after discovery;
                    # duplicate OS handles still get created and must be closed.
                    preload = ('import sys\nsys.path.insert(0,'+repr(str(Path(__file__).resolve().parents[1]/"python"))+')\n'
                        'from range_workflow import piezo_external_time as ext\n'
                        'streams=ext._WINDOWS_STREAMS\nrefresh=streams.refresh\n'
                        'fault='+repr(fault)+'\ntarget='+repr(name)+'\n'
                        'def discover():\n'
                        '    added=refresh()\n'
                        '    for crt in added:\n'
                        '        if crt._name!=target:continue\n'
                        '        original=crt._open_osfhandle\n        calls=[0]\n'
                        '        def allocate(handle,flags,original=original,calls=calls):\n'
                        '            calls[0]+=1\n'
                        '            if fault=="persistent" or calls[0]==({"first":1,"second":2}.get(fault,-1)):\n'
                        '                return -1\n'
                        '            return original(handle,flags)\n'
                        '        crt._open_osfhandle=allocate\n'
                        '    return added\n'
                        'streams.refresh=discover\n'
                        'adopt=streams.adopt_loaded\nraised=[False]\n'
                        'def adopt_then_throw(saved,handles):\n'
                        '    before={id(c) for c,_,_ in saved}\n'
                        '    adopt(saved,handles)\n'
                        '    if fault=="after-pair" and not raised[0] and any(c._name==target and id(c) not in before for c,_,_ in saved):\n'
                        '        raised[0]=True\n        raise RuntimeError("injected after paired publication")\n'
                        'streams.adopt_loaded=adopt_then_throw\n')
                    record=self.record('def load(t):\n    if t==1:\n'
                        '        import ctypes\n        crt=ctypes.CDLL('+repr(name)+')\n'
                        '        crt.printf.argtypes=[ctypes.c_char_p]\n'
                        '        crt.printf(b"ADOPTION_BUFFERED_LOG\\n")\n'
                        '    return {"force:7:X":{"value":t,"unit":"N"}}\n')
                    probe = ('\nimport ctypes,msvcrt,os\n'
                        'kernel=streams.kernel\ncompare=ctypes.WinDLL("kernelbase").CompareObjectHandles\n'
                        'compare.argtypes=[ctypes.c_void_p,ctypes.c_void_p]\ncompare.restype=ctypes.c_int\n'
                        'for crt in streams.crts:\n'
                        '    crt._write.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]\n'
                        '    for fd,number in ((1,-11),(2,-12)):\n'
                        '        assert crt._write(fd,b"",0)==0\n'
                        '        handle=crt._get_osfhandle(fd)\n'
                        '        assert compare(handle,kernel.GetStdHandle(number))\n'
                        '        assert not compare(handle,kernel.GetStdHandle(-12 if fd==1 else -11))\n'
                        'for fd,number in ((1,-11),(2,-12)):\n'
                        '    assert compare(msvcrt.get_osfhandle(fd),kernel.GetStdHandle(number))\n'
                        'get_count=kernel.GetProcessHandleCount\n'
                        'get_count.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]\n'
                        'def count():\n    value=ctypes.c_ulong()\n'
                        '    assert get_count(kernel.GetCurrentProcess(),ctypes.byref(value))\n    return value.value\n'
                        'before=count()\n'
                        'for _ in range(256):\n    with ext.guarded_output():pass\n'
                        'assert count()<=before+2\n'
                        'os.write(2,b"PY_RESTORED\\n")\n'
                        'for crt in streams.crts:\n    assert crt._write(2,b"CRT_RESTORED\\n",13)==13\n'
                        'kernel.WriteFile.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.c_void_p]\n'
                        'written=ctypes.c_ulong()\n'
                        'assert kernel.WriteFile(kernel.GetStdHandle(-12),b"WIN32_RESTORED\\n",15,ctypes.byref(written),None)\n')
                    result=self.stdio(record,preload=preload,probe=probe)
                    lines=result.stdout.splitlines()
                    if fault=="persistent":
                        self.assertEqual(result.returncode,ext.FATAL_OUTPUT_EXIT_CODE,(result.stdout,result.stderr))
                        self.assertEqual(len(lines),1,(result.stdout,result.stderr))
                        self.assertTrue(json.loads(lines[0])["ok"])
                        self.assertEqual(result.stderr,"")
                    else:
                        self.assertEqual(result.returncode,0,result.stderr)
                        self.assertEqual(len(lines),3,(result.stdout[-500:],result.stderr))
                        responses=[json.loads(line) for line in lines]
                        self.assertEqual([r["ok"] for r in responses],[True,False,True])
                        self.assertIn("stdout/stderr cleanup failed",responses[1]["error"])
                        self.assertEqual(responses[2]["result"]["forces"],[2.]+[0.]*11)
                        self.assertEqual(result.stderr,"PY_RESTORED\n"+"CRT_RESTORED\n"*3+"WIN32_RESTORED\n")

    def test_mutated_class_method_and_dependency_after_function_are_rejected(self):
        body = ('class Law:\n    def force(self, t):\n        return t\n'
                'law=Law()\ndef load(t):\n    return {"force:7:X": {"value": law.force(t), "unit": "N"}}\n')
        wrapper = self.wrapper(self.record(body))
        def replacement(self, t):
            return 99.
        wrapper.function.__globals__["Law"].force.__code__ = replacement.__code__
        with self.assertRaisesRegex(pe.ExcitationError, "loaded dependency"):
            wrapper.evaluate(0.)
        dependency = Path(self.temp.name)/"data.txt"
        dependency.write_text("frozen", encoding="utf-8")
        body = ('from pathlib import Path\ndef load(t):\n    Path('+repr(str(dependency))+
                ').write_text("changed")\n    return {"force:7:X": {"value": t, "unit": "N"}}\n')
        wrapper = self.wrapper(self.record(body, (dependency,)))
        with self.assertRaisesRegex(pe.ExcitationError, "SHA256"):
            wrapper.evaluate(0.)
        self.assertEqual(wrapper._observed, {})

    def test_system_exit_and_async_are_explicit_external_errors(self):
        record = self.record('def load(t):\n    raise SystemExit(1)\n')
        with self.assertRaisesRegex(pe.ExcitationError, "failed"):
            self.wrapper(record).evaluate(0.)
        record = self.record('raise SystemExit(1)\ndef load(t):\n    return {}\n')
        with self.assertRaisesRegex(pe.ExcitationError, "load failed"):
            self.wrapper(record)
        record = self.record('async def load(t):\n    return {}\n')
        with self.assertRaisesRegex(pe.ExcitationError, "Python function"):
            self.wrapper(record)

    def test_signed_zero_retry_detection_both_cache_settings(self):
        for cache in (False, True):
            record = self.record('n=0\ndef load(t):\n    global n\n    n+=1\n    return {"force:7:X": {"value": 0.0 if n == 1 else -0.0, "unit": "N"}}\n')
            record["cacheExactTime"] = cache
            wrapper = self.wrapper(record)
            wrapper.evaluate(1.)
            with self.assertRaisesRegex(pe.ExcitationError, "nondeterministic"):
                wrapper.evaluate(1.)
        record = self.record('def load(t):\n    return {"force:7:X": {"value": t, "unit": "N"}}\n')
        wrapper = self.wrapper(record)
        wrapper.evaluate(0.)
        wrapper.evaluate(-0.)
        self.assertEqual(len(wrapper._observed), 2)

    def test_observation_and_cache_are_bounded_and_exact_time(self):
        record = self.record()
        record["cacheExactTime"] = True
        wrapper = self.wrapper(record)
        for k in range(4100):
            wrapper.evaluate(float(k))
        self.assertEqual(len(wrapper._cache), 4096)
        self.assertEqual(len(wrapper._observed), 4096)
        wrapper.evaluate(math.nextafter(4099., math.inf))
        self.assertEqual(len(wrapper._cache), 4096)

    def test_output_quarantine_stream_descriptor_and_module_import(self):
        for instruction in ('print("pollution")', 'import sys; print("pollution", file=sys.stderr)',
                            'import os; os.write(1, b"pollution\\n")',
                            'import os; os.write(2, b"x"*1000000)'):
            record = self.record('def load(t):\n    '+instruction+'\n    return {}\n')
            wrapper = self.wrapper(record)
            with self.subTest(instruction=instruction), self.assertRaisesRegex(pe.ExcitationError, "stdout/stderr"):
                wrapper.evaluate(0.)
        record = self.record('print("import pollution")\ndef load(t):\n    return {}\n')
        with self.assertRaisesRegex(pe.ExcitationError, "stdout/stderr"):
            self.wrapper(record)

    def test_output_validation_of_user_mapping_is_also_quarantined(self):
        record = self.record('from collections.abc import Mapping\n'
            'class Output(Mapping):\n'
            '    def __iter__(self):\n        print("iteration pollution")\n        return iter([])\n'
            '    def __len__(self):\n        return 0\n'
            '    def __getitem__(self, key):\n        raise KeyError(key)\n'
            'def load(t):\n    return Output()\n')
        with self.assertRaisesRegex(pe.ExcitationError, "stdout/stderr"):
            self.wrapper(record).evaluate(0.)

    def test_external_adds_to_explicit_builtin_terms_and_pure_sensor_none(self):
        record = self.record()
        config = {"channels": [{"channelId": "force:7:X", "unit": "N",
                               "terms": [{"kind": "constant", "value": 10.}]}],
                  "externalFunctions": [record]}
        proposal = pe.compile_excitation(self.catalog, config)
        self.assertEqual(proposal.evaluate(1.)["forces"]["force:7:X"]["value"], 15.)
        message = forced_configuration("none")
        message.update(inputs=[], B=[[]], D=[[]], actuatorVoltages0=[])
        backend, initial, _ = self.initialize(record, message)
        state = initial["initialOpaqueState"]
        response = backend.handle({"op": "propose", "opaqueState": state})
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["result"]["absoluteVoltages"], [])
        response = backend.handle({"op": "postFE", "opaqueState": response["result"]["candidateOpaqueState"],
                                   "appliedVoltages": []})
        self.assertTrue(response["ok"], response)
        self.assertTrue(backend.handle({"op": "validate", "opaqueState": response["result"]["candidateOpaqueState"],
                                      "heldVoltages": [], "appliedVoltages": []})["ok"])

    def test_external_mpc_endpoint_forecast_retry_and_restore_matches_builtin(self):
        record = self.record()
        backend, initial, message = self.initialize(record)
        reference = NativeBackend()
        expected = reference.handle(forced_configuration("MPC"))
        self.assertTrue(expected["ok"], expected)
        state = initial["initialOpaqueState"]
        request = {"op": "propose", "opaqueState": state, "q": [.1], "v": [.02]}
        first = backend.handle(request)
        self.assertTrue(first["ok"], first)
        self.assertEqual(first, backend.handle(request))
        expected_proposal = reference.handle({**request, "opaqueState": expected["result"]["initialOpaqueState"]})
        self.assertEqual(first["result"]["absoluteVoltages"], expected_proposal["result"]["absoluteVoltages"])
        self.assertEqual(first["result"]["diagnostics"], expected_proposal["result"]["diagnostics"])
        self.assertEqual(initial["design"]["forceAware"]["qualification"], "complete-known-forcing")
        self.assertEqual(backend.handle({"op": "force", "time": 1.7})["result"]["forces"][0], 7.1)
        fresh, _, _ = self.initialize(record, copy.deepcopy(message))
        self.assertEqual(fresh.handle(request), first)
        self.assertEqual(state, initial["initialOpaqueState"])
        # Source change invalidates force, proposal and validate before history changes.
        self.path.write_text(self.path.read_text()+"\n# changed\n", encoding="utf-8")
        for request in ({"op": "force", "time": 1.7}, request, {"op": "validate", "opaqueState": state}):
            result = backend.handle(request)
            self.assertFalse(result["ok"], result)
        changed_record = copy.deepcopy(record)
        changed_record["sourceSha256"] = hashlib.sha256(self.path.read_bytes()).hexdigest()
        changed, _, _ = self.initialize(changed_record, copy.deepcopy(message))
        result = changed.handle({"op": "validate", "opaqueState": state})
        self.assertFalse(result["ok"], result)
        self.assertIn("identity mismatch", result["error"])

    def test_external_force_mapping_is_required_for_mpc_and_sensor_only(self):
        record = self.record()
        for kind, settings in (("MPC", {}), ("LQR", {"stateSource": "sensor_only",
                "observer": {"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]}})):
            message = configuration(kind, **settings)
            message["configuration"]["excitation"] = {"channels": [], "externalFunctions": [record]}
            result = NativeBackend().handle(message)
            self.assertFalse(result["ok"], result)
            self.assertIn("requires native force operators", result["error"])

    def test_external_failure_does_not_consume_qp_or_observer_state(self):
        record = self.record('def load(t):\n    if t > 1.8:\n        raise RuntimeError("forecast failed")\n    return {"force:7:X": {"value": 2+3*t, "unit": "N"}}\n')
        backend, initial, _ = self.initialize(record)
        state = initial["initialOpaqueState"]
        before = copy.deepcopy(state)
        request = {"op": "propose", "opaqueState": state, "q": [.1], "v": [.02]}
        for _ in range(2):
            self.assertFalse(backend.handle(request)["ok"])
            self.assertEqual(state, before)
        self.assertEqual(backend.handle({"op": "validate", "opaqueState": state})["ok"], True)

    def test_nested_class_code_freeze_handles_cycles_and_pre_evaluation_mutation(self):
        record = self.record('class Outer:\n    class Inner:\n        def force(self, t):\n            return t\n'
            'Outer.Inner.cycle = Outer\n'
            'def load(t):\n    return {"force:7:X": {"value": Outer.Inner().force(t), "unit": "N"}}\n')
        wrapper = self.wrapper(record)
        self.assertEqual(wrapper.evaluate(1.)["force:7:X"], 1.)
        def replacement(self, t):
            return 999.
        wrapper.function.__globals__["Outer"].Inner.force.__code__ = replacement.__code__
        with self.assertRaisesRegex(pe.ExcitationError, "loaded dependency/function code changed"):
            wrapper.evaluate(2.)

    def test_nested_class_code_changed_during_evaluation_is_rejected(self):
        record = self.record('class Outer:\n    class Inner:\n        def force(self, t):\n            return t\n'
            'def replacement(self, t):\n    return 999.\n'
            'def load(t):\n    Outer.Inner.force.__code__ = replacement.__code__\n'
            '    return {"force:7:X": {"value": Outer.Inner().force(t), "unit": "N"}}\n')
        with self.assertRaisesRegex(pe.ExcitationError, "loaded dependency/function code changed"):
            self.wrapper(record).evaluate(2.)

    def test_stdio_output_fault_recovers_one_response_per_request(self):
        record = self.record('def load(t):\n    if t == 1:\n        import os\n        os.write(1, b"not JSON\\n")\n    return {"force:7:X": {"value": t, "unit": "N"}}\n')
        message = configuration("none")
        message["configuration"]["excitation"] = {"channels": [], "externalFunctions": [record]}
        requests = [message, {"op": "force", "time": 1.}, {"op": "force", "time": 2.}]
        bootstrap = ("import sys,runpy;sys.path.insert(0,"+repr(str(Path(__file__).resolve().parents[1]/"python"))+
                     ");sys.argv=['range_workflow.piezo_native_backend','--stdio'];"
                     "runpy.run_module('range_workflow.piezo_native_backend',run_name='__main__')")
        result = subprocess.run([sys.executable, "-B", "-c", bootstrap],
            input="\n".join(json.dumps(r) for r in requests)+"\n", text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r["ok"] for r in responses], [True, False, True])
        self.assertIn("stdout/stderr", responses[1]["error"])
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
