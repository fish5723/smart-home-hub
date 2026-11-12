/**
 * @file startup_stm32f103xb_boot.s
 * @brief F103 bootloader 专用最小启动文件（无 SystemInit / 无 libc 初始化）
 *
 * 与 app 的 startup 区别：
 *   - 不调用 SystemInit（时钟由 boot_clock_init() 在 main 中配置）
 *   - 不调用 __libc_init_array（bootloader 无静态构造器）
 *   - 只保留 16 个系统向量 + 60 个 IRQ 向量（F103 medium-density）
 */
  .syntax unified
  .cpu cortex-m3
  .fpu softvfp
  .thumb

  .global g_pfnVectors
  .global Reset_Handler
  .global Default_Handler

  .section .isr_vector,"a",%progbits
  .type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .rept 14
  .word Default_Handler
  .endr
  /* F103 medium-density：60 个 IRQ 向量 */
  .rept 60
  .word Default_Handler
  .endr
  .size g_pfnVectors, .-g_pfnVectors

  .section .text.Reset_Handler,"ax",%progbits
  .type Reset_Handler, %function
Reset_Handler:
  /* 复制 .data（含 .RamFunc）到 RAM */
  ldr r0, =_sdata
  ldr r1, =_edata
  ldr r2, =_sidata
  movs r3, #0
  b LoopCopyDataInit

CopyDataInit:
  ldr r4, [r2, r3]
  str r4, [r0, r3]
  adds r3, r3, #4

LoopCopyDataInit:
  adds r4, r0, r3
  cmp r4, r1
  bcc CopyDataInit

  /* 清零 .bss */
  ldr r2, =_sbss
  ldr r4, =_ebss
  movs r3, #0
  b LoopFillZerobss

FillZerobss:
  str r3, [r2]
  adds r2, r2, #4

LoopFillZerobss:
  cmp r2, r4
  bcc FillZerobss

  bl main
  b .

  .size Reset_Handler, .-Reset_Handler

  .section .text.Default_Handler,"ax",%progbits
  .type Default_Handler, %function
Default_Handler:
  b .
  .size Default_Handler, .-Default_Handler
