"""Static guards for PC-11 profile-upgrade measurement routing."""
from pathlib import Path
import re
import unittest


SCRIPT = Path(__file__).with_name("pc11_gui_uia.ps1").read_text(encoding="utf-8")


class ProfileUpgradeRouting(unittest.TestCase):
    def test_full_requires_separate_old_result_authority(self):
        self.assertRegex(
            SCRIPT,
            re.compile(
                r"elseif \(\$Mode -eq 'Full' -and \$null -eq \$result "
                r"-and \$null -ne \$resultSearch "
                r"-and \$spec\.PSObject\.Properties\['profileUpgradeResultModel'\]\)\s*\{\s*"
                r"\$profileUpgradeResult=Absolute \(\[string\]\$spec\.profileUpgradeResultModel\)",
                re.MULTILINE,
            ),
        )

    def test_old_authority_keeps_registry_and_terminal_validation(self):
        self.assertIn("(Absolute ([string]$jobRecord.resultArtifactRef)) -ieq $profileUpgradeResult", SCRIPT)
        self.assertIn("$profileUpgradeResult -PathType Leaf", SCRIPT)
        self.assertIn("(Absolute ([string]$admission.resultModel)) -ine $profileUpgradeResult", SCRIPT)
        self.assertIn("$jobRecord.resultArtifactVerified", SCRIPT)
        self.assertIn("$jobRecord.resultManifestVerified", SCRIPT)
        self.assertIn("$jobRecord.resultSourceBindingVerified", SCRIPT)

    def test_new_full_result_must_be_distinct_and_inside_fresh_search(self):
        self.assertIn("$terminal.jobId -eq $upgradeAuthority.jobId", SCRIPT)
        self.assertIn("$newResult -ieq $oldResult", SCRIPT)
        self.assertIn("$newResult.StartsWith($resultPrefix,[StringComparison]::OrdinalIgnoreCase)", SCRIPT)
        self.assertIn("$priorResultMatches.Count -ne 0", SCRIPT)
        self.assertIn("$result=$terminal.resultModel", SCRIPT)
        self.assertIn("Log 'native-result-auto-bound'", SCRIPT)
        self.assertIn("Picker 'Export certified accepted samples' $csv $false $true", SCRIPT)

    def test_old_profile_authority_is_never_passed_to_result_inputs(self):
        self.assertNotRegex(SCRIPT, r"Picker[^\r\n]*\$profileUpgradeResult")
        self.assertNotRegex(SCRIPT, r"Picker[^\r\n]*profileUpgradeResultModel")
        self.assertNotRegex(SCRIPT, r"Picker[^\r\n]*\$result(?:\s|$)")

    def test_source_readback_binds_exact_name_revision_and_family(self):
        self.assertIn("'axialRod' {$expectedFamilyText='axial rod / cable'}", SCRIPT)
        self.assertIn("'planarBeam' {$expectedFamilyText='planar layered beam'}", SCRIPT)
        self.assertIn("'flatPlate' {$expectedFamilyText='flat layered plate'}", SCRIPT)
        self.assertIn("[regex]::Escape($sourceRootName)+' \\| revision [0-9]+ \\| '+[regex]::Escape($expectedFamilyText)+'$'", SCRIPT)
        self.assertIn("$sourceStatus.Current.Name -cmatch $sourceStatusPattern", SCRIPT)
        self.assertIn("$sourceStatusText -cnotmatch $sourceStatusPattern", SCRIPT)
        self.assertIn("[Pc11GuiUia]::IsSelected($selectedRoots[0])", SCRIPT)
        self.assertIn("workbenchReadback=$sourceStatus.Current.Name", SCRIPT)
        self.assertNotIn("Contains($sourceRootName)", SCRIPT)

    def test_model_root_selection_does_not_require_hidden_choose_model_button(self):
        self.assertNotIn("Action 'piezoChooseModel' 'Choose model' 'Button'", SCRIPT)
        selected_branch = SCRIPT.index("if(![Pc11GuiUia]::IsSelected($allRoots[0]))")
        direct_selection = SCRIPT.index("[Pc11GuiUia]::Act($allRoots[0])", selected_branch)
        readback_loop = SCRIPT.index("$selectionClock=[Diagnostics.Stopwatch]::StartNew()", direct_selection)
        self.assertLess(selected_branch, direct_selection)
        self.assertLess(direct_selection, readback_loop)
        self.assertIn("$selectionMethod='already-selected'", SCRIPT)
        self.assertIn("$selectionMethod='selected-by-harness'", SCRIPT)
        self.assertIn("selectionMethod=$selectionMethod", SCRIPT)
        self.assertIn("selectedRootAutomationId=$selectedRoots[0].Current.AutomationId", SCRIPT)
        self.assertIn("$sourceStatusText -cnotmatch $sourceStatusPattern", SCRIPT)

    def test_run_waits_for_live_preview_and_avoids_stale_results_tab(self):
        run_segment = SCRIPT[SCRIPT.index("if ($Mode -in @('Run','Full'))"):SCRIPT.index("if($null -ne $readRelease)")]
        self.assertNotIn("Accepted results", run_segment)
        self.assertNotIn("piezoShowAcceptedSeries", run_segment)
        self.assertNotIn("Picker", run_segment)
        run = SCRIPT.index("Action 'piezoRun' 'Run saved experiment'")
        preview = SCRIPT.index("$preview=Wait-PreviewPanel 120", run)
        confirm = SCRIPT.index("Action 'piezoConfirm' 'Confirm preview'", run)
        self.assertLess(run, preview)
        self.assertLess(preview, confirm)
        self.assertIn("Find($script:guiPid,'piezoPreviewPanel','','',$true)", SCRIPT)
        self.assertIn("$null -ne $panel -and $panel.Current.IsEnabled", SCRIPT)

    def test_confirm_is_not_waited_for_before_run_preview_exists(self):
        full_start = SCRIPT.index("if ($Mode -eq 'Full') {")
        run = SCRIPT.index("Action 'piezoRun' 'Run saved experiment'", full_start)
        pre_run = SCRIPT[full_start:run]
        self.assertNotIn("piezoConfirm", pre_run)
        self.assertNotIn("confirmVisible", pre_run)
        reveal_start = SCRIPT.index("function Reveal-WorkflowControls")
        reveal_end = SCRIPT.index("$sampler=$null", reveal_start)
        reveal = SCRIPT[reveal_start:reveal_end]
        self.assertIn("$controls=@('piezoRun')", reveal)
        self.assertNotIn("piezoConfirm", reveal)
        preview = SCRIPT.index("$preview=Wait-PreviewPanel 120", run)
        confirm = SCRIPT.index("Action 'piezoConfirm' 'Confirm preview'", preview)
        self.assertLess(run, preview)
        self.assertLess(preview, confirm)

    def test_automatic_result_binding_and_csv_picker_remain_required(self):
        self.assertIn("Log 'native-result-auto-bound'", SCRIPT)
        self.assertIn("Picker 'Export certified accepted samples' $csv $false $true", SCRIPT)
        for token in ("actualGuiSteps=20000", "Observe-BaselineMemory", "baseline-sampler-ready",
                      "'--root-pid'", "'--expected-root-image'", "'terminal-native-20000'"):
            self.assertIn(token, SCRIPT)

    def test_physical_ram_uses_direct_os_sampler_for_the_full_run(self):
        self.assertIn('GlobalMemoryStatusEx', SCRIPT)
        self.assertIn('new Timer(Capture, null, intervalMilliseconds, intervalMilliseconds)', SCRIPT)
        self.assertIn("::Start((Join-Path $out 'physical-ram'),50)", SCRIPT)
        self.assertIn('[Pc11PhysicalRamSampler]::Stop()', SCRIPT)
        self.assertIn('maximumUsedPercent', SCRIPT)
        self.assertNotRegex(SCRIPT, r'Get-Counter|Get-CimInstance|gwmi')
        self.assertIn("informational=$true", SCRIPT)

    def test_exposure_uses_main_root_for_hits_but_samples_workbench_bounds(self):
        self.assertIn("WindowExposure($visibleWindow.Current.NativeWindowHandle,$script:main.Current.NativeWindowHandle,$script:guiPid)", SCRIPT)
        self.assertIn("::Capture($visibleWindow.Current.NativeWindowHandle,$script:guiPid", SCRIPT)
        helper = Path(__file__).with_name("pc11_gui_uia.cs").read_text(encoding="utf-8")
        self.assertIn("WindowExposure(long targetHandle,long mainWindowHandle,int pid)", helper)
        self.assertIn("GetAncestor(target,3)", helper)
        self.assertIn("mainPid!=(uint)pid || !IsWindowVisible(main)", helper)
        self.assertIn("bool inMain=hit==main || IsChild(main,hit)", helper)
        self.assertIn("bool isTarget=hit==target", helper)
        self.assertIn("bool isTargetChild=hit!=target && IsChild(target,hit)", helper)
        self.assertIn("bool owned=ClassifyExposureHit(isTarget,isTargetChild,hitPid,targetPid)", helper)
        self.assertIn("return hitPid==targetPid && (hitIsTarget || hitIsTargetChild)", helper)
        self.assertNotRegex(helper, r"hitPid\s*==\s*\(uint\)pid\s*&&\s*\(inTarget\s*\|\|\s*inMain\)")
        self.assertIn("OwnedByMain=inMain", helper)
        self.assertIn("AllSamplePointsOwned=allOwned", helper)

    def test_measurement_windows_are_arranged_before_acceptance_work(self):
        native = Path(__file__).with_name("pc11_gui_native.cs").read_text(encoding="utf-8")
        target_found = SCRIPT.index("$script:measurementWindow=[Pc11GuiUia]::NativeWindowFor($run)")
        layout_call = SCRIPT.index("Arrange-MeasurementWindows 'workbench-discovered'")
        existing_models = SCRIPT.index("$existingModelRoots=@(Get-ModelTreeRoots)", layout_call)
        run_action = SCRIPT.index("Action 'piezoRun' 'Run saved experiment'", layout_call)
        visible_branch = SCRIPT.index("$workbenchLayoutKind='already-visible-workbench'")
        self.assertLess(target_found, layout_call)
        self.assertLess(visible_branch, layout_call)
        self.assertLess(layout_call, existing_models)
        self.assertLess(layout_call, run_action)
        self.assertIn("Arrange-MeasurementWindows 'main-after-source-open'", SCRIPT)
        self.assertIn("ArrangeMeasurementWindows(long mainHandle,long workbenchHandle,int pid)", native)
        self.assertIn("swpNoZOrder|swpNoActivate", native)
        self.assertIn("if(areaWidth<2340 || areaHeight<700)", native)
        self.assertIn("int totalWidth=Math.Min(3200,areaWidth)", native)
        self.assertIn("int mainWidth=Math.Min(1330,totalWidth-1000)", native)
        self.assertIn("int workbenchWidth=totalWidth-mainWidth", native)
        self.assertIn("int actualMainWidthBeforeWorkbench=mainBounds.Right-mainBounds.Left", native)
        self.assertIn("SetWindowPos(workbench,IntPtr.Zero,workbenchLeft,info.Work.Top,workbenchWidth,height,flags)", native)
        self.assertIn("bool widthBound=actualTotalWidth<=areaWidth", native)
        self.assertIn("actualMainWidth>=640 && actualWorkbenchWidth>=1000 && actualWorkbenchHeight>=700", native)
        self.assertIn("LayoutValid=noOverlap && within && sameMonitor && dimensions && widthBound", native)


if __name__ == "__main__":
    unittest.main()
