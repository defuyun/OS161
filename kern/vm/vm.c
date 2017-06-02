#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>


void vm_bootstrap(void) {
        init_ft_hpt();
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
        struct addrspace * as;
        as = proc_getas();

        if (as == NULL || faulttype == VM_FAULT_READONLY ||
            faultaddress >= MIPS_KSEG0) {

                return EFAULT;
        }

        spinlock_acquire(&hpt_lock);

        if (hpt == NULL) {
                spinlock_release(&hpt_lock);
                return EFAULT;
        }

        vaddr_t vpn = faultaddress & PAGE_FRAME;
        uint32_t pid = (uint32_t) as;
        int index = hpt_hash(as, vpn);
        bool found = false;

        while (index != NO_NEXT_PAGE && hpt[index].inuse) {

                if ((hpt[index].entry_hi & PAGE_FRAME) == vpn &&
                    hpt[index].pid == pid && hpt[index].inuse) {

                        found = true;
                        break;

                } else {
                        index = hpt[index].next;
                }
        }

        while (!found && index != NO_NEXT_PAGE && hpt[index].inuse) {

                if ((hpt[index].entry_hi & PAGE_FRAME) == vpn &&
                    hpt[index].pid == pid && hpt[index].inuse) {

                        found = true;
                        break;
                } else {
                        index = hpt[index].prev;
                }
        }

        if (!found) {
                spinlock_release(&hpt_lock);
                return EFAULT;
        }

        uint32_t entry_lo = hpt[index].entry_lo;

        if ((faulttype == VM_FAULT_READ && !(entry_lo & HPTABLE_READ)) ||
            (faulttype == VM_FAULT_WRITE &&
            !(entry_lo & (HPTABLE_WRITE | HPTABLE_SWRITE)))) {

                spinlock_release(&hpt_lock);
                return EFAULT;
        }

        if ((entry_lo & PAGE_FRAME) == 0) {
                int result = allocate_memory(index);
                if (result) {
                        spinlock_release(&hpt_lock);
                        return ENOMEM;
                }
        }

        entry_lo = hpt[index].entry_lo;
        entry_lo &= ~HPTABLE_STATEBITS;
        if (faulttype == VM_FAULT_WRITE) {
                entry_lo |= (1 << HPTABLE_DIRTY);
        }

        spinlock_release(&hpt_lock);

        int spl = splhigh();
        tlb_random(vpn, KVADDR_TO_PADDR(entry_lo));
        splx(spl);
        return 0;
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
        (void)ts;
        panic("vm tried to do tlb shootdown?!\n");
}