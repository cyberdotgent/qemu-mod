# Runtime and evidence

For the supplied C5 z/OS2.5 volumes, authoritative IPL is `-ipl 0A80 -loadparm 0A82CS`. Earlier NZ IPL paths differ. Use an existing fully inspected volume/topology launcher as a baseline, then assign unique console/GDB ports and monitor sockets. Old launchers may contain global pkill commands; do not execute them unchanged alongside another guest.

Current default compatibility behavior: `-machine mtm-3907 -accel kvm -cpu host` performs the guarded z/OS SVT adjustment at STSI exits automatically. The old `QEMU_S390_ZOS_COMPAT` environment variable is no longer used. Generic `s390-ccw-virtio` does not perform it. Source and tests are authoritative if this changes.

The hook checks PSA0x10 -> CVT+0x8c -> ECVT+0x1dc level>=0x0102 and clears `[PSA0xb4c]+0x560`. This is an emulator compatibility side effect, not architectural STSI/STNSM behavior. It reproduces the result of Hercules's STNSM0x950/ff adjustment without copying code. Prior natural-STSI KVM IPL reached JES2, jobs, VTAM and TSO; see research `oracles/hercules-np05-svt560-clear.md`, `boot/master-scheduler/svt560-zero-test.md`, and the later STSI validation report. Do not repeat superseded missing-DASD, UCBTYP, or negative-breakpoint hypotheses without new evidence.

Useful launch fragments (include the full required DASD/3270 configuration separately):

```
-machine mtm-3907 -accel kvm -cpu host -smp 1 -m 32G
-L /home/friedkiwi/qemu-mod/share/qemu -snapshot -S
-chardev stdio,id=sclplm,signal=off
-device sclplmconsole,chardev=sclplm,echo=off
-monitor unix:/tmp/UNIQUE_RUN/monitor.sock,server=on,wait=off
-gdb tcp:127.0.0.1:UNIQUE_PORT
-ipl 0A80 -loadparm 0A82CS
```

Attach a3270 console and press Enter before releasing the processor with monitor `cont`. For the line console, `VARY CN(*),ACTIVATE` enabled input/output in prior runs. `D A,L` demonstrates active jobs; `L TSO` on the VTAM welcome terminal reached `IKJ56700A ENTER USERID`. A logon prompt does not prove authenticated TSO usage. Preserve transcripts and list residual nonfatal errors explicitly.

Tools: local exec_command/write_stdin and apply_patch; ssh/scp; remote native gdb, socat and s3270; local gdb-multiarch and s390x-linux-gnu-objdump/readelf. Remote rg was absent in earlier sessions, so fall back to grep. Web tools can inspect primary architecture documentation/current Hercules if needed. App connectors are not required.

For debugging, distinguish host-gdb attached to QEMU from guest GDB stub. The stub connection pauses the guest. Validate live instruction signatures and relocation before setting breakpoints; CSECTs can be scattered across XTLST extents, and alias names may lack CDEs. Hardware breakpoint zero-length handling was fixed in b92eaf905a; verify the running binary contains the fix before drawing conclusions from misses. Test a positive breakpoint control before relying on negative results. Post-DMA page-copy gates can precede guest DAT installation. Avoid hundreds of thousands of remote single steps or huge whole-memory scans.

Capture the actual causal branch/control-block input and guest PSW/registers before recovery changes state. For S0A0 the original context is in SDWA, while current RB/TCB may describe recovery. Clean channel-end/device-end status does not prove correct record payload. Compare semantics with Hercules without lifting code, then add a focused KVM regression for the specific defect.

Keep investigation notes and small reproducible tools in `/home/friedkiwi/src/zos-research`, with exact source commit, dirty diff provenance, binary hash, command, machine/loadparm, run directory, timestamps, expected/observed outcome and caveats. Keep large dumps/traces out of Git; preserve their paths/hashes and use bounded extracts. Existing HANDOVER-2026-11-14.md is historical and partly superseded by the successful compatibility fix; read later notes before acting.
