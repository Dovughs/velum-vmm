; src/vmx.asm
; IPI_LEVEL-safe leaf wrappers (no callees): resident .text and kernel stack only.
; No pageable memory or calls; frame metadata describes the stack operations.
; Return 0 on success, 1 on VMfailValid (ZF), 2 on VMfailInvalid (CF).
; Windows x64 ABI: RCX=PA/field, RDX=value/output pointer; EAX=status.
; VMREAD writes output only on success; at IPI_LEVEL its output must be nonpaged.
; VMCS opcodes/operand order: Intel SDM Vol. 3C 326019-078US 31.3.

OPTION CASEMAP:NONE
; Export names collide with MASM mnemonics; emit the instruction bytes below.
OPTION NOKEYWORD:<VMXON, VMXOFF>

; C dispatch/helper functions called from the VM-exit handler.
EXTERN VmExitDispatch:PROC
EXTERN FilterDecide:PROC
EXTERN KernelCapture:PROC
; Offset of PER_CPU_DATA.LaunchCallerRsp, read by the VMRESUME failure fallback.
EXTERN g_LaunchCallerRspOffset:DWORD
; Offset of PER_CPU_DATA.GuestRegs, read by the GPR save/restore.
EXTERN g_GuestRegsOffset:DWORD
; Offset of PER_CPU_DATA.Persist (shutdown state).
EXTERN g_PersistOffset:DWORD
EXTERN g_NativeOffset:DWORD
EXTERN VmExitResumeFailure:PROC
EXTERN PersistRootNmi:PROC
EXTERN PersistRootFault:PROC
; CPUID probe result buffer (src/driver.c).
EXTERN g_CpuidProbe:QWORD
; syscall hook globals (src/syscall.c).
EXTERN g_PerCpuData:QWORD

.code

PUBLIC VmxOn
PUBLIC VmxOff

VmxOn PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    mov qword ptr [rsp], rcx
    db 0F3h, 00Fh, 0C7h, 034h, 024h ; vmxon qword ptr [rsp]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    add rsp, 10h
    pop rbp
    ret
VmxOn ENDP

VmxOff PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    db 00Fh, 001h, 0C4h ; vmxoff
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    add rsp, 10h
    pop rbp
    ret
VmxOff ENDP

PUBLIC VmxClear
VmxClear PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    mov qword ptr [rsp], rcx
    db 066h, 00Fh, 0C7h, 034h, 024h ; 66 0F C7 /6: vmclear [rsp]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    add rsp, 10h
    pop rbp
    ret
VmxClear ENDP

PUBLIC VmxPtrLoad
VmxPtrLoad PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    mov qword ptr [rsp], rcx
    db 00Fh, 0C7h, 034h, 024h ; NP 0F C7 /6: vmptrld [rsp]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    add rsp, 10h
    pop rbp
    ret
VmxPtrLoad ENDP

PUBLIC VmxRead
VmxRead PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    ; Field=RCX (ModRM.reg), result=R8 (ModRM.r/m); output pointer=RDX.
    db 041h, 00Fh, 078h, 0C8h ; NP 0F 78 /r: vmread r8, rcx
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    test eax, eax
    jnz VmxReadDone
    mov qword ptr [rdx], r8
VmxReadDone:
    add rsp, 10h
    pop rbp
    ret
VmxRead ENDP

PUBLIC VmxWrite
VmxWrite PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    ; Field=RCX (ModRM.reg), value=RDX (ModRM.r/m).
    db 00Fh, 079h, 0CAh ; NP 0F 79 /r: vmwrite rcx, rdx
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx

    add rsp, 10h
    pop rbp
    ret
VmxWrite ENDP

; ---------------------------------------------------------------------------
; guest-state capture helpers. Each is a leaf: no stack, no callees, so
; they are safe at any IRQL and from any core. Memory operands are the caller's
; nonpaged buffers. Encodings use the raw bytes because the mnemonics collide
; with the OPTION NOKEYWORD names used above.
; RFLAGS has no general-purpose-register encoding, so it is read through the
; stack; RSP is the caller's value (entry RSP + 8 for the return address).
; ---------------------------------------------------------------------------

PUBLIC HvGetEs
HvGetEs PROC
    xor eax, eax          ; clear bits 63:16 so the 32-bit return is well defined
    db 08Ch, 0C0h         ; mov ax, es   (8C /r, ModRM.reg=0)
    ret
HvGetEs ENDP

PUBLIC HvGetCs
HvGetCs PROC
    xor eax, eax
    db 08Ch, 0C8h         ; mov ax, cs   (ModRM.reg=1)
    ret
HvGetCs ENDP

PUBLIC HvGetSs
HvGetSs PROC
    xor eax, eax
    db 08Ch, 0D0h         ; mov ax, ss   (ModRM.reg=2)
    ret
HvGetSs ENDP

PUBLIC HvGetDs
HvGetDs PROC
    xor eax, eax
    db 08Ch, 0D8h         ; mov ax, ds   (ModRM.reg=3)
    ret
HvGetDs ENDP

PUBLIC HvGetFs
HvGetFs PROC
    xor eax, eax
    db 08Ch, 0E0h         ; mov ax, fs   (ModRM.reg=4)
    ret
HvGetFs ENDP

PUBLIC HvGetGs
HvGetGs PROC
    xor eax, eax
    db 08Ch, 0E8h         ; mov ax, gs   (ModRM.reg=5)
    ret
HvGetGs ENDP

PUBLIC HvGetTr
HvGetTr PROC
    xor eax, eax
    db 00Fh, 000h, 0C8h   ; str ax       (0F 00 /1, ModRM.reg=1)
    ret
HvGetTr ENDP

PUBLIC HvGetLdtr
HvGetLdtr PROC
    xor eax, eax
    db 00Fh, 000h, 0C0h   ; sldt ax      (0F 00 /0, ModRM.reg=0)
    ret
HvGetLdtr ENDP

PUBLIC HvGetRflags
HvGetRflags PROC
    pushfq
    pop rax
    ret
HvGetRflags ENDP

PUBLIC HvGetRsp
HvGetRsp PROC
    mov rax, rsp
    add rax, 8            ; undo the return address pushed by the call
    ret
HvGetRsp ENDP

; RCX = 16-byte output buffer; SGDT/SIDT store 2-byte limit then 64-bit base.
PUBLIC HvGetGdtr
HvGetGdtr PROC
    db 00Fh, 001h, 001h   ; sgdt [rcx]   (0F 01 /0)
    ret
HvGetGdtr ENDP

PUBLIC HvGetIdtr
HvGetIdtr PROC
    db 00Fh, 001h, 009h   ; sidt [rcx]   (0F 01 /1)
    ret
HvGetIdtr ENDP

; ---------------------------------------------------------------------------
; entry point targets. Each is a valid canonical nonpaged executable
; address in this image (SDM Vol. 3C 26.2.2 host RIP, 26.3.1.1 guest RIP).
; VMCS population stores these addresses; a guest VMCALL selects the exit
; path. These stubs execute only during VM entry and exit.
; ---------------------------------------------------------------------------

PUBLIC HvGuestEntryStub
HvGuestEntryStub PROC
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64: valid CET IBT entry target
    mov eax, 0DEADBEEFh     ; guest RAX = test magic (zero-extended)
    db 00Fh, 001h, 0C1h     ; vmcall: hand control back to the root
    ud2                     ; unreachable if the VMCALL is handled correctly
HvGuestEntryStub ENDP

 ; HOST_RSP is 16-byte aligned, with a PER_CPU_DATA pointer in its first slot.
; No interrupts or logging until the launcher's saved Windows state is restored.
; VmExitDispatch returns 0 to request VMRESUME, otherwise the launcher RSP for
; the ACTION_EXIT path. Guest GPRs (SDM Vol. 3C 27.1: not saved into the VMCS)
; are saved to Cpu->GuestRegs on entry and restored before VMRESUME.
PUBLIC HvHostEntryStub
HvHostEntryStub PROC
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64: host CET tracker may require it

    ; Save guest GPRs to Cpu->GuestRegs. R10/R11 are scratch (their guest values
    ; are parked on the host stack first).
    push r10
    push r11
    mov r11, [rsp + 16]        ; r11 = PER_CPU_DATA (first host-stack slot)
    mov r10d, g_GuestRegsOffset
    add r11, r10               ; r11 = &Cpu->GuestRegs
    mov [r11 + 0],   rax
    mov [r11 + 8],   rcx
    mov [r11 + 16],  rdx
    mov [r11 + 24],  rbx
    mov [r11 + 32],  rbp
    mov [r11 + 40],  rsi
    mov [r11 + 48],  rdi
    mov [r11 + 56],  r8
    mov [r11 + 64],  r9
    mov [r11 + 88],  r12
    mov [r11 + 96],  r13
    mov [r11 + 104], r14
    mov [r11 + 112], r15
    mov rax, [rsp + 8]         ; guest R10 (pushed first)
    mov [r11 + 72],  rax
    mov rax, [rsp]             ; guest R11 (pushed second)
    mov [r11 + 80],  rax
    add rsp, 16                ; drop the two pushes

    ; Call VmExitDispatch(Cpu, GuestRax). r11 still = &Cpu->GuestRegs.
    mov rdx, [r11 + 0]         ; rdx = guest RAX
    mov rcx, [rsp]             ; rcx = PER_CPU_DATA
    sub rsp, 240h
    fxsave64 [rsp+20h]
    mov r10, cr2
    mov [rsp+220h], r10
    mov r10, dr6
    mov [rsp+228h], r10
    call VmExitDispatch        ; rax = 0 => VMRESUME, 1 => shutdown, else launcher RSP
    fxrstor64 [rsp+20h]
    mov r10, [rsp+220h]
    mov cr2, r10
    mov r10, [rsp+228h]
    mov dr6, r10
    test rax, rax
    jz ResumeGuest
    mov rsp, rax
    jmp LaunchRestore
ResumeGuest:
    add rsp, 240h              ; rsp = HOST_RSP
    ; Restore guest GPRs from Cpu->GuestRegs.
    mov r11, [rsp]             ; r11 = PER_CPU_DATA
    mov r10d, g_GuestRegsOffset
    add r11, r10               ; r11 = &Cpu->GuestRegs
    mov rax, [r11 + 0]
    mov rcx, [r11 + 8]
    mov rdx, [r11 + 16]
    mov rbx, [r11 + 24]
    mov rbp, [r11 + 32]
    mov rsi, [r11 + 40]
    mov rdi, [r11 + 48]
    mov r8,  [r11 + 56]
    mov r9,  [r11 + 64]
    mov r12, [r11 + 88]
    mov r13, [r11 + 96]
    mov r14, [r11 + 104]
    mov r15, [r11 + 112]
    mov r10, [r11 + 72]        ; guest R10
    mov r11, [r11 + 80]        ; guest R11
    db 00Fh, 001h, 0C3h        ; VMRESUME
    ; VMRESUME returns here only on VMfail (CF/ZF set).
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx                ; eax = 0/1/2 status
    mov edx, eax
    mov rcx, [rsp]
    sub rsp, 20h
    call VmExitResumeFailure
    mov rsp, rax
    jmp LaunchRestore
HvHostEntryStub ENDP

PUBLIC VmcsLaunchOnCurrentCoreTest
; Frame offsets: status 00, RFLAGS 08, GDTR 10, IDTR 20, selectors 30..3A,
; FS/GS base 40/48, DEBUGCTL 50, DR7 58, CR2 68, CR0 80; GPRs at 88.
; Guest touches only RAX. C follows the ABI and preserves nonvolatile XMM state.
VmcsLaunchOnCurrentCoreTest PROC FRAME
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    mov [rcx], rsp
    mov [rsp+70h], edx     ; TransferCet
    mov [rsp+78h], r8d     ; Persistent mode: private root CET policy
    pushfq
    pop rax
    mov [rsp+08h], rax
    cli
    sgdt fword ptr [rsp+10h]
    sidt fword ptr [rsp+20h]
    mov word ptr [rsp+30h], es
    mov word ptr [rsp+32h], ds
    mov word ptr [rsp+34h], fs
    mov word ptr [rsp+36h], gs
    mov word ptr [rsp+38h], ss
    sldt word ptr [rsp+3Ah]
    mov ecx, 0C0000100h
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsp+40h], rax
    mov ecx, 0C0000101h
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsp+48h], rax
    mov ecx, 1D9h
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsp+50h], rax
    mov rax, dr7
    mov [rsp+58h], rax
    mov rax, dr6
    mov [rsp+60h], rax
    mov rax, cr2
    mov [rsp+68h], rax
    mov rax, cr0
    mov [rsp+80h], rax

    ; SDM 25.7/25.8, 27.3.2 and 28.5: transfer the live CET triplet.
    ; No CALL here: SSP must still reference the launcher's caller return address.
    ; Guest executes no CALL/RET; host C CALL/RET balances this same shadow stack.
    cmp dword ptr [rsp+70h], 0
    je CetPrepared
    mov ecx, 6A2h           ; IA32_S_CET (SDM Vol. 4)
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov r8, rax
    mov ecx, 6828h
    mov rdx, r8
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    mov ecx, 6C18h
    mov rdx, r8
    cmp dword ptr [rsp+78h], 0
    je HostCet6C18h
    xor edx, edx
HostCet6C18h:
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    mov ecx, 6A8h           ; IA32_INTERRUPT_SSP_TABLE_ADDR
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov r8, rax
    mov ecx, 682Ch
    mov rdx, r8
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    mov ecx, 6C1Ch
    mov rdx, r8
    cmp dword ptr [rsp+78h], 0
    je HostCet6C1Ch
    xor edx, edx
HostCet6C1Ch:
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    xor eax, eax
    ; F3 REX.W 0F 1E /1, ModRM=C8: RDSSPQ RAX.
    ; When supervisor shadow stacks are disabled this is a NOP, leaving zero.
    db 0F3h, 048h, 00Fh, 01Eh, 0C8h
    mov r8, rax
    mov ecx, 682Ah
    mov rdx, r8
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    mov ecx, 6C1Ah
    mov rdx, r8
    cmp dword ptr [rsp+78h], 0
    je HostCet6C1Ah
    xor edx, edx
HostCet6C1Ah:
    db 00Fh, 079h, 0CAh     ; VMWRITE field=RCX, value=RDX
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
CetPrepared:

    ; A valid stack in the current address space, not a stale C helper's RSP.
    mov ecx, 681Ch
    mov rdx, rsp
    db 00Fh, 079h, 0CAh
    pushfq
    pop rax
    test eax, 41h
    jnz LaunchFailed
    db 00Fh, 001h, 0C2h
    pushfq
    pop rax
LaunchFailed:
    mov rdx, rax
    and eax, 1
    shl eax, 1
    shr rdx, 6
    and edx, 1
    or eax, edx
    mov [rsp], rax

LaunchRestore::
    lgdt fword ptr [rsp+10h]
    lidt fword ptr [rsp+20h]
    mov es, word ptr [rsp+30h]
    mov ds, word ptr [rsp+32h]
    mov fs, word ptr [rsp+34h]
    mov gs, word ptr [rsp+36h]
    mov ss, word ptr [rsp+38h]
    lldt word ptr [rsp+3Ah]
    mov rax, [rsp+40h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0C0000100h
    wrmsr
    mov rax, [rsp+48h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0C0000101h
    wrmsr
    mov rax, [rsp+50h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 1D9h
    wrmsr
    mov rax, [rsp+58h]
    mov dr7, rax
    mov rax, [rsp+60h]
    mov dr6, rax
    mov rax, [rsp+68h]
    mov cr2, rax
    ; One-shot VM exits use a TS-cleared host CR0 for FXSAVE/FXRSTOR.
    ; Restore the caller on success and failure, before restoring interrupts.
    mov rax, [rsp+80h]
    mov cr0, rax
    mov eax, dword ptr [rsp]
    push qword ptr [rsp+08h]
    popfq
    lea rsp, [rsp+88h]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
VmcsLaunchOnCurrentCoreTest ENDP

; SDM 31.3 INVEPT: 66 0F 38 80 /r; RCX=type, [RSP]=128-bit descriptor.
; Resident leaf wrappers. The aligned 16-byte descriptor stays on the kernel
; stack; RBP is preserved, and no allocation/call/blocking operation is used.
PUBLIC EptInveptSingleContext
EptInveptSingleContext PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    mov qword ptr [rsp], rcx
    mov qword ptr [rsp+8], 0
    mov ecx, 1
    db 066h, 00Fh, 038h, 080h, 00Ch, 024h ; INVEPT RCX, oword ptr [RSP]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx
    add rsp, 10h
    pop rbp
    ret
EptInveptSingleContext ENDP

PUBLIC EptInveptAllContexts
EptInveptAllContexts PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    mov qword ptr [rsp], 0
    mov qword ptr [rsp+8], 0
    mov ecx, 2
    db 066h, 00Fh, 038h, 080h, 00Ch, 024h ; INVEPT RCX, oword ptr [RSP]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx
    add rsp, 10h
    pop rbp
    ret
EptInveptAllContexts ENDP

; INVVPID type in ECX, VPID in DX. A zeroed descriptor prevents reserved-bit
; faults. Type 1 includes global translations; type 2 uses a zero VPID.
PUBLIC VpidInvvpid
VpidInvvpid PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 10h
    .allocstack 10h
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    movzx edx, dx
    mov qword ptr [rsp], rdx
    mov qword ptr [rsp+8], 0
    db 066h, 00Fh, 038h, 081h, 00Ch, 024h ; INVVPID RCX, oword ptr [RSP]
    pushfq
    pop rax
    mov rcx, rax
    and eax, 1
    shl eax, 1
    shr rcx, 6
    and ecx, 1
    or eax, ecx
    add rsp, 10h
    pop rbp
    ret
VpidInvvpid ENDP

; ---------------------------------------------------------------------------
; self-hook handler. Reached by the shadow page's 12-byte absolute jump
; (mov rax,imm64; jmp rax); jmp rax is an indirect branch, so this entry must
; begin with ENDBR64 for CET IBT. It sets the fired magic and VMCALLs directly:
; continuing into HvGuestEntryStub via the trampoline would re-run
; "mov eax,0DEADBEEFh" and clobber the magic before the VMCALL.
; ---------------------------------------------------------------------------
PUBLIC MyHookHandlerVa
MyHookHandlerVa PROC
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64: valid CET IBT entry target
    mov eax, 0CAFEBABEh       ; magic: the hook fired
    db 00Fh, 001h, 0C1h       ; vmcall: return control to the root
    ud2                       ; unreachable if the VMCALL is handled
MyHookHandlerVa ENDP

; ---------------------------------------------------------------------------
; guest read probe. Guest entry that reads the first 8 bytes of the
; hooked function into RAX, then VMCALLs. When the hook is installed the read
; faults on the R=0 shadow leaf and is emulated (original bytes returned); when
; the hook is removed the read is trivially original. ENDBR64 preamble matches
; the other guest stubs.
; ---------------------------------------------------------------------------
PUBLIC HvGuestReadProbe
HvGuestReadProbe PROC
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64
    mov rax, offset HvGuestEntryStub ; rax = address of the hooked function
    mov rax, [rax]                   ; rax = first 8 bytes AT that address
    db 00Fh, 001h, 0C1h              ; vmcall
    ud2
HvGuestReadProbe ENDP

; ---------------------------------------------------------------------------
; guest CPUID probe. Executes three CPUIDs (each serviced + spoofed by the
; root handler) and stores the results into g_CpuidProbe, then VMCALLs. R8 holds
; the buffer base and is preserved by the CPUID handler's GPR save/restore.
; ---------------------------------------------------------------------------
PUBLIC HvGuestCpuidProbe
HvGuestCpuidProbe PROC
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64
    lea r8, g_CpuidProbe
    mov eax, 1                ; leaf 1
    xor ecx, ecx
    db 00Fh, 0A2h             ; cpuid
    mov [r8 + 0], rcx         ; leaf-1 ECX (bit 31 test)
    mov eax, 40000000h        ; leaf 0x40000000
    xor ecx, ecx
    db 00Fh, 0A2h             ; cpuid
    mov [r8 + 8],  rbx        ; EBX
    mov [r8 + 16], rdx        ; EDX
    mov [r8 + 24], rcx        ; ECX
    mov eax, 40000001h        ; leaf 0x40000001
    xor ecx, ecx
    db 00Fh, 0A2h             ; cpuid
    mov [r8 + 32], rax        ; EAX (interface signature)
    mov eax, 0DEADBEEFh       ; done marker
    db 00Fh, 001h, 0C1h       ; vmcall
    ud2
HvGuestCpuidProbe ENDP

; ---------------------------------------------------------------------------
; persistent launch. Called once per core by PersistLaunchAllCores.
;   rcx = &Cpu->LaunchCallerRsp (slot to save RSP for the failure path).
; On VMLAUNCH success the guest resumes at HvPersistentGuestEntry (GUEST_RIP is
; set by C to this label); on failure returns 0/1/2 (VMfail) to the C caller.
; The guest RSP is written here (= current RSP) so the continuation "ret" pops
; the return address pushed by the CALL to this function.
; ---------------------------------------------------------------------------
PUBLIC HvPersistentGuestEntry
HvPersistentGuestEntry PROC
    db 0F3h, 00Fh, 01Eh, 0FAh
    mov qword ptr [rsp], 0
    jmp LaunchRestore
HvPersistentGuestEntry ENDP

PUBLIC HvHookControl
PUBLIC HvHookCpuid
HvHookControl PROC
    db 0F3h, 00Fh, 01Eh, 0FAh
    push rbx
    mov eax, 4D794856h
HvHookCpuid::
    cpuid
    pop rbx
    ret
HvHookControl ENDP

PUBLIC HvPersistentShutdown
PUBLIC HvShutdownCpuid
PUBLIC HvShutdownVmcall
PUBLIC HvShutdownReturn
HvPersistentShutdown PROC
    db 0F3h, 00Fh, 01Eh, 0FAh
    pushfq
    cli
    push rbx
    mov eax, 4D794856h
    mov ecx, 80000001h
HvShutdownCpuid::
    cpuid
HvShutdownVmcall::
    db 00Fh, 001h, 0C1h
HvShutdownReturn::
    db 0F3h, 00Fh, 01Eh, 0FAh
    pop rbx
    popfq
    ret
HvPersistentShutdown ENDP

; Entered by a second VM exit with the guest CET triplet and CR3 already loaded.
; No CALL/RET until the original Windows continuation has been restored.
PUBLIC HvNativeExit
HvNativeExit PROC
    db 0F3h, 00Fh, 01Eh, 0FAh
    mov ecx, 4402h
    db 00Fh, 078h, 0C8h ; VMREAD RAX, RCX
    cmp eax, 18
    jne NativeFailed
    db 00Fh, 001h, 0C4h ; VMXOFF
    jbe NativeFailed
    mov r11, [rsp]
    mov r10d, g_NativeOffset
    add r11, r10
    lgdt fword ptr [r11+50h]
    lidt fword ptr [r11+60h]
    mov es, word ptr [r11+70h]
    mov ds, word ptr [r11+72h]
    mov fs, word ptr [r11+74h]
    mov gs, word ptr [r11+76h]
    mov ss, word ptr [r11+78h]
    lldt word ptr [r11+7Ah]
    mov rax, [r11+18h]
    mov cr0, rax
    mov rax, [r11+28h]
    mov cr4, rax
    mov rax, [r11+30h]
    mov dr7, rax
    mov rax, [r11+38h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 1D9h
    wrmsr
    mov rax, [r11+40h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0C0000100h
    wrmsr
    mov rax, [r11+48h]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0C0000101h
    wrmsr
    mov rcx, [r11+00h]
    mov rdx, [r11+08h]
    mov rax, [r11+10h]
    mov r11, [rsp]
    mov r10d, g_GuestRegsOffset
    add r11, r10
    mov r10, [r11+72]
    mov r11, [r11+80]
    mov rsp, rdx
    push rax
    popfq
    jmp rcx
NativeFailed:
    sub rsp, 20h
    call PersistRootFault
    ud2
HvNativeExit ENDP

; Root uses its own IDT, CET disabled. Save ALL registers that C may clobber;
; hardware aligns the interrupt stack, so the FX area remains 16-byte aligned.
PUBLIC HvRootNmi
HvRootNmi PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 220h
    fxsave64 [rsp+20h]
    call PersistRootNmi
    fxrstor64 [rsp+20h]
    add rsp, 220h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    iretq
HvRootNmi ENDP

PUBLIC HvRootFault
HvRootFault PROC
    and rsp, -16
    sub rsp, 20h
    call PersistRootFault
    ud2
HvRootFault ENDP
; guest-only filter. Root redirects before the original entry instruction.
; Entry S % 16 == 8 is checked in root. No CLI around the C helper: Windows
; must be able to service user-memory page faults. Its original return address
; and the CET shadow-stack return remain untouched throughout this frame.
;
; S -> pushfq: S-8 (0 mod 16) -> sub D0h: S-D8h (0 mod 16).
; [rsp+00..1F] home space; [20..7F] volatile XMM; [80..B7] volatile GPR.
; CALL pushes another 8 bytes, so C entry is 8 mod 16. Both paths restore S.
FILTER_RESTORE MACRO
    movdqu xmm0, xmmword ptr [rsp+20h]
    movdqu xmm1, xmmword ptr [rsp+30h]
    movdqu xmm2, xmmword ptr [rsp+40h]
    movdqu xmm3, xmmword ptr [rsp+50h]
    movdqu xmm4, xmmword ptr [rsp+60h]
    movdqu xmm5, xmmword ptr [rsp+70h]
    mov rax, [rsp+80h]
    mov rcx, [rsp+88h]
    mov rdx, [rsp+90h]
    mov r8,  [rsp+98h]
    mov r9,  [rsp+0A0h]
    mov r10, [rsp+0A8h]
    mov r11, [rsp+0B0h]
    add rsp, 0D0h
    popfq
ENDM

FILTER_ENTRY MACRO idx
LOCAL FilterGuestBlocked
PUBLIC HvFilterGuestEntry_&idx
PUBLIC HvFilterContinue_&idx
PUBLIC HvFilterBlock_&idx
HvFilterGuestEntry_&idx PROC FRAME
    db 0F3h, 00Fh, 01Eh, 0FAh ; ENDBR64
    pushfq
    .allocstack 8
    sub rsp, 0D0h
    .allocstack 0D0h
    .endprolog
    mov [rsp+80h], rax
    mov [rsp+88h], rcx
    mov [rsp+90h], rdx
    mov [rsp+98h], r8
    mov [rsp+0A0h], r9
    mov [rsp+0A8h], r10
    mov [rsp+0B0h], r11
    movdqu xmmword ptr [rsp+20h], xmm0
    movdqu xmmword ptr [rsp+30h], xmm1
    movdqu xmmword ptr [rsp+40h], xmm2
    movdqu xmmword ptr [rsp+50h], xmm3
    movdqu xmmword ptr [rsp+60h], xmm4
    movdqu xmmword ptr [rsp+70h], xmm5
    mov ecx, idx
    lea rdx, [rsp+80h]            ; FILTER_ARGUMENTS snapshot
    lea r8, [rsp+0D8h]            ; original entry RSP, for argument 5
    IF ((idx GE 13) AND (idx LT 24)) OR (idx EQ 25)
        call KernelCapture
    ELSE
        call FilterDecide
    ENDIF
    cmp eax, 1                    ; FILTER_ACTION_BLOCK
    je FilterGuestBlocked
    FILTER_RESTORE
HvFilterContinue_&idx::
    db 00Fh, 001h, 0C1h           ; Root replays original instruction; no RET here.
    ud2                          ; VMX remains alive while this reference exists.
FilterGuestBlocked:
    FILTER_RESTORE
HvFilterBlock_&idx::
    db 00Fh, 001h, 0C1h           ; Root sets status and retires the real RET via MTF.
    ud2
HvFilterGuestEntry_&idx ENDP

ENDM

FILTER_ENTRY 0
FILTER_ENTRY 1
FILTER_ENTRY 2
FILTER_ENTRY 3
FILTER_ENTRY 4
FILTER_ENTRY 5
FILTER_ENTRY 6
FILTER_ENTRY 7
FILTER_ENTRY 8
FILTER_ENTRY 10
FILTER_ENTRY 12
FILTER_ENTRY 13
FILTER_ENTRY 14
FILTER_ENTRY 15
FILTER_ENTRY 16
FILTER_ENTRY 17
FILTER_ENTRY 18
FILTER_ENTRY 19
FILTER_ENTRY 20
FILTER_ENTRY 21
FILTER_ENTRY 22
FILTER_ENTRY 23
FILTER_ENTRY 24
FILTER_ENTRY 25
PUBLIC HvFilterReturn

; Root enters directly here with the original S and STATUS_ACCESS_DENIED in EAX.
; This real RET updates CET SSP in hardware. No synthetic return address or
; manual shadow-stack adjustment. IF is restored by the immediate MTF exit.
HvFilterReturn PROC
    ret
HvFilterReturn ENDP

END
