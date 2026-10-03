"""Explicit supported FE templates and their physical input conventions."""

TEMPLATES = {
    "four-rod": {"file": "stage9_truss4.ram", "sensor": "tip-velocity-x", "actuator": "tip-force-x",
                 "modes": 2, "boundary": 3, "dofs": [(node, "UX") for node in range(1, 5)]},
    "cantilever-beam": {"file": "stage10_beam.ram", "sensor": "tip-velocity-y", "actuator": "tip-force-y",
                        "modes": 1, "boundary": 1, "dofs": [(1, "UY"), (1, "RZ")]},
}
