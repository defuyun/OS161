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

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */
uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr)
{
    uint32_t index;
    index = (((uint32_t )as) ^ (faultaddr >> PAGE_BITS)) % hash_table_size;
    return index;
}

static bool insert_page_table_entry(struct addrspace * as, uint32_t entry_hi, uint32_t entry_lo) {
    uint32_t vpn = entry_hi & PAGE_FRAME;
    int index = hpt_hash(as, entry_hi);

    spinlock_acquire(&hash_page_table_lock);
    int next = hash_page_table[index].next;
    int head = index;
    int count = 0;

    while(hash_page_table[index].inuse && count < hash_table_size) {
        ++index;
        index %= hash_table_size;
        count++;
    }

    if(count == hash_table_size) {
        spinlock_release(&hash_page_table_lock);
        return false;
    }

    if(head != index) {
        hash_page_table[head].next = index;
        hash_page_table[index].next = next;
        hash_page_table[index].prev = head;
        if(next != NO_NEXT_PAGE) {
            hash_page_table[next].prev = index;
        }
    }

    hash_page_table[index].entry_hi = vpn;
    hash_page_table[index].entry_lo = entry_lo;
    hash_page_table[index].inuse = true;
    hash_page_table[index].pid = (uint32_t) as;

    spinlock_release(&hash_page_table_lock);
    return true;
}

static int allocate_memory(struct addrspace * as, vaddr_t addr, size_t memsize, int permission, int defined) {
    if(addr + memsize >= MIPS_KSEG0) {
        return EFAULT;
    }

    uint32_t top = ((addr + memsize + PAGE_SIZE -1) & PAGE_FRAME) >> FLAG_OFFSET;
    uint32_t base = addr >> FLAG_OFFSET;

    for(uint32_t start = top; start <= base; start++) {
        paddr_t paddr = alloc_kpages(1);
        uint32_t entry_hi = start << FLAG_OFFSET;
        uint32_t entry_lo = ((paddr & PAGE_FRAME) | (1 << HPTABLE_VALID)) & (1 << HPTABLE_GLOBAL);
        if(permission & HPTABLE_WRITE) {
            entry_lo |= 1 << HPTABLE_DIRTY;
        } else {
            entry_lo &= ~(1 << HPTABLE_DIRTY);
        }
        entry_lo |= permission;
        entry_lo |= defined;

        if(!insert_page_table_entry(as, entry_hi, entry_lo)) {
            return ENOMEM;
        }
    }

    return 0;
}

void tlb_flush(void) {
	int i, spl;
 	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
 }

struct addrspace *
as_create(void)
{
    struct addrspace *as;

    as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }
    return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
    struct addrspace *newas;

    newas = as_create();
    if (newas==NULL) {
        return ENOMEM;
    }

    uint32_t pid = (uint32_t) old;

    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].inuse && hash_page_table[i].pid == pid) {
                share_address(hash_page_table[i].entry_lo & PAGE_FRAME);
                if(!insert_page_table_entry(newas,
                                        hash_page_table[i].entry_hi,
                                        hash_page_table[i].entry_lo
                                        )) {
                spinlock_release(&hash_page_table_lock);
                as_destroy(newas);
                return ENOMEM; // return some fault telling user no more space left in page table
            }
        }
    }
    spinlock_release(&hash_page_table_lock);

    *ret = newas;
    return 0;
}

void
as_destroy(struct addrspace *as)
{
    uint32_t pid = (uint32_t) as;
    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].inuse && hash_page_table[i].pid == pid) {
            free_kpages(hash_page_table[i].entry_lo);

            int prev = hash_page_table[i].prev;
            int next = hash_page_table[i].next;

            if(prev != NO_NEXT_PAGE) {
                hash_page_table[prev].next = next;
            }

            if(next != NO_NEXT_PAGE) {
                hash_page_table[next].prev = prev;
            }

            hash_page_table[i].inuse = false;
            hash_page_table[i].next = NO_NEXT_PAGE;
            hash_page_table[i].prev = NO_NEXT_PAGE;
        }
    }
    spinlock_release(&hash_page_table_lock);

    kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	tlb_flush();
}

void
as_deactivate(void)
{
        /*
         * Write this. For many designs it won't need to actually do
         * anything. See proc.c for an explanation of why it (might)
         * be needed.
         */

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
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                 int readable, int writeable, int executable)
{
    int result = allocate_memory(as, vaddr, memsize, (readable|writeable|executable) << 1, HPTABLE_DEFINED);
    if(result) {
        return ENOMEM;
    }

    return 0;
}

int
as_prepare_load(struct addrspace *as)
{
    uint32_t pid = (uint32_t) as;
    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].inuse &&
           hash_page_table[i].pid == pid &&
           hash_page_table[i].entry_lo & HPTABLE_DEFINED) {
            hash_page_table[i].entry_lo &= ~HPTABLE_DEFINED;
            hash_page_table[i].entry_lo |= HPTABLE_SWRITE;
        }
    }
    spinlock_release(&hash_page_table_lock);
    return 0;
}

int
as_complete_load(struct addrspace *as)
{
    uint32_t pid = (uint32_t) as;
    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].inuse &&
           hash_page_table[i].pid == pid &&
           hash_page_table[i].entry_lo & HPTABLE_SWRITE) {
            hash_page_table[i].entry_lo &= ~HPTABLE_SWRITE;
        }
    }
    spinlock_release(&hash_page_table_lock);
    // need to flush tlb because during prepare load we set the softwrite which consequently caused
    // the tlb entry to have dirty bit set, but this soft write is only temporary so by flushing the tlb
    // the next time there won't be a softwrite and therefore the permission will be set to normal
    tlb_flush();
    return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
        /* Initial user-level stack pointer */
    *stackptr = USERSTACK;
    vaddr_t location = USERSTACK - PAGE_SIZE * STACK_PAGE;
    int result = allocate_memory(as, location, PAGE_SIZE * STACK_PAGE, 6 << 1, 0);
    if(result) {
        return ENOMEM;
    }

    return 0;
}

