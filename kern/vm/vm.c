#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>


/* Place your page table functions here */

void vm_bootstrap(void)
{
    init_frame_and_page_table();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    struct addrspace * as;
    int spl;

    as = proc_getas();
    if(as == NULL) {
        return EFAULT;
    }

    if(faulttype == VM_FAULT_READONLY) {
        return EINVAL;
    }

    if(faultaddress >= MIPS_KSEG0) {
        return EFAULT;
    }

    vaddr_t vpn = faultaddress & PAGE_FRAME;
    uint32_t pid = (uint32_t) as;
    int index = hpt_hash(as, vpn);
    bool found = false;

    spinlock_acquire(&hash_page_table_lock);

    if(hash_page_table == NULL) {
        spinlock_release(&hash_page_table_lock);
        return EFAULT;
    }

    while(index != NO_NEXT_PAGE && hash_page_table[index].inuse) {
        if((hash_page_table[index].entry_hi & PAGE_FRAME) == vpn && hash_page_table[index].pid == pid) {
            found = true;
            break;
        } else {
            index = hash_page_table[index].next;
        }
    }

    uint32_t entry_hi = vpn;
    uint32_t entry_lo;

    if(found) {
        entry_lo = hash_page_table[index].entry_lo;

        if((faulttype == VM_FAULT_READ && !(entry_lo & HPTABLE_READ)) ||
           (faulttype == VM_FAULT_WRITE && !(entry_lo & (HPTABLE_WRITE | HPTABLE_SWRITE)))) {
            spinlock_release(&hash_page_table_lock);
            return EFAULT;
        } else {
            if((entry_lo & PAGE_FRAME) == 0) {
                int result = allocate_memory(index);
                if(result) {
                    spinlock_release(&hash_page_table_lock);
                    return ENOMEM;
                }
            }

            entry_lo = hash_page_table[index].entry_lo;
            entry_lo &= ~HPTABLE_STATEBITS;
            if(faulttype == VM_FAULT_WRITE) {
                // we need to reset dirty bit because it might be a have soft write set
                entry_lo |= (1 << HPTABLE_DIRTY);
            }
        }
    } else {
        // not sure what to do when not found in page table, what does it mean to check valid region
        spinlock_release(&hash_page_table_lock);
        return EFAULT;
    }

    spinlock_release(&hash_page_table_lock);

    spl = splhigh();
    tlb_random(KVADDR_TO_PADDR(entry_hi), entry_lo);
    splx(spl);
    return 0;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
    (void)ts;
    panic("vm tried to do tlb shootdown?!\n");
}

