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

bool insert_page_table_entry(struct addrspace * as, uint32_t entry_hi, uint32_t entry_lo) {
    uint32_t vpn = entry_hi & PAGE_FRAME;
    uint32_t index = hpt_hash(as, entry_hi);

    spinlock_acquire(&hash_table_lock);
    uint32_t next = hash_page_table[index].next;
    uint32_t head = index;
    int count = 0;

    while(hash_page_table[index].inuse && count < hash_table_size) {
        ++index;
        index %= hash_table_size;
        count++;
    }

    if(count == hash_table_size) {
        spinlock_release(&hash_table_lock);
        return false;
    }

    if(head != index) {
        hash_page_table[head].next = index;
        hash_page_table[index].next = next;
        hash_page_table[index].prev = head;
    }

    hash_page_table[index].entry_hi = entry_hi;
    hash_page_table[index].entry_lo = entry_lo;
    hash_page_table[index].inuse = true;
    hash_page_table[index].pid = (uint32_t) as;

    spinlock_release(&hash_table_lock);
    return true;
}

void tlb_flush(void) {
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

    int pid = (uint32_t) old;

    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].pid == pid) {
            if(!insert_page_table_entry(newas,
                                        hash_page_table[i].entry_hi,
                                        hash_page_table[i].entry_lo,
                                        )) {
                spinlock_release(&hash_page_table_lock);
                return EFAULT; // return some fault telling user no more space left in page table
            }
        }
    }
    spinlock_release(&hash_page_table_lock);

    (void)old;

    *ret = newas;
    return 0;
}

void
as_destroy(struct addrspace *as)
{
    uint32_t pid = (uint32_t) as;
    spinlock_acquire(&hash_page_table_lock);
    for(int i = 0; i < hash_table_size; i++) {
        if(hash_page_table[i].pid == pid) {
            free_kpages(hash_page_table[i].entry_lo >> FLAG_OFFSET);

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
	int i, spl;
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
        /*
         * Write this.
         */

        (void)as;
        (void)vaddr;
        (void)memsize;
        (void)readable;
        (void)writeable;
        (void)executable;
        return ENOSYS; /* Unimplemented */
}

int
as_prepare_load(struct addrspace *as)
{
        /*
         * Write this.
         */

        (void)as;
        return 0;
}

int
as_complete_load(struct addrspace *as)
{
        /*
         * Write this.
         */

        (void)as;
        return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
        /*
         * Write this. wtf is this suppose to be
         */

        (void)as;

        /* Initial user-level stack pointer */
        *stackptr = USERSTACK;

        return 0;
}

