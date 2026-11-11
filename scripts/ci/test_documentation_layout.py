"""Source organization must not change the installed documentation namespace."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = {
    'WORKFLOW_CONTRACT.md', 'CONTROL_WORKFLOW.md', 'WORKFLOW_SDK.md', 'NATIVE_DOCUMENT_SDK.md',
    'NATIVE_RESULTS.md', 'RECOVERY_OPERATIONS.md', 'RECOVERY_CONTRACT.md', 'CONVERSATIONS.md',
    'TASK_PLANS.md', 'GUI_INTERACTION_DESIGN.md', 'GUI_UX_ACCEPTANCE.md', 'EXTENSION_SDK.md',
    'STAGE12_ACCEPTANCE.md', 'AGENT_OPERATIONS.md', 'PIEZO_CONTROL_DELIVERY.md',
    'PIEZO_DEPENDENCY_PROVENANCE.md', 'piezo_control_plan.json', 'APPLICATION_SERVICE.md',
    'PROJECT_CATALOG.md', 'NATIVE_MODEL_OPERATIONS.md', 'evidence/piezo_dependency_provenance.json',
    'evidence/piezo_delivery_performance_contract.json', 'evidence/piezo_representative_cases_spec.json',
    'CONTROLLER_ADAPTERS.md', 'TYPED_FE_CONTROL.md', 'STAGE9_CONTROL_ANALYSIS.md', 'TECHNICAL_ROUTE_STAGE9.md',
}

class DocumentationLayoutTests(unittest.TestCase):
    def pairs(self):
        text = (ROOT / 'src/PrepareDocumentation.cmake').read_text()
        return re.findall(r'"(docs/[^"|]+)\|([^"|]+)"', text)

    def test_same_allowlist_unique_sources_and_existing_files(self):
        pairs = self.pairs()
        self.assertEqual({dest for _, dest in pairs}, EXPECTED)
        self.assertEqual(len(pairs), len(EXPECTED))
        self.assertEqual(len({src for src, _ in pairs}), len(pairs))
        for src, _ in pairs:
            self.assertTrue((ROOT / src).is_file(), src)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake required for documentation staging')
    def test_staged_documents_keep_names_bytes_and_packaged_crosslinks(self):
        with tempfile.TemporaryDirectory(prefix='range-doc-layout-') as temporary:
            output = Path(temporary) / 'doc output with spaces'
            result = subprocess.run(['cmake', f'-DRANGE_DOCUMENTATION_SOURCE_ROOT={ROOT}',
                f'-DRANGE_INSTALL_DOCUMENTATION_ROOT={output}', '-P', str(ROOT / 'src/PrepareDocumentation.cmake')],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual({str(p.relative_to(output)).replace('\\', '/') for p in output.rglob('*') if p.is_file()}, EXPECTED)
            pairs = self.pairs()
            source_dest = {(ROOT / src).resolve(): output / dest for src, dest in pairs}
            for src, dest in pairs:
                source = ROOT / src
                staged = output / dest
                if source.suffix == '.json':
                    self.assertEqual(staged.read_bytes(), source.read_bytes(), src)
                    continue
                original_links = re.findall(r'\]\(([^)]+)\)', source.read_text())
                installed_links = re.findall(r'\]\(([^)]+)\)', staged.read_text())
                self.assertEqual(len(original_links), len(installed_links), src)
                for original, installed in zip(original_links, installed_links):
                    if re.match(r'^[a-zA-Z][\w+.-]*:', original) or original.startswith('#'):
                        self.assertEqual(installed, original)
                        continue
                    original_path = (source.parent / original.split('#')[0]).resolve()
                    if original_path in source_dest:
                        self.assertEqual((staged.parent / installed.split('#')[0]).resolve(),
                                         source_dest[original_path].resolve(), (src, original, installed))
                        self.assertTrue((staged.parent / installed.split('#')[0]).is_file())

if __name__ == '__main__':
    unittest.main()
