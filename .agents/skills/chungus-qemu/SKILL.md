---
name: chungus-qemu
description: Build this QEMU-mod fork for the CHUNGUS s390x Linux KVM host, transfer and deploy binaries, and investigate z/OS device or KVM behavior. Use for this repository and host workflow.
---

# QEMU-mod on CHUNGUS

Read the repository's applicable AGENTS.md and inspect the current Git state before changing anything. This fork prioritizes local functionality over conflicting upstream functionality, and requires license compliance. Do not import upstream agent guidelines. Hercules may inform behavioral comparisons; do not lift its code.

## Locations and invariants

- Local source: `/home/friedkiwi/src/qemu` (amd64 host).
- Remote: `friedkiwi@chungus.lan.cyber.gent`, SSH key authentication, native s390x Linux/KVM.
- Remote source normally `/home/friedkiwi/src/qemu`, native binary `build/qemu-system-s390x`.
- Install location (since 2026-11-15): `/kvm/bin` (`qemu-system-s390x` plus `qemu-img`, `qemu-io`, `qemu-nbd`, `qemu-pr-helper`, `qemu-bridge-helper`, `qemu-edid`), firmware `/kvm/share/qemu` (found relative to the binary, no `-L` needed). Always install the latest validated builds there. `/home/friedkiwi/qemu-mod/{bin,share}` is the old location and keeps older backups only.
- Research Git repo: `/home/friedkiwi/src/zos-research`, outside QEMU. Inspect its current notes rather than trusting historical hypotheses. No remote was configured there; verify before promising a push.
- Disks: `/home/friedkiwi/z1090/disks`. Use `-snapshot` for experimental IPLs. Do not modify base volumes or commit snapshot writes.
- Execute **KVM only**, never TCG, including test wrappers. Building cross targets does not authorize executing them through binfmt/QEMU user emulation. Bare-metal guest tests run via `qemu-system-s390x -accel kvm -cpu host -kernel TEST`; never execute their ELF directly as Linux programs.
- Do not invoke IBM awsckd/zPDT. Static examination is permitted when relevant. User authorizes Hercules as a behavioral oracle, not source copying.

Read [build-and-transfer.md](references/build-and-transfer.md) for cross/native compilation, scp, deployment and verification. Read [research-and-runtime.md](references/research-and-runtime.md) for IPL/debugging and evidence collection.

## Working in this checkout

Use grep/rg via the Bash tool (or an Explore agent for broad sweeps, when delegation is authorized) to locate subsystem code, the Read/Edit/Write tools for local edits, and focused tests proportional to the change. For unfamiliar QEMU routing, invoke the `qemu-codebase` skill (`.agents/skills/qemu-codebase/SKILL.md`). Known entry points: `hw/s390x/eckd-ccw.c`, `hw/s390x/ckd-image.c`, `target/s390x/ioinst.c`, `target/s390x/kvm/kvm.c`, `hw/s390x/s390-virtio-ccw.c` (incl. `mtm-*` machines), `hw/s390x/sclp.c`, `hw/s390x/event-facility.c`, `hw/char/sclpconsole-lm.c` (line-mode operator console), `hw/char/sclpconsole-3270.c` (integrated 3270 console), and `hw/char/*3270*` / `terminal3215`. SCLP console usage and tracing is summarized in [research-and-runtime.md](references/research-and-runtime.md).

Existing tracked edits, untracked `.debug` dumps/build trees and dirty ROM submodules may belong to the user or other agents. Preserve them. Coordinate ownership before editing shared files. For simultaneous research, one agent owns the live VM while others inspect static code. Use subagents when explicitly requested/authorized; skill use alone does not authorize new delegation.

Inspect `git remote -v`, branch, HEAD and worktree on BOTH hosts. Local canonical origin has been GitHub `cyberdotgent/qemu-mod`; remote CHUNGUS source has had a different GitLab origin and divergent branch history. Never push the remote branch wholesale or reset either checkout to synchronize it. Transfer scoped patches or a known source archive to an isolated tree. Record any dirty overlay included in a build; a Git version string alone is insufficient provenance.

When commits/pushes are requested, stage only owned files, inspect staged diff and run `git diff --check`; use `scripts/checkpatch.pl` with appropriate project conventions. Push only the intended repository/branch. Report code, validation, installed binary and still-running guest state separately.
