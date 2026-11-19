# Runtime and evidence

For the supplied C5 z/OS2.5 volumes, authoritative IPL is `-ipl 0A80 -loadparm 0A82CS`. Earlier NZ IPL paths differ. Use an existing fully inspected volume/topology launcher as a baseline, then assign unique console/GDB ports and monitor sockets. Old launchers may contain global pkill commands; do not execute them unchanged alongside another guest.

Current default compatibility behavior: `-machine czam -accel kvm -cpu host` performs the guarded z/OS SVT adjustment at STSI exits automatically. The old `QEMU_S390_ZOS_COMPAT` environment variable is no longer used. Generic `s390-ccw-virtio` does not perform it. Source and tests are authoritative if this changes.

The hook checks PSA0x10 -> CVT+0x8c -> ECVT+0x1dc level>=0x0102 and clears `[PSA0xb4c]+0x560`. This is an emulator compatibility side effect, not architectural STSI/STNSM behavior. It reproduces the result of Hercules's STNSM0x950/ff adjustment without copying code. Prior natural-STSI KVM IPL reached JES2, jobs, VTAM and TSO; see research `oracles/hercules-np05-svt560-clear.md`, `boot/master-scheduler/svt560-zero-test.md`, and the later STSI validation report. Do not repeat superseded missing-DASD, UCBTYP, or negative-breakpoint hypotheses without new evidence.

Under `czam`, SCLP Read SCP Info and CHSC characteristics may also be patched to real z14 ZR1 LPAR values (`s390_mainframe_patch_read_info`, `s390_mainframe_chsc_characteristics`). Check `git log`/`git status` for whether that work is committed or still a working-tree overlay before relying on it or reporting provenance. The machine also exposes `lpar-name` (SYSIB 2.2.2, default `QEMUKVM`).

## QETH / OSA networking

`qeth-ccw` supports layer 2 and layer 3 (guest-selected in ULP_ENABLE). z/OS
TCP/IP uses layer 3: the image's TRLE `OSATRL1E` is READ/WRITE/DATAPATH
0400/0401/0402, profile `ADCD.Z25C.TCPPARMS(PROF2)` -> `ZPDTDEV1` (HOME
85.234.216.174, default gateway 85.234.216.161). Attach with
`-qeth devno=0400,hostdev=tap0`; CHUNGUS's persistent `tap0` carries
85.234.216.161/28 (plus 192.168.70.1/24 and fd70::1/64), so `ping
85.234.216.174` and `c3270 85.234.216.174` work from the host once TCP/IP shows
`EZZ4313I INITIALIZATION COMPLETE FOR DEVICE PORTA`. Only one QEMU can hold
`tap0` at a time.

Trace with `-trace 'qeth_*' -trace 'qdio_*'` (`qeth_ctrl_data` dumps raw
IDX/MPC/IPA). Unattended tooling lives in `~/qeth-dev` on CHUNGUS:
`run-zos.sh RUN` (sockets, scripted s3270, tcpdump), `zcmd RUN "cmd"`,
`screen.sh PORT`, `stop-zos.sh RUN`, and `linux-qeth-test.sh RUN` (Linux
qeth_l2/qeth_l3 regression with the host kernel and initramfs, IPv4 and
IPv6), `zos-ipv4-regress.sh RUN QEMU QETH_OPTS` and `tn3270-check.sh HOST`
(accepts `[fd70::174]`). Every QEMU using `tap0` must hold
`~/qeth-dev/tap0.lock` (flock). Never use `pkill -f`/`pgrep -f` patterns that
also match the invoking ssh command line.

IPv6 testbed: `~/zos25-ipv6.sh` boots the image with persistent qcow2
overlays (`~/z1090/overlays/ipv6/`, with `pristine/` and `configured/`
copies; base volumes stay untouched) and `-qeth devno=0400,hostdev=tap0,datadevs=2`
(the testbed's TRLE lists a second data device, so the 3-device adapter fails
there). Guest IPv6 address fd70::174, gateway fd70::1. Unattended:
`~/qeth-dev/ipv6/test-ipv6.sh RUN [QEMU]` (boot, evidence, IPv4/IPv6 ping and
TN3270 checks, orderly shutdown), `reset-overlay.sh --to-configured`. Details
and member texts: zos-research `investigations/ipv6-testbed.md`. The bring-up
history is in zos-research `investigations/qeth-layer3.md`.

## SCLP consoles

Source and `tests/tcg/s390x/system/meson.build` are authoritative; re-check them if this drifts.

- **Line-mode operator console** `sclplmconsole` (event type 0x02, message/MDB framing): properties `chardev`, `echo` (default on; use `echo=off` for scripted input), `write_errors`. Input is delivered per line on CR/LF. z/OS operator messages appear here; `VARY CN(*),ACTIVATE` enabled input/output in prior runs.
- **Integrated 3270 console** `sclp3270console` (event type 0x1b): property `chardev` only. It writes TN3270 records (TELNET-quoted, EOR-terminated) and relies on the chardev for negotiation, so attach it to `socket,...,server=on,wait=off,tn3270=on` and connect `s3270`/`c3270`. With no client attached (chardev open/close events) or a failed write, any command other than NOP is answered 0x05F0 "improper state" and the buffer is left unaccepted, instead of an equipment check that would loop guest console recovery. NOP is always accepted.
- **Serial mapping:** on `czam` (`line_mode_console_first`), `-serial` #0 becomes `sclplmconsole` and #1 the VT220 `sclpconsole`; generic `s390-ccw-virtio` keeps VT220 first. So `-serial unix:RUN/lm.sock,server=on,wait=off` alone gives the z/OS line console under `mtm-3907`; add `-device sclplmconsole,...` explicitly only when you need `echo=off` or a non-mtm machine.
- **Tracing:** `-trace 'sclp_*'` covers `sclp_write_event` (type/len/rc for every write event), `sclp_unimplemented_command`, `sclp_lm_mdb_object` and `sclp_lm_message`; ECKD/CHSC equivalents are `eckd_unimplemented`, `eckd_accepted_ignored`, `ioinst_chsc_*unimplemented`. Use these to tell "message never sent" from "sent and dropped" before patching console code. Send trace output to a file (`-D RUN/trace.log`), not the console.
- **KVM tests:** `sclp-3270.c` (ringbuf), `sclp-3270-noclient.c` (listening socket, no client → 0x05F0), `sclp-line-console.c` and `sclp-line-console-czam.c` (scripted `D IPLINFO` input via a wrapper). Run on CHUNGUS with KVM only; `sclp-3270.c` and `sclp-line-console.c` meson args omit `-accel kvm`, so add `-accel kvm -cpu host` explicitly rather than letting them fall back to TCG.

Useful launch fragments (include the full required DASD/3270 configuration separately; replace RUN, ports and sockets with unique values):

```
-machine czam -accel kvm -cpu host -smp 1 -m 32G
-L /kvm/share/qemu -snapshot -S
-chardev socket,id=sclplm,path=RUN/lm.sock,server=on,wait=off
-device sclplmconsole,chardev=sclplm,echo=off
-chardev socket,id=sysg,host=127.0.0.1,port=UNIQUE_TN_PORT,server=on,wait=off,tn3270=on
-device sclp3270console,chardev=sysg
-monitor unix:RUN/monitor.sock,server=on,wait=off
-gdb tcp:127.0.0.1:UNIQUE_PORT
-D RUN/trace.log -trace 'sclp_*'
-ipl 0A80 -loadparm 0A82CS
```

Attach the 3270 console(s) and press Enter before releasing the processor with monitor `cont`. Drive the line console with `socat - UNIX-CONNECT:RUN/lm.sock`. `D A,L` demonstrates active jobs; `L TSO` on the VTAM welcome terminal reached `IKJ56700A ENTER USERID`. A logon prompt does not prove authenticated TSO usage. Preserve transcripts and list residual nonfatal errors explicitly.

Tools: the Bash tool for local and ssh/scp commands; Read/Edit/Write for local files; remote native gdb, socat and s3270; local gdb-multiarch and s390x-linux-gnu-objdump/readelf. Remote rg was absent in earlier sessions, so fall back to grep.

Bash has no interactive stdin, so do not rely on typing into a foreground process. Run long-lived remote guests detached (e.g. `ssh ... 'nohup ... >RUN/qemu.log 2>&1 &'` or inside a remote tmux/screen session) and drive them non-interactively: QEMU monitor via `socat - UNIX-CONNECT:RUN/monitor.sock`, 3270 consoles via scripted `s3270` (`-script` or piped actions), guest GDB via `gdb -batch -ex ...`. Replace `-chardev stdio` for the line console with a socket or file chardev for the same reason. Use Bash `run_in_background` or the Monitor tool to wait on a log condition rather than foreground sleeps, and keep output bounded (`tail`, `grep -m`). WebFetch/WebSearch and the IBM BookServer (geist) tools can inspect primary architecture documentation/current Hercules if needed. Other app connectors are not required.

For debugging, distinguish host-gdb attached to QEMU from guest GDB stub. The stub connection pauses the guest. Validate live instruction signatures and relocation before setting breakpoints; CSECTs can be scattered across XTLST extents, and alias names may lack CDEs. Hardware breakpoint zero-length handling was fixed in b92eaf905a; verify the running binary contains the fix before drawing conclusions from misses. Test a positive breakpoint control before relying on negative results. Post-DMA page-copy gates can precede guest DAT installation. Avoid hundreds of thousands of remote single steps or huge whole-memory scans.

Capture the actual causal branch/control-block input and guest PSW/registers before recovery changes state. For S0A0 the original context is in SDWA, while current RB/TCB may describe recovery. Clean channel-end/device-end status does not prove correct record payload. Compare semantics with Hercules without lifting code, then add a focused KVM regression for the specific defect.

Keep investigation notes and small reproducible tools in `/home/friedkiwi/src/zos-research`, with exact source commit, dirty diff provenance, binary hash, command, machine/loadparm, run directory, timestamps, expected/observed outcome and caveats. Keep large dumps/traces out of Git; preserve their paths/hashes and use bounded extracts. Existing HANDOVER-2026-11-14.md is historical and partly superseded by the successful compatibility fix; read later notes before acting.
