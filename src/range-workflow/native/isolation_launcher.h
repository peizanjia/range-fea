#ifndef RANGE_WORKFLOW_ISOLATION_LAUNCHER_H
#define RANGE_WORKFLOW_ISOLATION_LAUNCHER_H

// Standalone, host-only launcher. No general executable/argv interface is exposed.
// See docs/agent_playbooks/wf12_launcher_checkpoint.md for the versioned protocol.
int rangeWorkflowIsolationLauncherMain(int argc, char **argv);

#endif
