#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

/* Place your frametable data-structures here
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock frame_table_lock = SPINLOCK_INITIALIZER;


static struct frame_table_entry * frame_table = NULL;
static int total_frame_entries;
static int next_free_frame;

static void set_frame_table_entry(int index, int _next, int _references, short _inuse) {
    frame_table[index].next = _next;
    frame_table[index].references = _references;
    frame_table[index].inuse = _inuse;
}

// initialize frame table
void init_frame_table(void) {
    spinlock_acquire(&frame_table_lock);
    // allocate memory space for the frame table
    paddr_t size = ram_getsize();
    total_frame_entries = size / PAGE_SIZE;
    paddr_t location = size - (total_frame_entries * sizeof(struct frame_table_entry));
    frame_table = (struct frame_table_entry *) PADDR_TO_KVADDR(location);

    // initialize the next pointers of the frame table
    for(int i = 0; i < total_frame_entries; i++) {
        set_frame_table_entry(i,i+1,0,FRAME_UNUSED);
    }

    set_frame_table_entry(total_frame_entries-1, NO_NEXT_FRAME, 0, FRAME_UNUSED);

    // set os161 frame to reserved
    vaddr_t os161_size = ram_getfirstfree();
    int os161_num_entries = os161_size / PAGE_SIZE;
    for(next_free_frame = 0; next_free_frame < os161_num_entries; next_free_frame++) {
        set_frame_table_entry(i,NO_NEXT_FRAME,1,FRAME_RESERVED);
    }

    // set frame table as reserved
    int frame_table_num_entries = (sizeof(struct frame_table_entry) * total_frame_entries) / PAGE_SIZE;
    for(int i = 0; i < frame_table_num_entries; i++) {
        set_frame_table_entry(total_frame_entries-1-i, NO_NEXT_FRAME, 1, FRAME_RESERVED);
    }

    spinlock_release(&frame_table_lock);
}

/* Note that this function returns a VIRTUAL address, not a physical
 * address
 * WARNING: this function gets called very early, before
 * vm_bootstrap().  You may wish to modify main.c to call your
 * frame table initialisation function, or check to see if the
 * frame table has been initialised and call ram_stealmem() otherwise.
 */

vaddr_t alloc_kpages(unsigned int npages)
{
        /*
         * IMPLEMENT ME.  You should replace this code with a proper
         *                implementation.
         */

        paddr_t addr;

        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);

        if(addr == 0)
                return 0;

        return PADDR_TO_KVADDR(addr);
}

void free_kpages(vaddr_t addr)
{
        (void) addr;
}

