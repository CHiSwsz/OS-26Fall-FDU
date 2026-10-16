#include <aarch64/intrinsic.h>
#include <driver/uart.h>
#include <kernel/printk.h>
#include <kernel/core.h>
#include <common/string.h>

extern char edata[], end[];

static volatile bool boot_secondary_cpus = false;

void main()
{
    if (cpuid() == 0) {
        // Clear BSS and any padding after .data before starting other cores.
        memset(edata, 0, (usize)end - (usize)edata);

        smp_init();
        uart_init();
        printk_init();

        printk("Hello, world! (Core 0)\n");

        arch_fence();

        // Set a flag indicating that the secondary CPUs can start executing.
        boot_secondary_cpus = true;
    } else {
        while (!boot_secondary_cpus)
            ;
        arch_fence();

        printk("Hello, world! (Core %d)\n", (int)cpuid());
    }

    set_return_addr(idle_entry);
}
