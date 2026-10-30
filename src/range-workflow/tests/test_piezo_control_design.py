"""Independent PC-07 numerical checks; no native FE/GUI delivery claim."""
from dataclasses import replace
import itertools
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))

import numpy as np
from numpy.testing import assert_allclose
from range_workflow.piezo_design import DesignError, SourceIdentity, StateSpace
from range_workflow.piezo_control_design import (
    constrained_mpc, discrete_lqr, lqr_action, mpc_proposal,
    observer_correction, observer_prediction, steady_state_kalman,
)


def system(a=((1.,),), b=((1.,),), c=((1.,),), d=None):
    a, b, c = [np.array(v, dtype=float) for v in (a, b, c)]
    d = np.zeros((len(c), b.shape[1])) if d is None else np.array(d, dtype=float)
    source = SourceIdentity('a'*64, tuple((f'q:{i}', 'm') for i in range(len(a))),
        tuple((f'voltage:{i}', 'V') for i in range(b.shape[1])),
        tuple((f'sensor:{i}', 'V') for i in range(len(c))))
    return StateSpace(a, b, c, d, source, 'b'*64, (), .1)


def scalar_dare(a, b, q, r):
    # Multiplying scalar DARE by r+b^2*p gives an explicit quadratic.
    linear = r*(1-a*a)-q*b*b
    return (-linear+np.sqrt(linear*linear+4*b*b*q*r))/(2*b*b)


def independent_two_step_qp(x, previous, lower, upper, delta, state_offsets=(0., 0.)):
    # A=B=Q=R=Qf=1, N=2. x0 cost is constant.
    # J=x0^2+u0^2+(x0+u0)^2+u1^2+(x0+u0+u1)^2.
    H = np.array([[6., 2.], [2., 4.]])
    h0, h1 = state_offsets
    free = np.array([x, x+h0, x+h0+h1])
    f = np.array([4*x+4*h0+2*h1, 2*x+2*h0+2*h1])
    G = np.array([[1, 0], [-1, 0], [0, 1], [0, -1],
                  [1, 0], [-1, 0], [-1, 1], [1, -1]], dtype=float)
    h = np.array([upper, -lower, upper, -lower, previous+delta,
                  delta-previous, delta, delta])
    candidates = []
    for count in range(3):
        for indices in itertools.combinations(range(len(h)), count):
            active = G[list(indices)]
            kkt = np.block([[H, active.T], [active, np.zeros((count, count))]])
            try:
                sol = np.linalg.solve(kkt, np.concatenate((-f, h[list(indices)])))
            except np.linalg.LinAlgError:
                continue
            u, multipliers = sol[:2], sol[2:]
            if np.max(G@u-h) <= 1e-10 and np.all(multipliers >= -1e-10):
                candidates.append((.5*u@H@u+f@u+free@free, u))
    return min(candidates, key=lambda item: item[0])


class PiezoControlDesignTest(unittest.TestCase):
    def test_mpc_varying_transitions_independent_affine_linear_solve(self):
        plant = system()
        d = constrained_mpc(plant, [[.7]], [[1.2]], [[2.3]], 2,
                            lower=[-10.], upper=[10.], delta=[10.])
        aa, bb, hh, x = np.array([.8, 1.1]), np.array([1.3, .6]), np.array([.4, -.2]), .7
        free = np.array([x, aa[0]*x+hh[0], aa[1]*(aa[0]*x+hh[0])+hh[1]])
        T = np.array([[0., 0.], [bb[0], 0.], [aa[1]*bb[0], bb[1]]])
        W = np.diag([.7, .7, 2.3])
        optimum = -np.linalg.solve(T.T@W@T+1.2*np.eye(2), T.T@W@free)
        result = mpc_proposal(d, plant, [x], [0.], state_offsets=hh[:, None],
                             state_transitions=aa[:, None, None], input_transitions=bb[:, None, None])
        assert_allclose(result.trajectory[:, 0], optimum, atol=1e-7)
        assert_allclose(result.predicted_states[:, 0], free+T@optimum, atol=1e-7)
        self.assertLess(result.certificate.action_stationarity, 1e-6)
        for args in ({'state_transitions': aa[:, None, None]},
                     {'state_transitions': [[[1.]]], 'input_transitions': bb[:, None, None]},
                     {'state_transitions': [[[True]], [[1.]]], 'input_transitions': bb[:, None, None]},
                     {'state_transitions': aa[:, None, None], 'input_transitions': [[[float('inf')]], [[1.]]]}):
            with self.assertRaises(DesignError):
                mpc_proposal(d, plant, [x], [0.], **args)

    def test_mpc_known_affine_scalar_analytic_and_purity(self):
        plant = system()
        d = constrained_mpc(plant, [[0.]], [[1.]], [[1.]], 1,
                            lower=[-10.], upper=[10.], delta=[10.])
        offsets = np.array([[2.]])
        result = mpc_proposal(d, plant, [0.], [0.], state_offsets=offsets)
        # Independent J(u)=u^2+(u+2)^2: dJ/du=4u+4.
        assert_allclose(result.first_action, [-1.], atol=1e-12)
        assert_allclose(result.predicted_states, [[0.], [1.]], atol=1e-12)
        self.assertAlmostEqual(result.objective, 2.)
        assert_allclose(offsets, [[2.]], atol=0)
        again = mpc_proposal(d, plant, [0.], [0.], state_offsets=offsets)
        assert_allclose(again.trajectory, result.trajectory, atol=0)

    def test_mpc_affine_multistep_independent_linear_solve(self):
        a, b, x, N = .8, 1.3, .7, 3
        plant = system(((a,),), ((b,),))
        q, r, qf = .7, 1.2, 2.3
        d = constrained_mpc(plant, [[q]], [[r]], [[qf]], N,
                            lower=[-10.], upper=[10.], delta=[10.])
        h = np.array([[.4], [-.2], [.1]])
        free = np.array([a**k*x+sum(a**(k-1-j)*h[j, 0] for j in range(k)) for k in range(N+1)])
        T = np.array([[b*a**(k-1-j) if j < k else 0. for j in range(N)] for k in range(N+1)])
        W = np.diag([q]*N+[qf])
        optimum = -np.linalg.solve(T.T@W@T+r*np.eye(N), T.T@W@free)
        result = mpc_proposal(d, plant, [x], [0.], state_offsets=h)
        assert_allclose(result.trajectory[:, 0], optimum, atol=2e-7)
        assert_allclose(result.predicted_states[:, 0], free+T@optimum, atol=2e-7)
        self.assertLess(result.certificate.action_stationarity, 1e-6)

    def test_mpc_affine_constrained_independent_active_set(self):
        plant = system()
        d = constrained_mpc(plant, [[1.]], [[1.]], [[1.]], 2,
                            lower=[-.5], upper=[.5], delta=[.2])
        for offsets in ((.8, -.2), (-.5, .9), (.2, .3)):
            expected_cost, expected_u = independent_two_step_qp(.3, .1, -.5, .5, .2, offsets)
            result = mpc_proposal(d, plant, [.3], [.1], state_offsets=np.array(offsets)[:, None])
            assert_allclose(result.trajectory[:, 0], expected_u, atol=1e-7)
            self.assertAlmostEqual(result.objective, expected_cost, places=8)
            assert_allclose(result.predicted_states[1:, 0], result.predicted_states[:-1, 0]+
                            result.trajectory[:, 0]+offsets, atol=1e-12)
            c = result.certificate
            self.assertLess(max(c.primal, c.stationarity, c.dual, c.complementarity, c.action_stationarity), 1e-6)

    def test_mpc_affine_bad_dimensions_nonfinite_and_legacy_zero(self):
        plant = system()
        d = constrained_mpc(plant, [[1.]], [[1.]], [[1.]], 2,
                            lower=[-1.], upper=[1.], delta=[.5])
        legacy = mpc_proposal(d, plant, [.3], [.1])
        zero = mpc_proposal(d, plant, [.3], [.1], state_offsets=np.zeros((2, 1)))
        assert_allclose(legacy.trajectory, zero.trajectory, atol=0)
        for value in ([[1.]], [[1., 2.], [3., 4.]], [[float('inf')], [0.]], [[complex(1, 2)], [0.]],
                      [[True], [0.]], [["2"], [0.]]):
            with self.assertRaises(DesignError):
                mpc_proposal(d, plant, [.3], [.1], state_offsets=value)

    def test_scalar_lqr_analytic_golden(self):
        plant = system(((1.2,),), ((.7,),))
        design = discrete_lqr(plant, [[2]], [[3]])
        p = scalar_dare(1.2, .7, 2, 3)
        gain = .7*p*1.2/(3+.49*p)
        assert_allclose(design.P, [[p]], rtol=1e-12)
        assert_allclose(design.K, [[gain]], rtol=1e-12)
        assert_allclose(lqr_action(design, plant, [4]), [-4*gain])
        self.assertLess(design.riccati_relative_residual, 1e-12)
        self.assertTrue(all(abs(pole) < 1 for pole in design.closed_poles))
        self.assertEqual(dict(design.identity.provenance)['numpyVersion'], '2.2.6')
        self.assertEqual(dict(design.identity.provenance)['scipyVersion'], '1.15.3')
        with self.assertRaises(ValueError):
            design.K.setflags(write=True)

    def test_known_independent_multi_actuator_lqr(self):
        plant = system(np.diag([1.1, .8]), np.diag([1., 2.]), np.eye(2))
        result = discrete_lqr(plant, np.diag([2., 3.]), np.diag([4., 5.]))
        expected = [scalar_dare(a, b, q, r) for a, b, q, r in
                    zip([1.1, .8], [1, 2], [2, 3], [4, 5])]
        assert_allclose(result.P, np.diag(expected), rtol=1e-12, atol=1e-14)

    def test_unstabilizable_and_cost_undetectable_rejected(self):
        for plant, q in [(system(((1.1,),), ((0.,),)), [[1]]),
                         (system(((1.1,),)), [[0]]),
                         (system(((1.,),), ((0.,),)), [[1]])]:
            with self.subTest(plant=plant), self.assertRaisesRegex(DesignError, 'PBH'):
                discrete_lqr(plant, q, [[1]])
        with self.assertRaises(DesignError):
            discrete_lqr(system(), [[-1]], [[1]])
        with self.assertRaises(DesignError):
            discrete_lqr(system(), [[1]], [[0]])

    def test_qp_independent_active_set_goldens_and_actual_kkt(self):
        plant = system()
        for x, previous, delta in [(2., .1, .2), (-1., -.2, .3), (.05, 0., .8)]:
            with self.subTest(x=x):
                design = constrained_mpc(plant, [[1]], [[1]], [[1]], 2,
                                         lower=[-.5], upper=[.5], delta=[delta])
                expected_cost, expected = independent_two_step_qp(x, previous, -.5, .5, delta)
                proposal = mpc_proposal(design, plant, [x], [previous])
                assert_allclose(proposal.trajectory[:, 0], expected, atol=2e-6)
                self.assertAlmostEqual(proposal.objective, expected_cost, places=8)
                certificate = proposal.certificate
                self.assertLess(max(certificate.primal, certificate.stationarity,
                                    certificate.dual, certificate.complementarity,
                                    certificate.action_stationarity), 1e-6)
                self.assertTrue(np.all(certificate.multipliers >= 0))
                assert_allclose(proposal.predicted_states[1:, 0],
                    proposal.predicted_states[:-1, 0]+proposal.trajectory[:, 0], atol=1e-12)

    def test_first_step_rate_warm_start_and_transaction_purity(self):
        plant = system()
        design = constrained_mpc(plant, [[1]], [[1]], [[1]], 3,
                                 lower=[-1], upper=[1], delta=[.1])
        warm, previous = np.array([[99.], [-99.], [99.]]), np.array([.5])
        saved_warm, saved_previous = warm.copy(), previous.copy()
        one = mpc_proposal(design, plant, [2], previous, warm_start=warm)
        two = mpc_proposal(design, plant, [2], previous, warm_start=warm)
        assert_allclose(one.trajectory, two.trajectory, atol=0)
        assert_allclose(warm, saved_warm, atol=0)
        assert_allclose(previous, saved_previous, atol=0)
        self.assertLessEqual(np.max(np.abs(np.diff(np.r_[previous, one.trajectory[:, 0]]))), .1+1e-9)
        assert_allclose(one.next_warm_start, np.vstack((one.trajectory[1:], one.trajectory[-1:])))
        with self.assertRaises(ValueError):
            one.first_action.setflags(write=True)
        with self.assertRaisesRegex(DesignError, 'previous_applied'):
            mpc_proposal(design, plant, [2], [1.1])

    def test_scalar_unconstrained_qp_one_step_golden(self):
        plant = system(((.8,),), ((2.,),))
        design = constrained_mpc(plant, [[1]], [[3]], [[4]], 1,
                                 lower=[-10], upper=[10], delta=[np.inf])
        proposal = mpc_proposal(design, plant, [2], [0])
        assert_allclose(proposal.first_action, [-2*4*.8*2/(3+4*4)], atol=1e-10)

    def test_tiny_objective_keeps_physically_resolved_action_and_certification(self):
        plant = system()
        design = constrained_mpc(plant, [[0]], [[1e-3]], [[1e-3]], 1,
                                 lower=[-20], upper=[20], delta=[20])
        # Independent optimum of .001*u^2+.001*(x+u)^2 is -x/2.
        # Its objective improvement is below SLSQP's absolute 1e-12 ftol,
        # while returning zero violates the frozen 1e-7 action certificate.
        warm = np.zeros((1, 1))
        for x in (1e-5, 1.1e-5):
            proposal = mpc_proposal(design, plant, [x], [0], warm_start=warm,
                                    verification_tol=1e-7)
            assert_allclose(proposal.first_action, [-x/2], rtol=1e-13, atol=0)
            self.assertLess(proposal.certificate.action_stationarity, 1e-15)
            assert_allclose(warm, [[0]], atol=0)
        from types import SimpleNamespace
        for result in (SimpleNamespace(success=False, message='injected failure'),
                       SimpleNamespace(success=True, x=np.zeros(1))):
            with patch('range_workflow.piezo_control_design.optimize.minimize', return_value=result):
                with self.assertRaises(DesignError):
                    mpc_proposal(design, plant, [1e-5], [0], warm_start=warm,
                                 verification_tol=1e-7)
            assert_allclose(warm, [[0]], atol=0)

    def test_high_contrast_qp_preserves_low_weight_action_direction(self):
        plant = system(np.eye(2), np.eye(2), np.eye(2))
        design = constrained_mpc(plant, np.zeros((2, 2)), np.eye(2),
            np.diag([1., 1e12]), 1, lower=[-2, -2], upper=[2, 2],
            delta=[np.inf, np.inf])
        # Independent scalar minimization: u0^2+(1+u0)^2 => u0=-1/2;
        # the second independent direction has zero initial state, optimum 0.
        proposal = mpc_proposal(design, plant, [1, 0], [0, 0])
        assert_allclose(proposal.first_action, [-.5, 0], atol=1e-10)
        self.assertAlmostEqual(proposal.objective, .5, places=12)
        self.assertLess(proposal.certificate.action_stationarity, 1e-10)
        from types import SimpleNamespace
        with patch('range_workflow.piezo_control_design.optimize.minimize',
                   return_value=SimpleNamespace(success=True, x=np.zeros(2))):
            with self.assertRaisesRegex(DesignError, 'KKT'):
                mpc_proposal(design, plant, [1, 0], [0, 0])

    def test_tiny_constrained_amplitude_and_slew_analytic_goldens(self):
        plant = system()
        for bound, previous, delta in [(1e-6, 0., 20.), (2e-6, 0., 20.),
                                       (1e-5, 0., 20.), (20., 0., 1e-6),
                                       (20., 1e-6, 2e-6)]:
            with self.subTest(bound=bound, previous=previous, delta=delta):
                design = constrained_mpc(plant, [[0]], [[1e-3]], [[1e-3]], 1,
                    lower=[-bound], upper=[bound], delta=[delta])
                # J=.001*u^2+.001*(x+u)^2; clip its exact -x/2
                # minimizer to the independent amplitude/rate interval.
                expected = max(-bound, previous-delta, min(bound, previous+delta, -5e-6))
                warm = np.zeros((1, 1))
                saved = warm.copy()
                proposal = mpc_proposal(design, plant, [1e-5], [previous],
                    warm_start=warm, verification_tol=1e-7)
                assert_allclose(proposal.first_action, [expected], rtol=1e-10, atol=1e-15)
                c = proposal.certificate
                self.assertLess(max(c.primal, c.stationarity, c.dual, c.complementarity,
                                    c.action_stationarity), 1e-7)
                if abs(expected) < 5e-6:
                    self.assertGreater(np.max(c.multipliers), 0)
                assert_allclose(warm, saved, atol=0)

    def test_tiny_constrained_multistep_independent_active_set_goldens(self):
        plant = system()
        for bound, previous, delta in [(1e-6, 0., 20.), (2e-6, 0., 20.),
                                       (20., 0., 1e-6), (20., 1e-6, 2e-6)]:
            with self.subTest(bound=bound, previous=previous, delta=delta):
                design = constrained_mpc(plant, [[1e-3]], [[1e-3]], [[1e-3]], 2,
                    lower=[-bound], upper=[bound], delta=[delta])
                expected_cost, expected = independent_two_step_qp(1e-5, previous, -bound, bound, delta)
                proposal = mpc_proposal(design, plant, [1e-5], [previous], verification_tol=1e-7)
                assert_allclose(proposal.trajectory[:, 0], expected, rtol=1e-9, atol=1e-15)
                assert_allclose(proposal.objective, expected_cost*1e-3, rtol=1e-12, atol=1e-25)
                c = proposal.certificate
                self.assertGreater(np.max(c.multipliers), 0)
                self.assertLess(max(c.primal, c.stationarity, c.dual, c.complementarity,
                                    c.action_stationarity), 1e-7)

    def test_scaled_and_unscaled_solver_faults_remain_atomic(self):
        from types import SimpleNamespace
        for tiny in (False, True):
            plant = system()
            weight, x, bound = (1e-3, 1e-5, 1e-6) if tiny else (1., 2., .5)
            design = constrained_mpc(plant, [[0]], [[weight]], [[weight]], 1,
                lower=[-bound], upper=[bound], delta=[20])
            warm = np.zeros((1, 1))
            for result in [SimpleNamespace(success=False, message='injected failure'),
                           SimpleNamespace(success=True, x=np.zeros(1)),
                           SimpleNamespace(success=True, x=np.full(1, 100.))]:
                with self.subTest(tiny=tiny, result=result):
                    with patch('range_workflow.piezo_control_design.optimize.minimize', return_value=result):
                        with self.assertRaises(DesignError):
                            mpc_proposal(design, plant, [x], [0], warm_start=warm, verification_tol=1e-7)
                    assert_allclose(warm, [[0]], atol=0)

    def test_zero_and_overflow_coordinate_scale_keep_original_coordinates(self):
        import range_workflow.piezo_control_design as module
        real_minimize = module.optimize.minimize
        plant = system()
        design = constrained_mpc(plant, [[0]], [[1e-3]], [[1e-3]], 1,
            lower=[-20], upper=[20], delta=[20])
        for x in (0., 1e-309):
            with self.subTest(x=x):
                with patch('range_workflow.piezo_control_design.optimize.minimize', wraps=real_minimize) as solver:
                    proposal = mpc_proposal(design, plant, [x], [0], verification_tol=1e-7)
                self.assertTrue(np.all(np.isfinite(proposal.first_action)))
                self.assertTrue(np.all(np.abs(solver.call_args.args[1]) < 1e-300))
        real_solve = module.linalg.solve
        for nonfinite in (np.nan, np.inf):
            calls = []
            def solve_with_nonfinite_seed(*args, **kwargs):
                calls.append(None)
                return np.array([nonfinite]) if len(calls) == 1 else real_solve(*args, **kwargs)
            with self.subTest(nonfinite=nonfinite):
                with patch('range_workflow.piezo_control_design.linalg.solve', side_effect=solve_with_nonfinite_seed), \
                     patch('range_workflow.piezo_control_design.optimize.minimize', wraps=real_minimize) as solver:
                    proposal = mpc_proposal(design, plant, [0], [0], verification_tol=1e-7)
                assert_allclose(proposal.first_action, [0], atol=0)
                assert_allclose(solver.call_args.kwargs['jac'](np.zeros(1)), [0], atol=0)

    def test_solver_failure_and_false_success_kkt_are_atomic(self):
        plant = system()
        design = constrained_mpc(plant, [[1]], [[1]], [[1]], 2,
                                 lower=[-1], upper=[1], delta=[.2])
        warm = np.zeros((2, 1))
        from types import SimpleNamespace
        for result in [SimpleNamespace(success=False, message='injected failure'),
                       SimpleNamespace(success=True, x=np.zeros(2)),
                       SimpleNamespace(success=True, x=np.ones(2)*100)]:
            with patch('range_workflow.piezo_control_design.optimize.minimize', return_value=result):
                with self.assertRaises(DesignError):
                    mpc_proposal(design, plant, [2], [0], warm_start=warm)
            assert_allclose(warm, np.zeros((2, 1)), atol=0)

    def test_invalid_configuration(self):
        plant = system()
        for horizon in [0, -1, True, 1.5]:
            with self.assertRaises(DesignError):
                constrained_mpc(plant, [[1]], [[1]], [[1]], horizon,
                                lower=[-1], upper=[1], delta=[1])
        for lower, upper, delta in [([-np.inf], [1], [1]), ([1], [1], [1]),
                                    ([-1], [1], [0]), ([-1], [1], [np.nan])]:
            with self.assertRaises(DesignError):
                constrained_mpc(plant, [[1]], [[1]], [[1]], 2,
                                lower=lower, upper=upper, delta=delta)
        with self.assertRaisesRegex(DesignError, 'discrete'):
            discrete_lqr(replace(plant, sampling_period=None), [[1]], [[1]])

    def test_sensor_only_observer_analytic_direct_term_purity(self):
        plant = system(((1.1,),), ((2.,),), ((.7,), (99.,)), ((3.,), (100.,)))
        design = steady_state_kalman(plant, [[2]], [[3]], selected_outputs=[0])
        p = scalar_dare(1.1, .7, 2, 3)
        gain = p*.7/(3+.49*p)
        assert_allclose(design.Pprior, [[p]], rtol=1e-12)
        assert_allclose(design.correction_gain, [[gain]], rtol=1e-12)
        prior, previous, measured = np.array([.2]), np.array([.4]), np.array([2.])
        proposal = observer_correction(design, plant, prior, measured, previous)
        innovation = 2-3*.4-.7*.2
        assert_allclose(proposal.innovation, [innovation])
        assert_allclose(proposal.corrected_state, [.2+gain*innovation])
        next_prior = observer_prediction(design, plant, proposal.corrected_state, [.3])
        assert_allclose(next_prior, 1.1*proposal.corrected_state+.6)
        assert_allclose(prior, [.2]); assert_allclose(previous, [.4]); assert_allclose(measured, [2.])
        again = observer_correction(design, plant, prior, measured, previous)
        assert_allclose(again.corrected_state, proposal.corrected_state, atol=0)
        self.assertLess(design.riccati_relative_residual, 1e-12)
        self.assertLess(abs(design.closed_poles[0]), 1)

    def test_unobserved_unstable_sensor_rejected(self):
        plant = system(np.diag([1.1, .8]), np.eye(2), [[0, 1]])
        with self.assertRaisesRegex(DesignError, 'detectability'):
            steady_state_kalman(plant, np.eye(2), [[1]], selected_outputs=[0])
        with self.assertRaisesRegex(DesignError, 'selected_outputs'):
            steady_state_kalman(system(), [[1]], [[1]], selected_outputs=[True])

    def test_material_negative_weight_direction_and_stale_backend_rejected(self):
        plant = system(np.diag([.8, .9]), np.eye(2), np.eye(2))
        bad = np.diag([-1., 1e12])
        with self.assertRaises(DesignError):
            discrete_lqr(plant, bad, np.eye(2))
        with self.assertRaises(DesignError):
            constrained_mpc(plant, np.eye(2), np.eye(2), bad, 2,
                            lower=[-1, -1], upper=[1, 1], delta=[1, 1])
        with self.assertRaises(DesignError):
            steady_state_kalman(plant, bad, np.eye(2), selected_outputs=[0, 1])
        lqr = discrete_lqr(system(), [[1]], [[1]])
        with patch('range_workflow.piezo_control_design.Path.read_bytes', return_value=b'changed'):
            with self.assertRaisesRegex(DesignError, 'source changed'):
                lqr_action(lqr, system(), [0])
        stale = replace(lqr, identity=replace(lqr.identity, provenance=()))
        with self.assertRaisesRegex(DesignError, 'provenance mismatch'):
            lqr_action(stale, system(), [0])

    def test_source_and_operator_mismatch_rejected(self):
        plant = system()
        lqr = discrete_lqr(plant, [[1]], [[1]])
        mpc = constrained_mpc(plant, [[1]], [[1]], [[1]], 2,
                              lower=[-1], upper=[1], delta=[1])
        observer = steady_state_kalman(plant, [[1]], [[1]], selected_outputs=[0])
        altered = [replace(plant, source=replace(plant.source, sha256='c'*64)),
                   replace(plant, A=np.array([[.9]])), replace(plant, sampling_period=.2),
                   replace(plant, matrix_sha256='c'*64)]
        for other in altered:
            with self.assertRaisesRegex(DesignError, 'identity mismatch'):
                lqr_action(lqr, other, [0])
            with self.assertRaisesRegex(DesignError, 'identity mismatch'):
                mpc_proposal(mpc, other, [0], [0])
            with self.assertRaisesRegex(DesignError, 'identity mismatch'):
                observer_correction(observer, other, [0], [0], [0])


if __name__ == '__main__':
    unittest.main()
