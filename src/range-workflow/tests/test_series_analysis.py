"""Analytic checks for the bounded WF10 pure time-history functions."""
from __future__ import annotations

import copy
import math
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.series_analysis import (  # noqa: E402
    MAX_SOURCE_SAMPLES,
    METHOD_VERSION,
    SeriesAnalysisError,
    compare_series,
    envelope_samples,
    phase_plot,
    spectrum,
    validate_series,
)


# Frozen analytic tolerance: relative 1e-10, absolute 1e-12.
REL_TOL = 1.0e-10
ABS_TOL = 1.0e-12


def assert_analytic(test: unittest.TestCase, actual: float, expected: float) -> None:
    test.assertTrue(math.isclose(actual, expected, rel_tol=REL_TOL, abs_tol=ABS_TOL),
                    f"actual={actual!r}, expected={expected!r}")


def make_series(values, *, sample_rate=64.0, unit="m", axis="X", variable="displacement",
                channel_id="node:101:displacement:X", source_kind="native-active-fe",
                model_sha="a" * 64, source_hash="b" * 64, completed=True,
                start_record=1, origins=False):
    count = len(values)
    samples = []
    for index, value in enumerate(values):
        sample = {"time": index / sample_rate, "value": value,
                  "recordNo": start_record + index}
        if origins and index == 0:
            sample["origin"] = {"originKind": "current-job", "sourceRecord": 1}
        samples.append(sample)
    return {"sourceKind": source_kind, "jobId": "job-20261009-01",
            "artifactId": "artifact/job-20261009-01/active/0",
            "modelSha256": model_sha, "channelId": channel_id, "axis": axis,
            "variable": variable, "unit": unit, "timeUnit": "s",
            "completed": completed, "sourceHash": source_hash, "samples": samples}


class SeriesAnalysisTest(unittest.TestCase):
    def test_integer_bin_sine_amplitude_and_window_power_parseval(self):
        count, sample_rate, amplitude, bin_index = 64, 64.0, 3.25, 5
        values = [amplitude * math.cos(2.0 * math.pi * bin_index * index / count)
                  for index in range(count)]
        source = make_series(values, sample_rate=sample_rate)
        for window in ("rectangular", "hann"):
            with self.subTest(window=window):
                result = spectrum(source, window=window)
                self.assertEqual(result["methodVersion"], METHOD_VERSION)
                self.assertEqual(result["units"]["frequency"], "Hz")
                self.assertEqual(result["units"]["amplitude"], "m")
                self.assertEqual(result["units"]["psd"],
                                 {"numeratorUnit": "m", "power": 2, "denominatorUnit": "Hz"})
                self.assertEqual(result["window"], window)
                assert_analytic(self, result["frequenciesHz"][bin_index], 5.0)
                assert_analytic(self, result["amplitude"][bin_index], amplitude)
                # For this integer-bin cosine, both rectangular and periodic
                # Hann weighting have a window-power-normalized mean square A^2/2.
                df = result["frequenciesHz"][1] - result["frequenciesHz"][0]
                assert_analytic(self, math.fsum(result["psd"]) * df, amplitude ** 2 / 2.0)
                self.assertEqual(result["provenance"]["source"]["sourceHash"], "b" * 64)

    def test_odd_length_last_real_fft_bin_is_doubled(self):
        count, sample_rate, amplitude, bin_index = 5, 5.0, 2.0, 2
        values = [amplitude * math.cos(2.0 * math.pi * bin_index * index / count)
                  for index in range(count)]
        result = spectrum(make_series(values, sample_rate=sample_rate),
                          window="rectangular")
        self.assertEqual(len(result["frequenciesHz"]), (count + 1) // 2)
        assert_analytic(self, result["amplitude"][-1], amplitude)
        df = result["frequenciesHz"][1] - result["frequenciesHz"][0]
        assert_analytic(self, math.fsum(result["psd"]) * df, amplitude ** 2 / 2.0)

    def test_dc_and_even_length_nyquist_are_not_doubled(self):
        count = 64
        constant = spectrum(make_series([3.0] * count), window="rectangular", remove_mean=False)
        assert_analytic(self, constant["amplitude"][0], 3.0)
        self.assertTrue(all(abs(value) <= ABS_TOL for value in constant["amplitude"][1:]))
        assert_analytic(self, constant["psd"][0], 9.0)

        nyquist_source = make_series([2.0 * (-1.0) ** index for index in range(count)])
        nyquist = spectrum(nyquist_source, window="rectangular", remove_mean=False)
        nyquist_index = count // 2
        assert_analytic(self, nyquist["amplitude"][nyquist_index], 2.0)
        assert_analytic(self, nyquist["psd"][nyquist_index], 4.0)
        self.assertEqual(len(nyquist["frequenciesHz"]), count // 2 + 1)
        self.assertTrue(all(abs(value) <= ABS_TOL for value in nyquist["amplitude"][1:nyquist_index]))

        demeaned_dc = spectrum(make_series([3.0] * count), window="rectangular")
        self.assertTrue(all(abs(value) <= ABS_TOL for value in demeaned_dc["amplitude"]))

    def test_psd_scaling_preserves_representable_values_at_extreme_scales(self):
        large_values = [1.0e100, -1.0e100, 1.0e100, -1.0e100]
        large_input = make_series(large_values, sample_rate=1.0e308)
        large_psd = spectrum(large_input, window="rectangular", remove_mean=False)["psd"][-1]
        self.assertTrue(math.isclose(large_psd, 4.0e-108, rel_tol=REL_TOL, abs_tol=0.0),
                        f"large-input Nyquist PSD lost scale: {large_psd!r}")

        small_values = [1.0e-200, -1.0e-200, 1.0e-200, -1.0e-200]
        small_input = make_series(small_values, sample_rate=1.0e-99)
        small_psd = spectrum(small_input, window="rectangular", remove_mean=False)["psd"][-1]
        self.assertTrue(math.isclose(small_psd, 4.0e-301, rel_tol=REL_TOL, abs_tol=0.0),
                        f"small-input Nyquist PSD underflowed: {small_psd!r}")

        unrepresentable_psd = make_series(small_values, sample_rate=1.0e308)
        with self.assertRaisesRegex(SeriesAnalysisError, "PSD underflows"):
            spectrum(unrepresentable_psd, window="rectangular", remove_mean=False)

        unrepresentable_rate = make_series([1.0, -1.0, 1.0, -1.0])
        for index, sample in enumerate(unrepresentable_rate["samples"]):
            sample["time"] = index * 1.0e-320
        with self.assertRaisesRegex(SeriesAnalysisError, "sampling rate"):
            spectrum(unrepresentable_rate, window="rectangular", remove_mean=False)

    def test_dto_rejects_irregular_nonfinite_oversized_and_untrusted_shapes(self):
        valid = make_series([0.0, 1.0, 0.0, -1.0])
        malformed = []
        nonfinite = copy.deepcopy(valid)
        nonfinite["samples"][2]["value"] = float("nan")
        malformed.append((nonfinite, "finite"))
        boolean_number = copy.deepcopy(valid)
        boolean_number["samples"][0]["value"] = True
        malformed.append((boolean_number, "finite number"))
        wrong_units = copy.deepcopy(valid)
        wrong_units["timeUnit"] = "ms"
        malformed.append((wrong_units, "timeUnit"))
        extra_file_input = {**valid, "path": "C:/not-consumed"}
        malformed.append((extra_file_input, "unsupported fields"))
        untrusted_source_kind = copy.deepcopy(valid)
        untrusted_source_kind["sourceKind"] = "anything-the-caller-claims"
        malformed.append((untrusted_source_kind, "sourceKind"))
        missing_hash = copy.deepcopy(valid)
        missing_hash["sourceHash"] = "not-a-sha256"
        malformed.append((missing_hash, "SHA-256"))
        oversized = copy.deepcopy(valid)
        oversized["samples"] *= MAX_SOURCE_SAMPLES // len(oversized["samples"]) + 1
        malformed.append((oversized, "actual records"))
        for dto, message in malformed:
            with self.subTest(message=message):
                with self.assertRaisesRegex(SeriesAnalysisError, message):
                    validate_series(dto)

    def test_nonuniform_input_is_never_interpolated(self):
        source = make_series([0.0, 1.0, 0.0, -1.0])
        source["samples"][2]["time"] += 1.0e-8
        with self.assertRaisesRegex(SeriesAnalysisError, "interpolation is not performed"):
            spectrum(source)

        within_tolerance = make_series([0.0, 1.0, 0.0, -1.0], sample_rate=1.0)
        within_tolerance["samples"][2]["time"] += 5.0e-10
        result = spectrum(within_tolerance, window="rectangular")
        assert_analytic(self, result["sampleIntervalS"], 1.0)

    def test_comparison_requires_same_provenance_and_physical_channel_and_grid(self):
        left = make_series([4.0, 3.0, 2.0, 1.0], unit="m", source_hash="b" * 64)
        right = make_series([1.0, 1.0, 1.0, 1.0], unit="m", source_kind="native-passive-fe",
                            source_hash="c" * 64, start_record=101)
        result = compare_series(left, right)
        self.assertEqual([sample["value"] for sample in result["samples"]], [3.0, 2.0, 1.0, 0.0])
        self.assertEqual(result["units"], {"time": "s", "value": "m"})
        self.assertEqual(result["samples"][0]["leftRecordNo"], 1)
        self.assertEqual(result["samples"][0]["rightRecordNo"], 101)
        self.assertEqual(result["provenance"]["left"]["sourceHash"], "b" * 64)
        self.assertEqual(result["provenance"]["right"]["sourceHash"], "c" * 64)

        incompatible = [
            ("modelSha256", "d" * 64),
            ("channelId", "node:202:displacement:X"),
            ("axis", "Y"),
            ("variable", "velocity"),
            ("unit", "m/s"),
        ]
        for field, value in incompatible:
            changed = copy.deepcopy(right)
            changed[field] = value
            with self.subTest(field=field):
                with self.assertRaises(SeriesAnalysisError):
                    compare_series(left, changed)
        changed_grid = copy.deepcopy(right)
        changed_grid["samples"][2]["time"] += 0.001
        with self.assertRaisesRegex(SeriesAnalysisError, "same actual time grid"):
            compare_series(left, changed_grid)

    def test_phase_plot_keeps_distinct_axis_units_and_checks_model_time(self):
        x_source = make_series([0.0, 1.0, 0.0, -1.0], unit="m", axis="X")
        y_source = make_series([2.0, 0.0, -2.0, 0.0], unit="N", axis="Y",
                               channel_id="node:101:force:Y", variable="force", start_record=201)
        result = phase_plot(x_source, y_source)
        self.assertEqual(result["units"], {"time": "s", "x": "m", "y": "N"})
        self.assertEqual(result["samples"][1], {"time": 1.0 / 64.0, "xRecordNo": 2,
                                                 "yRecordNo": 202, "x": 1.0, "y": 0.0})
        changed_model = copy.deepcopy(y_source)
        changed_model["modelSha256"] = "e" * 64
        with self.assertRaisesRegex(SeriesAnalysisError, "different model"):
            phase_plot(x_source, changed_model)
        changed_time = copy.deepcopy(y_source)
        changed_time["samples"][1]["time"] += 0.002
        with self.assertRaisesRegex(SeriesAnalysisError, "same actual time grid"):
            phase_plot(x_source, changed_time)

    def test_envelope_keeps_global_endpoints_and_bucket_edge_and_interior_spikes(self):
        count, limit = 10_000, 1_000
        values = [0.0] * count
        bucket_count = (limit - 2) // 4
        interior_count = count - 2
        edge_bucket = 50
        edge_index = (edge_bucket * interior_count) // bucket_count + 1
        interior_bucket = 150
        interior_start = (interior_bucket * interior_count) // bucket_count + 1
        interior_end = ((interior_bucket + 1) * interior_count) // bucket_count
        interior_index = interior_start + (interior_end - interior_start) // 2
        values[edge_index] = 123.0
        values[interior_index] = -456.0
        source = make_series(values, sample_rate=10.0, origins=True)

        result = envelope_samples(source, limit=limit)
        selected_records = [sample["recordNo"] for sample in result["samples"]]
        selected_values = {sample["value"] for sample in result["samples"]}
        self.assertLessEqual(len(result["samples"]), limit)
        self.assertEqual(len(selected_records), len(set(selected_records)))
        self.assertEqual(selected_records, sorted(selected_records))
        self.assertEqual(selected_records[0], 1)
        self.assertEqual(selected_records[-1], count)
        self.assertIn(123.0, selected_values)
        self.assertIn(-456.0, selected_values)
        self.assertEqual(result["sourceSampleCount"], count)
        self.assertTrue(result["coverageComplete"])
        self.assertTrue(result["displayOnly"])
        self.assertEqual(result["provenance"]["sourceHash"], "b" * 64)
        self.assertEqual(result["samples"][0]["origin"], {"originKind": "current-job", "sourceRecord": 1})

    def test_partial_history_is_displayable_but_not_analyzable_and_limit_is_bounded(self):
        partial = make_series([1.0, 2.0, 3.0], sample_rate=2.0, completed=False)
        envelope = envelope_samples(partial, limit=8)
        self.assertFalse(envelope["coverageComplete"])
        self.assertTrue(envelope["displayOnly"])
        self.assertEqual(envelope["samples"], partial["samples"])
        with self.assertRaisesRegex(SeriesAnalysisError, "completed history"):
            spectrum(partial)
        with self.assertRaisesRegex(SeriesAnalysisError, "completed history"):
            compare_series(make_series([1.0, 2.0, 3.0, 4.0]),
                           make_series([1.0, 2.0, 3.0], completed=False))
        for limit in (7, True, 8.0):
            with self.subTest(limit=limit):
                with self.assertRaisesRegex(SeriesAnalysisError, "at least 8"):
                    envelope_samples(partial, limit=limit)
        with self.assertRaisesRegex(SeriesAnalysisError, "actual records"):
            envelope_samples(make_series(range(MAX_SOURCE_SAMPLES + 1)), limit=1000)

    def test_10000_small_sample_origins_fit_the_shared_metadata_budget(self):
        source = make_series([0.0] * MAX_SOURCE_SAMPLES)
        for index, sample in enumerate(source["samples"]):
            sample["origin"] = {"originKind": "current-job", "sourceSeq": index}
        normalized = validate_series(source)
        self.assertEqual(len(normalized["samples"]), MAX_SOURCE_SAMPLES)
        self.assertEqual(normalized["samples"][-1]["origin"]["sourceSeq"], MAX_SOURCE_SAMPLES - 1)

    def test_origin_text_limit_is_aggregate_and_preflighted_before_copy(self):
        payload = "x" * 4096
        source = make_series([0.0] * 1024)
        for sample in source["samples"]:
            sample["origin"] = {"payload": payload}
        with mock.patch("range_workflow.series_analysis._copy_metadata") as metadata_copy:
            with self.assertRaisesRegex(SeriesAnalysisError, "aggregate 4 MiB"):
                validate_series(source)
            metadata_copy.assert_not_called()

        # Both groups fit alone, while their shared source-plus-sample total
        # exceeds 4 MiB. This guards against resetting the allowance per origin.
        sample_origins = make_series([0.0] * 1000)
        for sample in sample_origins["samples"]:
            sample["origin"] = {"payload": payload}
        validate_series(sample_origins)
        source_origin = make_series([0.0] * 4)
        source_origin["origin"] = {f"p{index}": payload for index in range(24)}
        validate_series(source_origin)

        combined = copy.deepcopy(sample_origins)
        combined["origin"] = source_origin["origin"]
        with mock.patch("range_workflow.series_analysis._copy_metadata") as metadata_copy:
            with self.assertRaisesRegex(SeriesAnalysisError, "aggregate 4 MiB"):
                validate_series(combined)
            metadata_copy.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
