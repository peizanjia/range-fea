"""WF-11 analytic scientific boundary tests, independent of native/build tests."""
import copy
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import importlib.util
import marshal
import struct

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from range_workflow.scientific_contract import (
    INPUT_SCHEMA, ALGORITHM, CONTRACT_HASH, ScientificContractError,
    strict_json, validate_input, validate_output, verification_profile)
from range_workflow.scientific_reference import compare_results, filter_history, reference_result, verify_result


def series(values, *, fs=32.0, window="rectangular", remove_mean=False, filt=None):
    return {"schema": INPUT_SCHEMA, "algorithm": ALGORITHM, "domain": "time", "scalarType": "real64",
            "times": [i/fs for i in range(len(values))], "timeUnit": "s",
            "samplePeriodS": 1/fs, "samplingRateHz": fs,
            "channels": [{"id": "displacement-x", "quantity": "displacement", "axis": "x", "unit": "m", "values": values}],
            "window": window, "removeMean": remove_mean, "filter": filt or {"kind": "none"}}


def lowpass(cutoff=3.0):
    return {"kind": "butterworth-lowpass", "order": 1, "cutoffHz": cutoff,
            "phase": "causal", "initialCondition": "zero"}


class ScientificContractTests(unittest.TestCase):
    def test_oracle_executes_hashed_sources_and_ignores_forged_bytecode(self):
        package = Path(__file__).parents[1]/"python"/"range_workflow"
        data = series([3.]*32)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("scientific_oracle_child.py", "scientific_contract.py", "scientific_reference.py"):
                (root/name).write_bytes((package/name).read_bytes())
            (root/"__init__.py").write_text("raise RuntimeError('unexpected package execution')", encoding="utf-8")
            reference = root/"scientific_reference.py"
            cache = root/"__pycache__"; cache.mkdir()
            forged = compile("raise RuntimeError('forged cached oracle')", str(reference), "exec")
            header = importlib.util.MAGIC_NUMBER + struct.pack("<III", 0, int(reference.stat().st_mtime), reference.stat().st_size)
            (cache/f"scientific_reference.{sys.implementation.cache_tag}.pyc").write_bytes(header+marshal.dumps(forged))
            (root/"input.json").write_text(json.dumps(data), encoding="utf-8")
            completed = subprocess.run([sys.executable, "-I", "-B", str(root/"scientific_oracle_child.py"),
                "--input", str(root/"input.json"), "--output", str(root/"output.json")], capture_output=True, timeout=20)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            verify_result(strict_json((root/"output.json").read_bytes()), data)
    def test_dc_and_mean_removal(self):
        data = series([3.0]*32)
        out = reference_result(data, small_n=True)
        self.assertAlmostEqual(out["channels"][0]["amplitude"][0], 3)
        self.assertEqual(out["channels"][0]["fft"][0], [96.0, 0.0])
        data["removeMean"] = True
        self.assertEqual(reference_result(data, small_n=True)["channels"][0]["amplitude"], [0.0]*17)

    def test_sine_amplitude_and_phase(self):
        for n in (31, 32):
            phase, amplitude, k = 0.37, 2.3, 3
            data = series([amplitude*math.sin(2*math.pi*k*j/n+phase) for j in range(n)])
            c = reference_result(data, small_n=True)["channels"][0]
            self.assertAlmostEqual(c["amplitude"][k], amplitude, places=11)
            self.assertAlmostEqual(c["fft"][k][0], n*amplitude*math.sin(phase)/2, places=10)
            self.assertAlmostEqual(c["fft"][k][1], -n*amplitude*math.cos(phase)/2, places=10)

    def test_odd_even_last_bin_and_nyquist(self):
        for n in (31, 32):
            k = n//2
            out = reference_result(series([4*math.cos(2*math.pi*k*j/n) for j in range(n)]), small_n=True)
            self.assertEqual(out["binCount"], n//2+1)
            self.assertAlmostEqual(out["frequenciesHz"][-1], k*32/n)
            self.assertAlmostEqual(out["channels"][0]["amplitude"][-1], 4, places=10)
            if n%2 == 0:
                self.assertEqual(out["channels"][0]["fft"][-1][1], 0)

    def test_hann_parseval_and_coherent_gain(self):
        for n in (2, 31, 32):
            values = [1.3 + math.sin(2*math.pi*4*j/n) for j in range(n)]
            data = series(values, window="hann")
            out = reference_result(data, small_n=True)
            windows = [0.5-0.5*math.cos(2*math.pi*j/n) for j in range(n)]
            expected = math.fsum((x*w)**2 for x,w in zip(values,windows))/math.fsum(w*w for w in windows)
            self.assertAlmostEqual(sum(out["channels"][0]["psd"])*out["binSpacingHz"], expected, places=10)
        data = series([2*math.sin(2*math.pi*4*j/32) for j in range(32)], window="hann")
        self.assertAlmostEqual(reference_result(data,small_n=True)["channels"][0]["amplitude"][4], 2, places=10)

    def test_filter_impulse_and_step_analytic(self):
        fs, cutoff = 32, 3
        k = math.tan(math.pi*cutoff/fs)
        b, pole = k/(1+k), (1-k)/(1+k)
        impulse = filter_history([1.0]+[0.0]*31, fs, lowpass())
        self.assertAlmostEqual(impulse[0], b)
        for j in range(1,32):
            self.assertAlmostEqual(impulse[j], b*(1+pole)*pole**(j-1))
        step = filter_history([1.0]*32, fs, lowpass())
        for j in range(32):
            self.assertAlmostEqual(step[j], 1-(1-b)*pole**j)
        from scipy.signal import butter, lfilter
        a,bcoeff = butter(1,cutoff,fs=fs)
        actual = lfilter(a,bcoeff,[1.0]+[0.0]*31)
        for x,y in zip(actual,impulse):
            self.assertAlmostEqual(x,y,places=13)

    def test_scalable_numpy_reference_matches_small_dft(self):
        for n in (31, 32, 128):
            data = series([math.sin(0.23*j)+j/20 for j in range(n)], window="hann", remove_mean=True, filt=lowpass())
            actual = reference_result(data)
            self.assertEqual(verify_result(actual,data), verification_profile())
        data = series([math.sin(j/50) for j in range(10000)])
        data["channels"] = [dict(data["channels"][0], id="channel-"+str(j)) for j in range(4)]
        self.assertEqual(reference_result(data)["sampleCount"],10000)
        with self.assertRaises(ScientificContractError):
            reference_result(data,small_n=True)

    def test_large_dc_and_small_sine_roundoff_bounds(self):
        for window in ("rectangular", "hann"):
            for remove_mean in (False, True):
                for sine in (0.0, 2.0):
                    data = series([1e10+sine*math.sin(2*math.pi*3*j/32) for j in range(32)],
                                  window=window, remove_mean=remove_mean)
                    actual, expected = reference_result(data), reference_result(data, small_n=True)
                    comparison = compare_results(actual, expected, data)
                    self.assertTrue(math.isfinite(comparison["maximumAbsoluteError"]))
                    self.assertEqual(verify_result(actual, data), verification_profile())
                    if sine:
                        corrupt = copy.deepcopy(actual)
                        corrupt["channels"][0]["fft"][3][1] *= -1
                        validate_output(corrupt, data)  # Same magnitude; wrong phase.
                        with self.assertRaises(ScientificContractError):
                            verify_result(corrupt, data)
        expected = reference_result(series([1e10]*32), small_n=True)
        self.assertEqual(expected["channels"][0]["fft"][1:], [[0.0, 0.0]]*16)
        actual = copy.deepcopy(expected)
        c = actual["channels"][0]
        c["fft"][0][0] *= 2
        c["amplitude"][0] *= 2
        c["psd"][0] *= 4
        with self.assertRaises(ScientificContractError):
            verify_result(actual, series([1e10]*32))

    def test_derived_finite_range_and_overflow_rejection(self):
        data = series([1e50]*32, fs=1.0)
        actual = reference_result(data)
        self.assertGreater(actual["channels"][0]["psd"][0], 1e101)
        decoded = strict_json(json.dumps(actual, allow_nan=False).encode())
        self.assertEqual(verify_result(decoded, data), verification_profile())
        self.assertEqual(strict_json(b'{"derived":1e101}')["derived"], 1e101)
        with self.assertRaises(ScientificContractError):
            validate_input(series([1e101]*32))
        for nonfinite in (math.inf, math.nan):
            corrupt = copy.deepcopy(actual)
            corrupt["channels"][0]["psd"][0] = nonfinite
            with self.assertRaises(ScientificContractError):
                validate_output(corrupt, data)
        corrupt = copy.deepcopy(actual)
        corrupt["channels"][0]["fft"][0][0] = 1e308
        with self.assertRaises(ScientificContractError):
            validate_output(corrupt, data)

    def test_input_rejections(self):
        data = series([1.0]*32)
        mutations = [lambda d:d.update(unknown=1), lambda d:d.update(algorithm="unknown"),
                     lambda d:d.update(samplePeriodS=True), lambda d:d.update(samplingRateHz=64),
                     lambda d:d["times"].__setitem__(4,0.123), lambda d:d.update(timeUnit="ms"),
                     lambda d:d["channels"][0].update(unit="N"), lambda d:d["channels"][0].update(quantity="mystery"),
                     lambda d:d["channels"][0]["values"].__setitem__(2,True),
                     lambda d:d["channels"][0]["values"].__setitem__(2,float("inf")),
                     lambda d:d.update(channels=d["channels"]*5),
                     lambda d:d.update(filter=lowpass(16)), lambda d:d.update(filter={**lowpass(),"order":True}),
                     lambda d:d.update(filter={**lowpass(),"initialCondition":"steady"})]
        for mutate in mutations:
            corrupted = copy.deepcopy(data)
            mutate(corrupted)
            with self.subTest(corrupted=corrupted), self.assertRaises(ScientificContractError):
                validate_input(corrupted)
        with self.assertRaises(ScientificContractError):
            validate_input(series([0]*10001))

    def test_duplicate_nonfinite_and_oversize_json(self):
        for raw in (b'{"a":1,"a":2}',b'{"a":NaN}',b'{"a":Infinity}', b'{"a":1e999}',b'\xff',b' '*(32*1024*1024+1)):
            with self.assertRaises(ScientificContractError):
                strict_json(raw)

    def test_output_corruption_and_consistent_wrong_fft(self):
        data = series([math.sin(2*math.pi*3*j/32) for j in range(32)])
        original = reference_result(data)
        mutations = [lambda d:d.update(unknown=1), lambda d:d.update(contractHash="0"*64),
                     lambda d:d.update(binCount=True),lambda d:d.update(binSpacingHz=2),
                     lambda d:d["frequenciesHz"].__setitem__(-1,15),
                     lambda d:d["channels"][0].update(amplitudeUnit="N"),
                     lambda d:d["channels"][0]["psdUnit"].update(power=2.0),
                     lambda d:d["channels"][0]["fft"][0].__setitem__(1,1),
                     lambda d:d["channels"][0]["amplitude"].__setitem__(3,4),
                     lambda d:d["channels"][0]["filteredValues"].__setitem__(4,True)]
        for mutate in mutations:
            corrupt = copy.deepcopy(original)
            mutate(corrupt)
            with self.assertRaises(ScientificContractError):
                validate_output(corrupt,data)
        corrupt = copy.deepcopy(original)
        c = corrupt["channels"][0]
        c["fft"][3] = [v*2 for v in c["fft"][3]]
        c["amplitude"][3] *= 2
        c["psd"][3] *= 4
        validate_output(corrupt,data)
        with self.assertRaises(ScientificContractError):
            verify_result(corrupt,data)
        corrupt = copy.deepcopy(original)
        corrupt["channels"][0]["filteredValues"][0] = 7
        with self.assertRaises(ScientificContractError):
            verify_result(corrupt,data)

    def test_child_isolated_fixed_file_protocol(self):
        child = Path(__file__).parents[1]/"python"/"range_workflow"/"extension_child_v2.py"
        # A minimal generated candidate is copied from the host once as a fixture;
        # independent numerical candidate validation is covered above.
        data = series([3.0]*32)
        expected = reference_result(data)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, inp, out = root/"candidate.py",root/"input.json",root/"output.json"
            source.write_text("def process(data):\n    return " + repr(expected) + "\n",encoding="utf-8")
            inp.write_text(json.dumps(data),encoding="utf-8")
            args = [sys.executable,"-I","-B",str(child),"--source",str(source),"--input",str(inp),"--output",str(out),"--run"]
            result = subprocess.run(args,capture_output=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)
            verify_result(strict_json(out.read_bytes()),data)
            inp.write_bytes(b'{"duplicate":1,"duplicate":2}')
            out.unlink()
            result = subprocess.run(args,capture_output=True,timeout=20)
            self.assertEqual(result.returncode,2)
            self.assertFalse(out.exists())
            self.assertNotIn(b"duplicate",result.stderr)

    def test_private_child_four_distinct_10000_channels_json_roundtrip(self):
        package = Path(__file__).parents[1]/"python"/"range_workflow"
        data = series([math.sin(j/50) for j in range(10000)], fs=100.0,
                      window="hann", remove_mean=True, filt=lowpass(7.0))
        specifications = [("displacement", "x", "m"), ("velocity", "y", "m/s"),
                          ("force", "z", "N"), ("energy", "scalar", "J")]
        data["channels"] = [{"id": "channel-"+str(k), "quantity": q, "axis": axis, "unit": unit,
                             "values": [0.3*k+(k+1)*math.sin((k+1)*j/50)+0.2*math.cos(j/13)
                                        for j in range(10000)]}
                            for k,(q,axis,unit) in enumerate(specifications)]
        data = validate_input(data)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, inp, out = root/"candidate.py", root/"input.json", root/"output.json"
            shutil.copyfile(package/"scientific_example.py", source)
            encoded = json.dumps(data, allow_nan=False, separators=(",", ":")).encode()
            inp.write_bytes(encoded)
            args = [sys.executable, "-I", "-B", str(package/"extension_child_v2.py"),
                    "--source", str(source), "--input", str(inp), "--output", str(out), "--run"]
            result = subprocess.run(args, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(inp.read_bytes(), encoded)
            actual = strict_json(out.read_bytes())
            self.assertEqual(actual["sampleCount"], 10000)
            self.assertEqual(len(actual["channels"]), 4)
            self.assertEqual(verify_result(actual, data), verification_profile())
            # Candidate mutates the supplied history before computing its hash.
            with source.open("a", encoding="utf-8") as stream:
                stream.write("\n_original_process = process\ndef process(data):\n"
                             "    data['channels'][0]['values'][0] += 1.0\n"
                             "    return _original_process(data)\n")
            out.unlink()
            result = subprocess.run(args, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(out.exists())
            self.assertEqual(inp.read_bytes(), encoded)

    def test_private_candidate_large_dc_small_sine_and_finite_psd(self):
        package = Path(__file__).parents[1]/"python"/"range_workflow"
        cases = [{"id": window+"-"+str(remove_mean),
                  "input": series([1e10+2*math.sin(2*math.pi*3*j/32) for j in range(32)],
                                  window=window, remove_mean=remove_mean)}
                 for window in ("rectangular", "hann") for remove_mean in (False, True)]
        cases.extend([{"id": "dc", "input": series([1e10]*32)},
                      {"id": "finite-psd", "input": series([1e50]*32, fs=1.0)}])
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, inp, out = root/"candidate.py", root/"input.json", root/"output.json"
            shutil.copyfile(package/"scientific_example.py", source)
            inp.write_text(json.dumps({"cases": cases}, allow_nan=False), encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", "-B", str(package/"extension_child_v2.py"),
                                     "--source", str(source), "--input", str(inp),
                                     "--output", str(out), "--test"], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            results = strict_json(out.read_bytes())["results"]
            self.assertEqual(len(results), len(cases))
            for case, record in zip(cases, results):
                self.assertEqual(record["id"], case["id"])
                actual, data = record["output"], case["input"]
                self.assertEqual(verify_result(actual, data), verification_profile())
                if case["id"] not in ("dc", "finite-psd"):
                    wrong_sign = copy.deepcopy(actual)
                    wrong_sign["channels"][0]["fft"][3][1] *= -1
                    with self.assertRaises(ScientificContractError):
                        verify_result(wrong_sign, data)
                wrong_scale = copy.deepcopy(actual)
                channel = wrong_scale["channels"][0]
                channel["fft"] = [[2*x, 2*y] for x,y in channel["fft"]]
                channel["amplitude"] = [2*x for x in channel["amplitude"]]
                channel["psd"] = [4*x for x in channel["psd"]]
                with self.assertRaises(ScientificContractError):
                    verify_result(wrong_scale, data)


if __name__ == "__main__":
    unittest.main()
