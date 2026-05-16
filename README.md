# X86_REPL

An interactive x86-64 assembly REPL that executes real instructions on hardware.
Each instruction you type is assembled by GNU `as`, injected into a live process
via `ptrace`, executed on the physical CPU, and its full register state is captured
and printed with per-field change highlighting.

Unlike simulators or emulators, this tool produces the exact same behavior as
machine code running natively, including correct flag semantics, alignment faults,
memory ordering, and hardware-maintained control register values.

---

## Architecture

The system is built around three independent register access paths:

**Path 1 — ptrace (primary)**
The REPL maintains a long-lived child process.  On each input line the assembled
bytes are written into a shared RWX mapping in the child via `PTRACE_POKETEXT`,
the instruction pointer is redirected to that mapping, and execution is resumed.
The child halts at a trailing `INT3`.  The parent then reads back all general-purpose
registers, segment bases, rflags, and floating-point/SSE state using
`PTRACE_GETREGS` and `PTRACE_GETFPREGS`.  This path requires no elevated privileges.

**Path 2 — kernel module (optional, ioctl)**
A loadable kernel module (`kmod/asmrepl.ko`) exposes `/dev/asmrepl` with world-
readable permissions (mode 0666).  Through this device, the REPL can read:

- Control registers: CR0, CR2, CR3, CR4, CR8
- Debug registers: DR0 through DR7
- A curated set of model-specific registers (MSRs): EFER, STAR, LSTAR, CSTAR,
  SFMASK, FS.BASE, GS.BASE, KernelGSBase, TSC, APIC_BASE, SYSENTER_CS/ESP/EIP,
  MTRRcap, and others.

All MSR reads are guarded by a static whitelist and by `rdmsr_safe()`, so a
`#GP` fault from a non-existent or restricted MSR terminates only the ioctl call
and does not affect the kernel.

**Path 3 — /dev/mem (optional, root)**
For memory-mapped hardware registers (local APIC, HPET), the tool optionally maps
the relevant physical pages through `/dev/mem`.  This path requires root access and
is silently skipped when unavailable.

---

## Prerequisites

| Component | Package (Arch Linux) |
|-----------|----------------------|
| C compiler | `gcc` |
| GNU assembler | `binutils` |
| editline | `libedit` |
| Kernel headers | `linux-headers` |

On other distributions, install the equivalent packages.  The kernel headers must
match the running kernel version exactly for the module build to succeed.

---

## Building

### Userspace binary

```sh
make
```

This produces the `asmrepl` binary.  No root is required.

### Kernel module (optional)

```sh
cd kmod
make
sudo make install
```

`make install` calls `insmod`.  The device node `/dev/asmrepl` is created
automatically by the kernel's devtmpfs/udev subsystem via the module's `devnode`
callback, which sets mode 0666.

To unload:

```sh
cd kmod
sudo make uninstall
```

---

## Usage

```
asmrepl [--att | --hex]
```

Start without arguments for Intel syntax (default).

### Input modes

| Flag | Description |
|------|-------------|
| _(none)_ | Intel syntax: `mov rax, 1` |
| `--att` | AT&T syntax: `movq $1, %rax` |
| `--hex` | Hex bytes: `48 c7 c0 01 00 00 00` |

Syntax can also be changed at runtime with `.syntax intel`, `.syntax att`,
or `.syntax hex`.

### Built-in commands

```
.gpr              General-purpose registers, rflags, segment bases
.xmm              XMM0 through XMM15 (128-bit, hex)
.fpu              x87 ST(0-7), FCW, FSW, MXCSR
.cr               CR0, CR2, CR3, CR4, CR8 with decoded bit fields  [needs kmod]
.dr               DR0 through DR7 with condition and mode decode   [needs kmod]
.msr              Whitelisted MSRs with symbolic names             [needs kmod]
.msr <hex_addr>   Read a specific MSR by address, e.g. .msr c0000080
.all              Print all of the above
.reset            Zero GPR and XMM state in the child process
.syntax <mode>    Switch input syntax (intel / att / hex)
.help             Print command reference
.quit / .exit     Exit the REPL (Ctrl-D also works)
```

Register values that changed since the previous instruction are highlighted in
yellow.  Unchanged values are shown in the default terminal color.

### Example session

```
Syntax: intel   |   Execution: ptrace child pid 12345

> mov rax, 100
  rax     0x0000000000000064 <--

> mov rbx, 200
  rbx     0x00000000000000c8 <--

> imul rax, rbx
  rax     0x0000000000004e20 <--

> xor rdx, rdx
> mov rsi, 7
> div rsi
  rax     0x0000000000000b29 <--   ; 20000 / 7 = 2857
  rdx     0x0000000000000001 <--   ; remainder 1

> .cr
  CR0     0x0000000080050033   [PG WP NE ET MP PE]
  CR4     0x0000000000f72ef0   [SMAP SMEP OSXSAVE PCE PGE PAE PSE DE VME]

> .msr
  EFER    0x0000000000000d01   (SCE LME LMA NXE)
  LSTAR   0xffffffffa7600080
  ...

> .quit
```

---

## Design notes

**Register delta tracking.**  The REPL maintains a copy of the register state
from before each instruction and computes a field-by-field diff.  Only fields
whose value changed are annotated.  This makes it straightforward to observe the
side effects of a single instruction without reading every line of output.

**Child process lifecycle.**  The trampoline child is created once and reused
across all inputs.  Reuse preserves accumulated register state, which is necessary
for multi-instruction sequences (loops, function prologues, etc.) to work
correctly.  If the child terminates abnormally (e.g., a `ud2` or a memory fault),
the REPL automatically forks a new child and continues.

**Assembly delegation.**  Rather than embedding an assembler, the tool delegates
to the system's `as` (GNU Binutils) via a temporary file pair.  `as --64`
produces an ELF object; `objcopy -O binary --only-section=.text` strips it down
to raw machine bytes.  This ensures complete instruction set coverage without
maintaining an encoder.

**SMP note.**  CR and DR reads from the kernel module reflect the state of
whichever CPU core executes the ioctl.  Because `ptrace` already serializes child
execution, the values are consistent for single-threaded instruction sequences.
For strict per-CPU accuracy across concurrent workloads an `smp_call_function_single`
wrapper would be needed; this is not implemented.

**MSR safety.**  MSR reads in the kernel module use `rdmsr_safe()`, which is
backed by the kernel's exception fixup table.  A `#GP` fault (raised on some
virtualized or restricted MSRs) causes `rdmsr_safe()` to return `-EIO` and
propagates as an ioctl error; it does not crash or hang the kernel.

---

## File layout

```
asmrepl_main.c      REPL main loop, command dispatch, libedit integration
asm_encode.c/.h     Instruction assembly: delegates to GNU as + objcopy
ptrace_exec.c/.h    ptrace child: code injection, execution, reset
hw_regs.c/.h        Three-path register capture and ANSI pretty-printers
include/
  asmrepl_ioctl.h   Shared ioctl definitions (userspace and kernel module)
kmod/
  asmrepl.c         Linux kernel module: /dev/asmrepl char device
  Makefile          Kernel module build rules
Makefile            Userspace build rules
```

---

## License

The userspace code is released under the MIT License.
The kernel module (`kmod/asmrepl.c`) is licensed under GPL-2.0-only, as required
for loadable kernel modules that use internal kernel symbols.

