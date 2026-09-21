tests = []

minilib_dir = meson.current_source_dir() / '..' / '..' / 'minilib'
minilib_printf = files(minilib_dir / 'printf.c')[0]
link_script = files('../softmmu.ld')[0]
head64 = files('../head64.S')[0]
console = files('../console.c')[0]
ipl_inc = meson.project_source_root()/'include'/'hw'/'s390x'/'ipl'
libc_inc = meson.current_source_dir()/'..'/'libc'
cflags = ['-nostdlib',
          '-ffreestanding',
          '-Wa,--noexecstack',
          '-I', minilib_dir, minilib_printf,
          '-I', ipl_inc,
          '-I', libc_inc,
          '-march=z13',
          head64, console]
asmflags = ['-nostdlib',
            '-Wl,-T', link_script, '-Wl,--build-id=none',
            '-march=z13', '-Wa,--noexecstack']
qemu_args = ['-display', 'none',
             '-action', 'panic=exit-failure',
             '-serial', 'stdio',
             '-kernel']

# Multi arch tests - add cflags only once per src
multi_src = []
foreach t: tcg_tests['multiarch-softmmu']['tests']
  foreach src, setup: t
    new_cflags = cflags
    if fs.stem(src) == 'memory'
      new_cflags += '-DCHECK_UNALIGNED=0'
    endif
    if 'test_name' in setup and setup['test_name'] == 'memory-access'
      # s390x clears the BSS section so we need to account for that
      # ./validate-memory-counts.py --bss-cleared
      setup += {'wrapper': setup['wrapper'] + '--bss-cleared'}
    endif
    if src not in multi_src
      setup += {'cflags': [new_cflags]}
      multi_src += src
    endif
    tests += {src: setup + {'qemu_args': qemu_args}}
  endforeach
endforeach

tests += {
  'mvc-smc.c': {'cflags': [cflags], 'qemu_args': qemu_args},
}

setup_asm = {'cflags': [asmflags], 'qemu_args': qemu_args}
tests += {
  'bal.S': setup_asm,
  'cksm.S': setup_asm,
  'clm.S': setup_asm,
  'exrl-ssm-early.S': setup_asm,
  'icm.S': setup_asm,
  'sam.S': setup_asm,
  'lpsw.S': setup_asm,
  'lpswe-early.S': setup_asm,
  'lra.S': setup_asm,
  'mc.S': setup_asm,
  'per.S': setup_asm,
  'precise-smc-softmmu.S': setup_asm,
  'sckc.S': setup_asm,
  'ssm-early.S': setup_asm,
  'stosm-early.S': setup_asm,
  'stpq.S': setup_asm,
  'unaligned-lowcore.S': setup_asm,
}

# Control-program and z/OS oriented instruction tests
tests += {
  'art.S': setup_asm,
  'asn-control.S': setup_asm,
  'bakr.S': setup_asm,
  'bassm.S': setup_asm,
  'cpu-timer.S': setup_asm,
  'csp-purge.S': setup_asm,
  'dfp-convert.S': setup_asm,
  'ipk.S': setup_asm,
  'ipte-alias.S': setup_asm,
  'keyed-move.S': setup_asm,
  'linkage-exceptions.S': setup_asm,
  'pc.S': setup_asm,
  'pc-ss.S': setup_asm,
  'pfpo.S': setup_asm,
  'pqap.S': setup_asm,
  'ptff.S': {'cflags': [asmflags], 'qemu_args': ['-cpu', 'max', qemu_args]},
  'storage-key.S': setup_asm,
  'strag.S': setup_asm,
  'svs.S': setup_asm,
  'tod-clock.S': setup_asm,
  'tprot-key.S': setup_asm,
  'trace.S': setup_asm,
  'transaction-constrained.S': setup_asm,
  'trte.S': setup_asm,
  'xc-teid.S': setup_asm,
}

# C tests using the minilib runtime
setup_c = {'cflags': [cflags], 'qemu_args': qemu_args}
tests += {
  'decimal.c': setup_c,
  'diag204.c': setup_c,
  'edat.c': setup_c,
  'hfp.c': setup_c,
  'la.c': setup_c,
  'plo.c': setup_c,
}

# Channel I/O device tests built against the s390-ccw firmware headers
ccw_inc = meson.project_source_root()/'pc-bios'/'s390-ccw'
ccw_cflags = cflags + ['-I', ccw_inc]
fba_qemu_args = ['-display', 'none',
                 '-action', 'panic=exit-failure',
                 '-serial', 'stdio',
                 '-nodefaults',
                 '-blockdev', 'driver=null-co,node-name=fba,size=1048576',
                 '-device', 'fba-ccw,drive=fba,devno=fe.0.0200',
                 '-kernel']
term3215_qemu_args = ['-display', 'none',
                      '-action', 'panic=exit-failure',
                      '-serial', 'stdio',
                      '-nodefaults',
                      '-chardev', 'ringbuf,id=term,size=4096',
                      '-device', '3215-ccw,chardev=term,devno=fe.0.0009',
                      '-kernel']
term3215_input_qemu_args = ['-monitor', 'none',
                            '-display', 'none',
                            '-nodefaults',
                            '-action', 'panic=exit-failure',
                            '-dev3215', 'devno=009',
                            '-kernel']
tests += {
  'fba-tic.c': {'cflags': [ccw_cflags], 'qemu_args': fba_qemu_args},
  'sclp-chp.c': {'cflags': [ccw_cflags], 'qemu_args': fba_qemu_args},
  'sclp-cpu.c': {'cflags': [ccw_cflags], 'qemu_args': qemu_args},
  'terminal3215.c': {'cflags': [ccw_cflags], 'qemu_args': term3215_qemu_args},
  'terminal3215-input.c': {
    'cflags': [ccw_cflags],
    'qemu_args': term3215_input_qemu_args,
    'wrapper': ['sh', '-c',
                '{ sleep 1; printf \'0 yes\\n\'; sleep 1; } | exec "$@"', 'sh'],
  },
  # The following drivers launch QEMU themselves and only take the
  # emulator and guest binary as arguments.
  'eckd-zero-data.c': {
    'cflags': [ccw_cflags],
    'wrapper': [python, files('../eckd-zero-data.py')],
  },
  'terminal3270-skip.c': {
    'cflags': [ccw_cflags],
    'wrapper': [python, files('../terminal3270-skip.py')],
  },
  'terminal3270-residual.c': {
    'cflags': [ccw_cflags],
    'wrapper': [python, files('../terminal3270-residual.py')],
  },
  'tape-write.c': {
    'cflags': [ccw_cflags],
    'wrapper': [python, files('../tape-write.py')],
  },
}
tests += {
  'tape-write.c': {
    'exe_name': 'tape-write-protect',
    'cflags': [ccw_cflags, '-DTAPE_WRITE_PROTECT_TEST'],
    'wrapper': [python, files('../tape-write.py'), '--protect'],
  },
}

foreach src: tests_s390x_pgm
  tests += {
    src: {
      'cflags': [asmflags, files('../pgm-specification-softmmu.S')],
      'qemu_args': qemu_args,
    }
  }
endforeach

if 'qemu-system-s390x' in emulators
  tcg_tests += {
    's390x-softmmu': {
      'cc': cc,
      'cc_dockerfile': cc_dockerfile,
      'cc_docker_host_arch': cc_docker_host_arch,
      'cc_feat_cflags': cc_feat_cflags,
      'folder': 's390x',
      'gdb_arch': 's390x',
      'qemu': emulators['qemu-system-s390x'],
      'tests': tests,
    }
  }
endif
