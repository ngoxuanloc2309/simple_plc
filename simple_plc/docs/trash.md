D:\Synaptix\SynaptiX\strategy\meshwf>arm-none-eabi-addr2line.exe -e build/release/RS485_IO_RF_V2.elf -f -C -i 0x080077B0 0x080077AC
xQueueGenericSend
??:?
xQueueGenericSend
??:?

D:\Synaptix\SynaptiX\strategy\meshwf>arm-none-eabi-objdump.exe -d -C --start-address=0x08007790 --stop-address=0x080077C0 build/release/RS485_IO_RF_V2.elf

build/release/RS485_IO_RF_V2.elf:     file format elf32-littlearm


Disassembly of section .text:

08007790 <xQueueGenericCreate+0x46>:
 8007790:       e8bd 81f0       ldmia.w sp!, {r4, r5, r6, r7, r8, pc}
 8007794:       4603            mov     r3, r0
 8007796:       e7f1            b.n     800777c <xQueueGenericCreate+0x32>
 8007798:       f001 fe84       bl      80094a4 <ulSetInterruptMask>
 800779c:       e7fe            b.n     800779c <xQueueGenericCreate+0x52>

0800779e <xQueueGenericSend>:
 800779e:       e92d 41ff       stmdb   sp!, {r0, r1, r2, r3, r4, r5, r6, r7, r8, lr}
 80077a2:       460f            mov     r7, r1
 80077a4:       461e            mov     r6, r3
 80077a6:       4604            mov     r4, r0
 80077a8:       9201            str     r2, [sp, #4]
 80077aa:       b910            cbnz    r0, 80077b2 <xQueueGenericSend+0x14>
 80077ac:       f001 fe7a       bl      80094a4 <ulSetInterruptMask>
 80077b0:       e7fe            b.n     80077b0 <xQueueGenericSend+0x12>
 80077b2:       b921            cbnz    r1, 80077be <xQueueGenericSend+0x20>
 80077b4:       6c03            ldr     r3, [r0, #64]   @ 0x40
 80077b6:       b113            cbz     r3, 80077be <xQueueGenericSend+0x20>
 80077b8:       f001 fe74       bl      80094a4 <ulSetInterruptMask>
 80077bc:       e7fe            b.n     80077bc <xQueueGenericSend+0x1e>
 80077be:       2e02            cmp     r6, #2

D:\Synaptix\SynaptiX\strategy\meshwf>arm-none-eabi-nm.exe build/release/RS485_IO_RF_V2.elf | findstr pxCurrentTCB
20030434 B pxCurrentTCB
08009520 t pxCurrentTCBConst
08009470 t pxCurrentTCBConst2

D:\Synaptix\SynaptiX\strategy\meshwf>