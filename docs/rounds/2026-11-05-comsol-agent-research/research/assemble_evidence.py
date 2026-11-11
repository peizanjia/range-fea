"""Rebuild the research indexes from source-specific, bounded evidence notes."""
from pathlib import Path
import hashlib
import json
from datetime import datetime, timezone
from urllib.parse import urlsplit, urlunsplit

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_json(name, obj):
    (HERE / name).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n")

def write_rows(name, rows):
    (HERE / name).write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows))

raw = json.loads((HERE / "research_agent.json").read_text())["sources"]
raw += [
    dict(id="C16", title="Error Estimation — Theory and Variables", version="6.4",
         url="https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_solver.36.015.html",
         date=None, locator="Theory Background / Accuracy; 本次根工作单元正文核查 L4–79",
         facts=["DWR针对目标泛函引入对偶问题，并在富集有限元空间计算弱残差。",
                "该空间误差估计有适定性、正则性和对偶近似条件，不包含提前终止求解的代数误差。"]),
    dict(id="C17", title="Piezoelectric Shear-Actuated Beam", version="6.4",
         url="https://doc.comsol.com/6.4/doc/com.comsol.help.models.mems.shear_bender/shear_bender.html",
         date=None, locator="Model Definition / Notes About the COMSOL Implementation; 本次根工作单元正文核查 L8–44",
         facts=["官方算例为特定PZT-5H夹层梁的静力剪切驱动，明确几何、电极、极化旋转和材料。",
                "该算例不是本项目PZT-5A多杆受迫控制模型的同一基准。"]),
]
sources, evidence, claims = [], [], []
for display, src in enumerate(raw, 1):
    parts = urlsplit(src["url"])
    canonical = urlunsplit((parts.scheme.lower(), parts.netloc.lower(), parts.path.rstrip("/"), parts.query, ""))
    sid = hashlib.sha256(canonical.encode()).hexdigest()[:16]
    sources.append(dict(source_id=sid, evidence_label=src["id"], display_number=display,
                        raw_url=src["url"], canonical_locator=canonical,
                        title=src["title"], organization="COMSOL", source_type="official_documentation_or_announcement",
                        version=src["version"], publication_date=src.get("date"),
                        accessed_at="2026-11-05", locator=src["locator"],
                        independence_group="COMSOL_vendor", fulltext_retained=False))
    for n, fact in enumerate(src["facts"], 1):
        eid = f"{src['id']}-E{n:02d}"
        evidence.append(dict(evidence_id=eid, source_id=sid, locator=src["locator"],
                             observation=fact, type="official_public_statement", extraction="paraphrase"))
        claims.append(dict(claim_id=f"{src['id']}-K{n:02d}", text=fact, evidence_ids=[eid],
                           status="source_supported", scope=src["version"],
                           limitation="Public description, not independent installed-product testing."))

local_paths = {
    "R01": HERE / "range_agent_gap.json",
    "R02": ROOT / "docs/audit_20261104/RESUME_REVIEW_20261105.md",
    "R03": ROOT / "docs/audit_20261104/RUNTIME_VALIDATION_20261105.md",
    "R04": ROOT / "docs/PIEZO_CONTROL_DELIVERY.md",
    "R05": ROOT / "docs/acceptance/phase-checkpoint-20261030.md",
}
for label, path in local_paths.items():
    sha = digest(path)
    sources.append(dict(source_id=f"local-{label}-{sha[:16]}", evidence_label=label,
                        path=str(path.relative_to(ROOT)), sha256=sha,
                        source_type="current_repository_record", accessed_at="2026-11-05"))

gap = json.loads((HERE / "range_agent_gap.json").read_text())
# Preserve the exact agent-owned schema as a linked evidence object, not an inferred PASS.
evidence.append(dict(evidence_id="R01-static-audit", source_id=sources[-5]["source_id"],
                     type="static_source_audit", artifact="range_agent_gap.json",
                     limitation="Implementation observed; current runtime not certified."))
claims += [
    dict(claim_id="R-G05", text="Core exposes piezo.configure-experiment, Agent command enum does not.",
         evidence_ids=["R01-static-audit"], status="current_static", locator="range_agent_gap.md G05"),
    dict(claim_id="R-G08", text="Trusted verification supports native-passive rod/beam, not a general coupled piezo verifier.",
         evidence_ids=["R01-static-audit"], status="current_static", locator="range_agent_gap.md G08"),
    dict(claim_id="R-G09", text="Automatic mesh proposal responds to required reference displacement/frequency failures, not arbitrary algebraic residual failure.",
         evidence_ids=["R01-static-audit"], status="current_static", locator="range_agent_gap.md G09"),
    dict(claim_id="R-G10", text="Adjacent QoI convergence does not certify absolute true error.",
         evidence_ids=["R01-static-audit", "C16-E02"], status="source_based_analysis"),
    dict(claim_id="DESIGN-D0-D6", text="Seven-layer architecture and D0–D6 are proposed Range development tasks.",
         evidence_ids=[], status="design_proposal_not_product_fact"),
]
if (HERE / "piezo_validation.json").exists():
    path = HERE / "piezo_validation.json"
    sources.append(dict(source_id="local-R06-" + digest(path)[:16], evidence_label="R06",
                        path=str(path.relative_to(ROOT)), sha256=digest(path), source_type="current_targeted_execution"))
    evidence.append(dict(evidence_id="R06-execution", source_id=sources[-1]["source_id"],
                         artifact="piezo_validation.json", type="targeted_run_receipts",
                         limitation="Read exact cases and layer; no inherited CLI/GUI/LLM/installer acceptance."))
write_rows("sources.jsonl", sources)
write_rows("evidence.jsonl", evidence)
write_rows("claims.jsonl", claims)
manifest = json.loads((HERE / "run_manifest.json").read_text())
manifest.update(provider_config={"primary": "web.run", "fallback_reason": "search CLI unavailable", "scholarly": None},
                source_count_official=len(raw), source_count_local=len(sources)-len(raw),
                evidence_count=len(evidence), claim_count=len(claims),
                external_scope="Official COMSOL public descriptions only; no COMSOL installation tested",
                repository_root=str(ROOT),
                uncertainty=["2027 release and full MCP contract not verified", "No comparative performance study", "Current full GUI/Provider/install chain not certified"],
                artifact_paths={"sources":"sources.jsonl","evidence":"evidence.jsonl","claims":"claims.jsonl",
                                "report":"report.md","handoff":"developer_handoff.json"})
write_json("run_manifest.json", manifest)
print(json.dumps({"official_sources":len(raw),"local_sources":len(sources)-len(raw),"evidence":len(evidence),"claims":len(claims)}))
