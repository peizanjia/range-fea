"""Independent numerical PC-04/05 checks; no native-FE integration claim."""
from dataclasses import replace
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))

import numpy as np
from numpy.testing import assert_allclose
from range_workflow.piezo_design import (
    DEPENDENCY_LOCK_SHA256, DesignError, SourceIdentity, compute_modes,
    continuous_state_space, exact_zoh, physical_damping, project_state,
    reduce_model, select_modes,
)


def source(n, nu=1, ny=1, digest='a'*64):
    return SourceIdentity(digest, tuple((f'node:{i}:x', 'm') for i in range(n)),
                          tuple((f'input:{i}', 'N') for i in range(nu)),
                          tuple((f'output:{i}', 'm') for i in range(ny)))


class PiezoDesignTest(unittest.TestCase):
    def test_unrelated_large_diagonal_cannot_hide_asymmetric_operators(self):
        bad = np.array([[1e12, 0, 0], [0, 1, 1], [0, -1, 1]])
        src = source(3)
        for operation in (
                lambda: compute_modes(bad, np.eye(3), source=src),
                lambda: compute_modes(np.eye(3), bad, source=src),
                lambda: physical_damping(np.eye(3), np.eye(3), source=src,
                                          kind='imported', imported_C=bad),
                lambda: continuous_state_space(np.eye(3), np.eye(3), bad,
                                                np.ones((3, 1)), source=src,
                                                Lq=np.ones((1, 3)))):
            with self.subTest(operation=operation), self.assertRaisesRegex(DesignError, 'symmetric'):
                operation()
        symmetric = np.diag([1e12, 1, 1])
        symmetric[1, 2] = symmetric[2, 1] = .25
        accepted = physical_damping(np.eye(3), np.eye(3), source=src,
                                    kind='imported', imported_C=symmetric)
        np.testing.assert_array_equal(accepted.C, symmetric)

    def test_two_mass_analytic_frequencies_residuals_and_signs(self):
        # Two masses m=2, springs k=6: roots k/m*(3 +/- sqrt(5))/2.
        m, k = 2*np.eye(2), 6*np.array([[2, -1], [-1, 1]])
        modes = compute_modes(m, k, source=source(2))
        expected = 1.5*np.array([3-np.sqrt(5), 3+np.sqrt(5)])
        assert_allclose(modes.eigenvalues, expected, rtol=1e-13)
        assert_allclose(modes.omega, np.sqrt(expected), rtol=1e-13)
        assert_allclose(modes.Phi.T@m@modes.Phi, np.eye(2), atol=1e-14)
        self.assertLess(max(modes.residuals), 1e-14)
        self.assertLess(modes.mass_orthogonality_error, 1e-14)
        for j in range(2):
            self.assertGreater(modes.Phi[np.argmax(abs(modes.Phi[:, j])), j], 0)

    def test_repeated_and_near_repeated_cluster_selection(self):
        modes = compute_modes(np.eye(4), np.diag([0, 4, 4+1e-8, 9]), source=source(4))
        self.assertEqual(modes.clusters, ((0,), (1, 2), (3,)))
        with self.assertRaisesRegex(DesignError, 'cluster'):
            select_modes(modes, [1])
        subset = select_modes(modes, [2, 1])
        self.assertEqual(subset.indices, (1, 2))
        self.assertEqual(subset.clusters, ((1, 2),))
        exact = compute_modes(np.eye(3), np.diag([4, 4, 9]), source=source(3))
        assert_allclose(exact.Phi[:, :2]@exact.Phi[:, :2].T, np.diag([1, 1, 0]))
        for indices in ([], [0, 0], [-1], [4], [True], [0.0]):
            with self.subTest(indices=indices), self.assertRaises(DesignError):
                select_modes(modes, indices)

    def test_complete_modal_C_has_requested_ratios(self):
        m, k, src = np.diag([2, 3, 4]), np.diag([8, 12, 36]), source(3)
        modes = compute_modes(m, k, source=src)
        damping = physical_damping(m, k, source=src, modes=modes,
                                   kind='per_cluster', zeta=[0.02, 0.07])
        assert_allclose(modes.Phi.T@damping.C@modes.Phi,
                        np.diag([0.08, 0.08, 0.42]), atol=1e-14)
        assert_allclose(np.linalg.eigvalsh(damping.C), [0.16, 0.24, 1.68])
        self.assertEqual(damping.strategy, 'complete-mass-normalized-basis')
        uniform = physical_damping(m, k, source=src, modes=modes, kind='uniform', zeta=0.1)
        assert_allclose(modes.Phi.T@uniform.C@modes.Phi, np.diag([0.4, 0.4, 0.6]), atol=1e-14)
        with self.assertRaisesRegex(DesignError, 'single damping ratio'):
            physical_damping(m, k, source=src, modes=modes, kind='per_mode', zeta=[.02, .03, .07])

    def test_partial_C_requires_explicit_complement_policy(self):
        m, k, src = np.eye(2), np.diag([4, 9]), source(2)
        modes = select_modes(compute_modes(m, k, source=src), [0])
        with self.assertRaisesRegex(DesignError, 'high_mode_policy'):
            physical_damping(m, k, source=src, modes=modes, kind='uniform', zeta=.1)
        damping = physical_damping(m, k, source=src, modes=modes, kind='uniform', zeta=.1,
                                   high_mode_policy='zero_damping')
        assert_allclose(damping.C, np.diag([.4, 0]))
        self.assertEqual(damping.strategy, 'partial-basis-zero-damping-complement')

    def test_none_rayleigh_and_imported_full_C(self):
        m, k, src = np.diag([2, 3]), np.diag([8, 27]), source(2)
        assert_allclose(physical_damping(m, k, source=src).C, np.zeros((2, 2)))
        damping = physical_damping(m, k, source=src, kind='rayleigh', alpha=.2, beta=.01)
        assert_allclose(damping.C, np.diag([.48, .87]))
        imported = [[1, -1], [-1, 1]]  # Rank-deficient C is valid PSD.
        assert_allclose(physical_damping(m, k, source=src, kind='imported', imported_C=imported).C, imported)
        for c in ([[1, 2], [2, 1]], [[1, 0], [1, 1]], [[float('nan'), 0], [0, 1]]):
            with self.subTest(c=c), self.assertRaises(DesignError):
                physical_damping(m, k, source=src, kind='imported', imported_C=c)
        for kwargs in (dict(kind='none', zeta=.1), dict(kind='rayleigh', alpha=-1, beta=0),
                       dict(kind='imported', imported_C=imported, beta=.1),
                       dict(kind='none', high_mode_policy='zero_damping')):
            with self.subTest(kwargs=kwargs), self.assertRaises(DesignError):
                physical_damping(m, k, source=src, **kwargs)

    def test_reject_malformed_physical_matrices(self):
        src = source(2)
        for m, k in (([[1, 0], [0, 0]], np.eye(2)),
                     (np.diag([1, -1]), np.eye(2)),
                     ([[1, 1], [0, 1]], np.eye(2)),
                     (np.eye(2), np.diag([1, -1])),
                     (np.eye(2), [[1, 1], [0, 1]]),
                     (np.eye(2), [[1, 0], [0, float('inf')]]),
                     (np.eye(2), np.eye(3)),
                     (np.eye(2, dtype=complex), np.eye(2)),
                     ([], [])):
            with self.subTest(m=m, k=k), self.assertRaises(DesignError):
                compute_modes(m, k, source=src)
        modes = compute_modes(np.eye(2), np.zeros((2, 2)), source=src)
        assert_allclose(modes.omega, [0, 0])
        self.assertEqual(modes.clusters, ((0, 1),))

    def test_high_contrast_negative_stiffness_and_damping_are_rejected(self):
        m, src = np.eye(2), source(2)
        # A global norm tolerance would wrongly accept these negative directions.
        for k in (np.diag([-1., 1e12]), np.array([[1e-12, 2], [2, 1e12]]),
                  np.array([[0., 1e-12], [1e-12, 1e12]])):
            with self.subTest(k=k), self.assertRaisesRegex(DesignError, 'positive semidefinite'):
                compute_modes(m, k, source=src)
            with self.assertRaises(DesignError):
                continuous_state_space(m, k, np.zeros((2, 2)), [[1], [0]], source=src, Lq=[[1, 0]])
        k = np.diag([1., 1e12])
        for c in (np.diag([-1., 1e12]), np.array([[1e-12, 2], [2, 1e12]]),
                  np.array([[0., 1e-12], [1e-12, 1e12]])):
            with self.subTest(c=c), self.assertRaisesRegex(DesignError, 'positive semidefinite'):
                physical_damping(m, k, source=src, kind='imported', imported_C=c)
            with self.assertRaises(DesignError):
                continuous_state_space(m, k, c, [[1], [0]], source=src, Lq=[[1, 0]])

    def test_positive_high_contrast_and_true_rigid_modes_preserve_FE_matrices(self):
        m, src = np.eye(3), source(3)
        for k in (np.diag([0., 1e-12, 1e12]), np.diag([0., 1., 1e12])):
            modes = compute_modes(m, k, source=src)
            assert_allclose(modes.eigenvalues, np.diag(k), rtol=1e-14, atol=0)
            self.assertEqual(modes.clusters, ((0,), (1,), (2,)))

            self.assertEqual(modes.numerical_zero_corrections, ())
            c = np.diag([0., 1e-12, 1e12])
            damping = physical_damping(m, k, source=src, kind='imported', imported_C=c)
            assert_allclose(damping.C, c, rtol=0, atol=0)
            ss = continuous_state_space(m, k, damping, [[1], [0], [0]], source=src, Lq=[[1, 0, 0]])
            assert_allclose(ss.A[3:, :3], -k, rtol=0, atol=0)
            assert_allclose(ss.A[3:, 3:], -c, rtol=0, atol=0)
        # Nonzero rigid coupling is valid when its diagonal congruence is PSD.
        coupled = np.array([[1e-12, 1.], [1., 1e12]])
        coupled_modes = compute_modes(np.eye(2), coupled, source=source(2))
        self.assertLess(abs(coupled_modes.eigenvalues[0]), 1e-25)

    def test_finite_extreme_stiffness_does_not_overflow_cluster_certificate(self):
        for k in (np.diag([1., 1e160]), np.array([[1e160, .9e80], [.9e80, 1.]])):
            with self.subTest(k=k):
                modes = compute_modes(np.eye(2), k, source=source(2))
                self.assertEqual(modes.clusters, ((0,), (1,)))
                self.assertTrue(np.all(np.isfinite(modes.residuals)))
                selected = select_modes(modes, [0])
                self.assertEqual(selected.indices, (0,))

    def test_negative_numerical_root_requires_directional_zero_certificate(self):
        import range_workflow.piezo_design as module
        # Controlled eigensolver roundoff: an exact zero direction can be certified.
        with patch.object(module.linalg, 'eigh', return_value=(np.array([-1e-15, 1e12]), np.eye(2))):
            modes = compute_modes(np.eye(2), np.diag([0., 1e12]), source=source(2))
        self.assertEqual(modes.numerical_zero_corrections, ((0, -1e-15, 0., 0.),))
        # A spurious negative root for an actual positive low mode is never clipped.
        with patch.object(module.linalg, 'eigh', return_value=(np.array([-1., 1e12]), np.eye(2))):
            with self.assertRaisesRegex(DesignError, 'qualified numerical rigid zero'):
                compute_modes(np.eye(2), np.diag([1., 1e12]), source=source(2))

    def test_FE_acceleration_and_electrical_direct_terms(self):
        # u columns are force and voltage; electrode Q=-G^T q+Ce V.
        src = source(1, 2, 3)
        ss = continuous_state_space([[2]], [[8]], [[.4]], [[1, -3]], source=src,
             Lq=[[0], [-3], [0]], Lv=[[0], [0], [1]], La=[[1], [0], [0]],
             Dinput=[[0, 0], [0, 5], [0, 0]])
        assert_allclose(ss.A, [[0, 1], [-4, -.2]])
        assert_allclose(ss.B, [[0, 0], [.5, -1.5]])
        assert_allclose(ss.Cy, [[-4, -.2], [-3, 0], [0, 1]])
        assert_allclose(ss.Dy, [[.5, -1.5], [0, 5], [0, 0]])
        x, u = np.array([.7, -.8]), np.array([2., 4.])
        acceleration = (2-3*4-.4*x[1]-8*x[0])/2
        assert_allclose(ss.Cy@x+ss.Dy@u, [acceleration, -3*x[0]+5*4, x[1]])
        discrete = exact_zoh(ss, .1)
        assert_allclose(discrete.Dy, ss.Dy)
        assert_allclose(discrete.Cy, ss.Cy)

    def test_exact_zoh_singular_double_integrator(self):
        ss = continuous_state_space([[2]], [[0]], [[0]], [[3]], source=source(1), Lq=[[1]])
        discrete = exact_zoh(ss, .2)
        assert_allclose(discrete.A, [[1, .2], [0, 1]], atol=1e-15)
        assert_allclose(discrete.B, [[.03], [.3]], atol=1e-15)
        self.assertEqual(discrete.sampling_period, .2)
        with self.assertRaises(DesignError):
            exact_zoh(discrete, .2)

    def test_exact_zoh_analytic_oscillator(self):
        omega, ts = 3., .17
        ss = continuous_state_space([[2]], [[18]], [[0]], [[4]], source=source(1), Lq=[[1]])
        discrete = exact_zoh(ss, ts)
        c, s = np.cos(omega*ts), np.sin(omega*ts)
        assert_allclose(discrete.A, [[c, s/omega], [-omega*s, c]], atol=2e-15)
        assert_allclose(discrete.B, [[2*(1-c)/omega**2], [2*s/omega]], atol=2e-15)
        for bad in (0, -1, float('nan'), float('inf'), True):
            with self.subTest(ts=bad), self.assertRaises(DesignError):
                exact_zoh(ss, bad)

    def test_two_mode_damped_zoh_and_acceleration_direct_terms(self):
        # Two independently solvable damped modes give a closed-form oracle for
        # the coupled four-state/multi-input matrix exponential. The production
        # path must preserve state order [q1, q2, v1, v2] and the direct term.
        masses = np.array([2., 3.])
        stiffness = np.array([8., 27.])
        damping = np.array([.4, .9])
        inputs = np.array([[1., 2.], [3., -1.]])
        la = np.array([[.2, .3]])
        direct = np.array([[.7, -.2]])
        ss = continuous_state_space(np.diag(masses), np.diag(stiffness),
                                    np.diag(damping), inputs, source=source(2, 2),
                                    Lq=[[1., -.5]], La=la, Dinput=direct)
        ts = .17
        discrete = exact_zoh(ss, ts)
        expected_a = np.zeros((4, 4))
        expected_b = np.zeros((4, 2))
        for i in range(2):
            omega2 = stiffness[i] / masses[i]
            decay = damping[i] / (2*masses[i])
            damped = np.sqrt(omega2-decay*decay)
            factor = np.exp(-decay*ts)
            cosine, sine = np.cos(damped*ts), np.sin(damped*ts)
            qq = factor*(cosine+decay*sine/damped)
            qv = factor*sine/damped
            vq = -omega2*qv
            vv = factor*(cosine-decay*sine/damped)
            expected_a[i, i] = qq
            expected_a[i, i+2] = qv
            expected_a[i+2, i] = vq
            expected_a[i+2, i+2] = vv
            # A constant force has equilibrium displacement B/k and zero
            # velocity; evolve its deviation with the homogeneous solution.
            expected_b[i, :] = (1-qq)*inputs[i, :]/stiffness[i]
            expected_b[i+2, :] = -vq*inputs[i, :]/stiffness[i]
        assert_allclose(discrete.A, expected_a, rtol=2e-14, atol=2e-15)
        assert_allclose(discrete.B, expected_b, rtol=2e-13, atol=2e-15)
        expected_direct = direct+la@np.diag(1/masses)@inputs
        assert_allclose(expected_direct, [[1.1, -.1]], atol=1e-15)
        assert_allclose(ss.Dy, expected_direct, atol=1e-15)
        assert_allclose(discrete.Dy, expected_direct, atol=1e-15)
        x0, u = np.array([.1, -.2, .3, -.4]), np.array([2., -1.])
        assert_allclose(discrete.A@x0+discrete.B@u,
                        expected_a@x0+expected_b@u, rtol=2e-13, atol=2e-15)

    def test_full_FE_ROM_equivalence_and_nonzero_q0_mass_projection(self):
        m = np.array([[3, .4], [.4, 2]])
        k = np.array([[10, -2], [-2, 7]])
        c = np.array([[.8, .1], [.1, .6]])
        b = np.array([[1, -.2], [.3, .7]])
        lq, lv, la, d = np.array([[1, 2]]), np.array([[.2, -.3]]), np.array([[.4, .1]]), np.array([[.5, .9]])
        src = source(2, 2)
        modes = compute_modes(m, k, source=src)
        full = continuous_state_space(m, k, c, b, source=src, Lq=lq, Lv=lv, La=la, Dinput=d)
        rom = reduce_model(m, k, c, b, modes, source=src, Lq=lq, Lv=lv, La=la, Dinput=d)
        for operator in (rom.M, rom.K, rom.C):
            np.testing.assert_array_equal(operator, operator.T)
        p = modes.Phi
        transform = np.block([[p, np.zeros_like(p)], [np.zeros_like(p), p]])
        assert_allclose(full.A@transform, transform@rom.state_space.A, atol=1e-14)
        assert_allclose(full.B, transform@rom.state_space.B, atol=1e-14)
        assert_allclose(full.Cy@transform, rom.state_space.Cy, atol=1e-14)
        assert_allclose(full.Dy, rom.state_space.Dy, atol=1e-14)
        q0, q, v = np.array([4, -3]), np.array([4.2, -2.7]), np.array([.5, -.2])
        projected = project_state(m, k, modes, q, v, q0, source=src)
        assert_allclose(transform@projected, np.concatenate((q-q0, v)), atol=1e-14)
        fe_d, rom_d = exact_zoh(full, .03), exact_zoh(rom.state_space, .03)
        assert_allclose(fe_d.A@transform, transform@rom_d.A, atol=1e-14)
        assert_allclose(fe_d.B, transform@rom_d.B, atol=1e-14)
        # A truncated projection is M-weighted, and every operator uses that Phi.
        selected = select_modes(modes, [0])
        reduced = reduce_model(m, k, c, b, selected, source=src, Lq=lq, Lv=lv, La=la, Dinput=d)
        for operator in (reduced.M, reduced.K, reduced.C):
            np.testing.assert_array_equal(operator, operator.T)
        assert_allclose(reduced.C, selected.Phi.T@c@selected.Phi)
        assert_allclose(reduced.Bplant, selected.Phi.T@b)
        assert_allclose(project_state(m, k, selected, q, v, q0, source=src),
                        np.concatenate((selected.Phi.T@m@(q-q0), selected.Phi.T@m@v)))

    def test_congruence_mirrors_only_roundoff_scale_antisymmetry(self):
        import range_workflow.piezo_design as module

        operator = np.array([[0.011112163891790957]])
        basis = np.array([[-0.93046804470820466, -0.029251822463273489]])
        raw = basis.T@operator@basis
        self.assertNotEqual(raw[0, 1], raw[1, 0])
        projected = module._symmetric_congruence(operator, basis, 'A')
        np.testing.assert_array_equal(projected, projected.T)
        np.testing.assert_array_equal(projected, np.triu(raw)+np.triu(raw, 1).T)

    def test_congruence_rejects_invalid_operator_and_over_bound_residual(self):
        import range_workflow.piezo_design as module

        with self.assertRaisesRegex(DesignError, 'symmetric'):
            module._symmetric_congruence([[1., 0.], [.1, 1.]], np.eye(2), 'A')

        original_matmul = np.matmul
        calls = 0

        def inject_over_bound_residual(left, right):
            nonlocal calls
            calls += 1
            result = original_matmul(left, right)
            if calls == 2:
                result = result.copy()
                result[0, 1] += 1e-6
            return result

        with patch.object(module.np, 'matmul', side_effect=inject_over_bound_residual):
            with self.assertRaisesRegex(DesignError, 'exceeds its floating-point bound'):
                module._symmetric_congruence(np.eye(2), np.eye(2), 'A')

    def test_source_invalidation_and_mapping_failures(self):
        m, k, src = np.eye(2), np.diag([4, 9]), source(2)
        modes = compute_modes(m, k, source=src)
        for changed_m, changed_k, changed_source in ((2*m, k, src), (m, 2*k, src), (m, k, source(2, digest='b'*64))):
            with self.subTest(source=changed_source), self.assertRaisesRegex(DesignError, 'identity mismatch'):
                physical_damping(changed_m, changed_k, source=changed_source, modes=modes, kind='uniform', zeta=.1)
        with self.assertRaises(DesignError):
            compute_modes(m, k, source=source(1))
        with self.assertRaises(DesignError):
            continuous_state_space(m, k, np.zeros_like(m), np.ones((2, 2)), source=src, Lq=[[1, 0]])
        with self.assertRaises(DesignError):
            continuous_state_space(m, k, np.zeros_like(m), [[1], [0]], source=source(2, ny=2), Lq=[[1, 0]])
        damping = physical_damping(m, k, source=src)
        with self.assertRaises(DesignError):
            continuous_state_space(m, 2*k, damping, [[1], [0]], source=src, Lq=[[1, 0]])
        for digest in ('x'*64, 'A'*64, 'a'*63):
            with self.assertRaises(DesignError):
                source(2, digest=digest)
        with self.assertRaises(DesignError):
            SourceIdentity('a'*64, (('q', 'm'), ('q', 'm')))

    def test_output_shape_and_nonfinite_rejection(self):
        base = dict(source=source(1), Lq=[[1]])
        for override in (dict(Lv=[[1, 2]]), dict(La=[[float('nan')]]),
                         dict(Dinput=[[1, 2]]), dict(Lq=[1])):
            with self.subTest(override=override), self.assertRaises(DesignError):
                continuous_state_space([[1]], [[1]], [[0]], [[1]], **(base|override))
        ss = continuous_state_space([[1]], [[1]], [[0]], [[1]], **base)
        with self.assertRaises(DesignError):
            exact_zoh(replace(ss, B=np.zeros((3, 1))), .1)

    def test_immutable_results_and_actual_dependency_identity(self):
        modes = compute_modes([[1]], [[4]], source=source(1))
        with self.assertRaises(ValueError):
            modes.Phi[0, 0] = 2
        with self.assertRaises(ValueError):
            modes.Phi.setflags(write=True)
        metadata = dict(modes.provenance)
        self.assertEqual(metadata['numpyVersion'], np.__version__)
        self.assertEqual(metadata['dependencyLockSha256'], DEPENDENCY_LOCK_SHA256)
        import scipy
        self.assertEqual(metadata['scipyVersion'], scipy.__version__)
        import hashlib
        import range_workflow.piezo_design as module
        self.assertEqual(metadata['moduleCodeSha256'], hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest())
        self.assertEqual(metadata['moduleIdentityScope'], 'this-module-source-and-executing-code-only')
        self.assertRegex(metadata['moduleExecutionCodeSha256'], r'^[0-9a-f]{64}$')

    def test_loaded_module_refuses_changed_or_missing_disk_source(self):
        import range_workflow.piezo_design as module
        loaded = module.provenance()
        modes = compute_modes([[1]], [[4]], source=source(1))
        ss = continuous_state_space([[1]], [[4]], [[0]], [[1]], source=source(1), Lq=[[1]])
        original = Path(module.__file__).read_bytes()
        with patch.object(Path, 'read_bytes', return_value=original+b'\n# changed after import\n'):
            for operation in (module.provenance,
                              lambda: compute_modes([[1]], [[4]], source=source(1)),
                              lambda: select_modes(modes, [0]),
                              lambda: physical_damping([[1]], [[4]], source=source(1)),
                              lambda: project_state([[1]], [[4]], modes, [0], [0], [0], source=source(1)),
                              lambda: exact_zoh(ss, .1)):
                with self.subTest(operation=operation), self.assertRaisesRegex(DesignError, 'changed since import'):
                    operation()
        with patch.object(Path, 'read_bytes', side_effect=OSError('missing source')):
            with self.assertRaisesRegex(DesignError, 'source is unavailable'):
                module.provenance()
        self.assertEqual(module.provenance(), loaded)


if __name__ == '__main__':
    unittest.main()
