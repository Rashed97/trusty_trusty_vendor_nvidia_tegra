/*
 * Copyright (c) 2015 Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <debug.h>
#include <dev/interrupt/arm_gic.h>
#include <dev/timer/arm_generic.h>
#include <kernel/vm.h>
#include <lk/init.h>
#include <platform/gic.h>
#include <platform/memmap.h>
#include <platform/tegra_debug.h>
#include <string.h>

#include "smc.h"

#define ARM_GENERIC_TIMER_INT_CNTV 27
#define ARM_GENERIC_TIMER_INT_CNTPS 29
#define ARM_GENERIC_TIMER_INT_CNTP 30

#define ARM_GENERIC_TIMER_INT_SELECTED(timer) ARM_GENERIC_TIMER_INT_##timer
#define XARM_GENERIC_TIMER_INT_SELECTED(timer) \
    ARM_GENERIC_TIMER_INT_SELECTED(timer)
#define ARM_GENERIC_TIMER_INT \
    XARM_GENERIC_TIMER_INT_SELECTED(TIMER_ARM_GENERIC_SELECTED)

/* initial memory mappings. parsed by start.S */
struct mmu_initial_mapping mmu_initial_mappings[] = {
        /* Mark next entry as dynamic as it might be updated
           by platform_reset code to specify actual size and
           location of RAM to use */
        {.phys = MEMBASE + KERNEL_LOAD_OFFSET,
         .virt = KERNEL_BASE + KERNEL_LOAD_OFFSET,
         .size = MEMSIZE,
         .flags = MMU_INITIAL_MAPPING_FLAG_DYNAMIC,
         .name = "ram"},

        /* This is the physical UART device mapping */
        {.phys = TEGRA_UARTA_BASE,
         .virt = KERNEL_BASE + KERNEL_LOAD_OFFSET + TEGRA_UARTA_BASE,
         .size = TEGRA_UARTA_SIZE,
         .flags = MMU_INITIAL_MAPPING_FLAG_DEVICE,
         .name = "uart" },

        /* null entry to terminate the list */
        {0, 0, 0, 0, 0}};

static pmm_arena_t ram_arena = {.name = "ram",
                                .base = MEMBASE + KERNEL_LOAD_OFFSET,
                                .size = MEMSIZE,
                                .flags = PMM_ARENA_FLAG_KMAP};

uint32_t debug_uart_id = DEFAULT_DEBUG_PORT;

void platform_early_init(void)
{
       	platform_init_debug_port(debug_uart_id);
}

void platform_init_mmu_mappings(void) {
    /* go through mmu_initial_mapping to find dynamic entry
     * matching ram_arena (by name) and adjust it.
     */
    struct mmu_initial_mapping* m = mmu_initial_mappings;
    for (uint i = 0; i < countof(mmu_initial_mappings); i++, m++) {
        if (!(m->flags & MMU_INITIAL_MAPPING_FLAG_DYNAMIC))
            continue;

        if (strcmp(m->name, ram_arena.name) == 0) {
            /* update ram_arena */
            ram_arena.base = m->phys;
            ram_arena.size = m->size;
            ram_arena.flags = PMM_ARENA_FLAG_KMAP;

            break;
        }
    }
    pmm_add_arena(&ram_arena);
}

static void tegra_map_regs(const char* name,
                                   vaddr_t vaddr,
                                   paddr_t paddr,
                                   size_t size) {
    status_t ret;
    void* vaddrp = (void*)vaddr;

    ret = vmm_alloc_physical(vmm_get_kernel_aspace(), "gic", GICC_SIZE, &vaddrp,
                             0, paddr, VMM_FLAG_VALLOC_SPECIFIC,
                             ARCH_MMU_FLAG_UNCACHED_DEVICE);
    if (ret) {
        dprintf(CRITICAL, "%s: failed %d\n", __func__, ret);
    }
}

static paddr_t tegra_get_reg_base(int reg) {
#if ARCH_ARM64
    return tegra_smc(SMC_FC64_GET_REG_BASE, reg, 0, 0);
#else
    return tegra_smc(SMC_FC_GET_REG_BASE, reg, 0, 0);
#endif
}

static void platform_during_vm_init(uint level) {
    paddr_t paddr = ROUNDDOWN(TEGRA_UARTA_BASE, PAGE_SIZE);
    void *vptr;
    status_t err = -1;

    /* Map UART port */
    err = vmm_alloc_physical(vmm_get_kernel_aspace(), "uart",
                             PAGE_SIZE, &vptr, PAGE_SIZE_SHIFT, paddr,
                             0, ARCH_MMU_FLAG_UNCACHED_DEVICE);
    if (err) {
        dprintf(CRITICAL, "%s: failed to map UART port %d\n", __func__, err);
    }
}

static void platform_after_vm_init(uint level) {
    paddr_t gicc = tegra_get_reg_base(SMC_GET_GIC_BASE_GICC);
    paddr_t gicd = tegra_get_reg_base(SMC_GET_GIC_BASE_GICD);

    dprintf(INFO, "gicc 0x%lx, gicd 0x%lx\n", gicc, gicd);

    tegra_map_regs("gicc", GICC_BASE_VIRT, gicc, GICC_SIZE);
    tegra_map_regs("gicd", GICD_BASE_VIRT, gicd, GICD_SIZE);

    /* initialize the interrupt controller */
    arm_gic_init();

    /* initialize the timer block */
    arm_generic_timer_init(ARM_GENERIC_TIMER_INT, 0);
}

LK_INIT_HOOK(platform_during_vm, platform_during_vm_init, LK_INIT_LEVEL_VM + 1);
LK_INIT_HOOK(platform_after_vm, platform_after_vm_init, LK_INIT_LEVEL_VM + 2);
