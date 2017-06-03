/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *        The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/* takes in an address space address and an entryhi, returns a hashed index
 * within the HPT */
uint32_t hpt_hash(struct addrspace *as, vaddr_t vpn) {
        uint32_t index;
        index = (((uint32_t )as) ^ (vpn >> PAGE_BITS)) % hpt_size;
        return index;
}



/* takes in an address space address and an entryhi, returns the double hash
 * value, which is the jump distance during linear probing */
uint32_t hpt_double_hash(struct addrspace *as, vaddr_t vpn) {
        uint32_t index;
        index = (((uint32_t ) as) << 7) ^ (vpn / 17) * (vpn >> PAGE_BITS);
        index %= hpt_size;
        if (index == 0) {
                index = 23;
        }
        if (index % 2 == 0) {
                index++;
        }
        return index;
}



/* all functions that call this have to use a sync primitive
 * to ensure mutex on the hpt. */
static bool insert_page_table_entry(struct addrspace *as, uint32_t entry_hi,
                                    uint32_t entry_lo) {

        uint32_t vpn = entry_hi & PAGE_FRAME;
        int index = hpt_hash(as, vpn);
        int jump_dist = hpt_double_hash(as, vpn);

        int count = 0;
        while (hpt[index].inuse && count < hpt_size) {
                index += jump_dist;
                index %= hpt_size;
                count++;
        }

        if (count == hpt_size) {
                return false;
        }

        hpt[index].pid = (uint32_t) as;
        hpt[index].entry_hi = vpn;
        hpt[index].entry_lo = entry_lo;
        hpt[index].inuse = true;

        return true;
}

static int define_memory(struct addrspace * as, vaddr_t addr, size_t memsize,
                         int permissions) {

        if (addr + memsize > MIPS_KSEG0) {
                return EFAULT;
        }

        uint32_t top = (addr + memsize + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t base = addr / PAGE_SIZE;
        paddr_t paddr = 0;

        for (uint32_t start = base; start < top; start++) {

                uint32_t entry_hi = start << FLAG_OFFSET;
                uint32_t entry_lo = (paddr & PAGE_FRAME) |
                                    (1 << HPTABLE_VALID) |
                                    (1 << HPTABLE_GLOBAL);

                if (permissions & HPTABLE_WRITE) {
                        entry_lo |= (1 << HPTABLE_DIRTY);
                } else {
                        entry_lo &= ~(1 << HPTABLE_DIRTY);
                }

                entry_lo |= permissions;
                entry_lo |= HPTABLE_SWRITE;

                spinlock_acquire(&hpt_lock);
                if (!insert_page_table_entry(as, entry_hi, entry_lo)) {

                        spinlock_release(&hpt_lock);
                        return ENOMEM;
                }

                spinlock_release(&hpt_lock);
        }

        return 0;
}


/* allocates a frame for a corresponding hash page table entry.
 * functions that call this have to use a sync primitive */
int allocate_memory(int hash_page_index) {
        vaddr_t vaddr = alloc_kpages(1);
        hpt[hash_page_index].entry_lo |= vaddr;
        if (vaddr == 0) {
                return ENOMEM;
        }
        return 0;
}

void tlb_flush(void) {
        int i, spl;
        spl = splhigh();

        for (i = 0; i < NUM_TLB; i++) {
                tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
        }

        splx(spl);
 }

struct addrspace * as_create(void) {
        struct addrspace *as;

        as = kmalloc(sizeof(struct addrspace));
        if (as == NULL) {
                return NULL;
        }
        tlb_flush();
        return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
        struct addrspace *newas;

        newas = as_create();
        if (newas == NULL) {
                return ENOMEM;
        }

        uint32_t pid = (uint32_t) old;

        spinlock_acquire(&hpt_lock);
        for (int i = 0; i < hpt_size; i++) {

                if (!hpt[i].inuse || hpt[i].pid != pid) {
                        continue;
                }

                vaddr_t old_entry_lo = hpt[i].entry_lo & PAGE_FRAME;
                vaddr_t new_entry_lo = old_entry_lo;

                if (old_entry_lo != 0) {
                        new_entry_lo = alloc_kpages(1);
                        if (new_entry_lo == 0) {
                                spinlock_release(&hpt_lock);
                                return ENOMEM;
                        }
                        memmove((void *)new_entry_lo,(void *)old_entry_lo,
                                PAGE_SIZE);
                }

                new_entry_lo |= ((hpt[i].entry_lo & HPTABLE_STATEBITS) |
                                 (hpt[i].entry_lo & (1 << HPTABLE_DIRTY)) |
                                 (hpt[i].entry_lo & (1 << HPTABLE_VALID)) |
                                 (hpt[i].entry_lo & (1 << HPTABLE_GLOBAL)));

                if (!insert_page_table_entry(newas, hpt[i].entry_hi,
                                             new_entry_lo)) {

                        spinlock_release(&hpt_lock);
                        as_destroy(newas);
                        return ENOMEM;
                }
        }
        spinlock_release(&hpt_lock);

        *ret = newas;
        return 0;
}

void as_destroy(struct addrspace *as) {
        uint32_t pid = (uint32_t) as;
        spinlock_acquire(&hpt_lock);

        for (int i = 0; i < hpt_size; i++) {

                if (!hpt[i].inuse || hpt[i].pid != pid) {
                        continue;
                }

                free_kpages(hpt[i].entry_lo);

                hpt[i].inuse = false;
                hpt[i].pid = 0;
                hpt[i].entry_hi = 0;
                hpt[i].entry_lo = 0;
        }
        spinlock_release(&hpt_lock);
        tlb_flush();
        kfree(as);
}

void as_activate(void) {
        struct addrspace *as;

        as = proc_getas();
        if (as == NULL) {
                return;
        }

        tlb_flush();
}

void as_deactivate(void) {
        struct addrspace *as;

        as = proc_getas();
        if (as == NULL) {
                return;
        }

        tlb_flush();
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                     int readable, int writeable, int executable) {

        int result = define_memory(as, vaddr, memsize,
                                   (readable|writeable|executable) << 1);

        if (result) {
                return ENOMEM;
        }

        return 0;
}

/* as_prepare_load does not need to do anything, because
 * as_define_region called the static function "int define_memory",
 * which set an SWRITE bit within each entry_lo.
 * 
 * When VM Fault is triggered, if it sees an
 * SWRITE bit within entry_lo, it will ignore the permissions bits
 * stored and instead temporarily allow read/write.
 * 
 * The SWRITE bit is cleared by as_complete_load. */
int as_prepare_load(struct addrspace *as) {
        (void) as;
        return 0;
}

int as_complete_load(struct addrspace *as) {

        uint32_t pid = (uint32_t) as;
        spinlock_acquire(&hpt_lock);
        for (int i = 0; i < hpt_size; i++) {

                if (hpt[i].inuse && hpt[i].pid == pid &&
                    hpt[i].entry_lo & HPTABLE_SWRITE) {

                        hpt[i].entry_lo &= ~HPTABLE_SWRITE;
                }
        }
        spinlock_release(&hpt_lock);
        /*
         * need to flush tlb because during prepare load we set
         * the softwrite which consequently caused
         * the tlb entry to have dirty bit set,
         * but this soft write is only temporary so by flushing the tlb
         * the next time there won't be a softwrite and
         * therefore the permission will be set to normal
         */
        tlb_flush();
        return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {

        *stackptr = USERSTACK;
        vaddr_t location = USERSTACK - (PAGE_SIZE * STACK_PAGE);

        int result = define_memory(as, location, PAGE_SIZE * STACK_PAGE,
                                   HPTABLE_STACK_RW << 1);
        if (result) {
                return ENOMEM;
        }
        return 0;
}

