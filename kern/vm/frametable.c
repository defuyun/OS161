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
struct spinlock hash_page_table_lock = SPINLOCK_INITIALIZER;

struct hash_page_entry * hash_page_table = NULL;
int hash_table_size;

static struct frame_table_entry * frame_table = NULL;
static int frame_table_size;
static int next_free_frame;

static void set_frame_table_entry(int index, int _next, int _references, short _inuse) {
    frame_table[index].next = _next;
    frame_table[index].references = _references;
    frame_table[index].inuse = _inuse;
}

// initialize frame table
void init_frame_and_page_table() {
    spinlock_acquire(&hash_page_table_lock);
    spinlock_acquire(&frame_table_lock);
    // allocate memory space for the frame table
    int i = 0;
    paddr_t size = ram_getsize();
    frame_table_size = size / PAGE_SIZE;
    paddr_t location = size - (frame_table_size * sizeof(struct frame_table_entry));
    frame_table = (struct frame_table_entry *) PADDR_TO_KVADDR(location);


    hash_table_size = frame_table_size * 2;
    paddr_t total_mem_usage = hash_table_size * sizeof(struct hash_page_entry);
    paddr_t hash_location = location - total_mem_usage;
    hash_page_table = (struct hash_page_entry *) PADDR_TO_KVADDR(hash_location);

    // initialize the next pointers of the frame table
    for(i = 0; i < frame_table_size; i++) {
        set_frame_table_entry(i,i+1,0,FRAME_UNUSED);
    }

    set_frame_table_entry(frame_table_size-1, NO_NEXT_FRAME, 0, FRAME_UNUSED);

    // initialize the entry of hash_page_table to not used
    for(i = 0; i < hash_table_size; i++) {
        hash_page_table[i].inuse = false;
    }

    // set frame table as reserved
    int frame_table_num_entries = (sizeof(struct frame_table_entry) * frame_table_size) / PAGE_SIZE;
    for(i = 0; i < frame_table_num_entries; i++) {
        set_frame_table_entry(frame_table_size-1-i, NO_NEXT_FRAME, 1, FRAME_RESERVED);
    }

    // set hash page table as reserved
    int hash_page_table_entries = total_mem_usage / PAGE_SIZE + i;
    for(; i < hash_page_table_entries; i++) {
        set_frame_table_entry(frame_table_size-1-i; NO_NEXT_FRAME, 1, FRAME_RESERVED);
    }

    // set os161 frame to reserved
    vaddr_t os161_size = ram_getfirstfree();
    int os161_num_entries = (os161_size + PAGE_SIZE -1 )/ PAGE_SIZE; // round up or else the top page will be rounded down and we loss a page
    for(next_free_frame = 0; next_free_frame < os161_num_entries; next_free_frame++) {
        set_frame_table_entry(i,NO_NEXT_FRAME,1,FRAME_RESERVED);
    }

    spinlock_release(&frame_table_lock);
    spinlock_release(&hash_page_table_lock);
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
    if(npages != 1) return 0;

    spinlock_acquire(&frame_table_lock);
    paddr_t addr;

    if(frame_table == NULL) {
        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    } else {
        if(next_free_frame == NO_NEXT_FRAME) {
            spinlock_release(&frame_table_lock);
            return 0;
        }

        int curr = next_free_frame;
        next_free_frame = frame_table[curr].next;
        set_frame_table_entry(curr, NO_NEXT_FRAME, frame_table[curr].references + 1, true);

        addr = curr * PAGE_SIZE;
    }
    spinlock_release(&frame_table_lock);

    if(addr == 0)
        return 0;

    return PADDR_TO_KVADDR(addr);
}

void free_kpages(vaddr_t addr)
{
    (void) addr;
}

