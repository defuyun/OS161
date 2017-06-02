#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <spinlock.h>

/* local defined constants for use by the frame table */
#define FRAME_UNUSED 0
#define FRAME_USED 1
#define FRAME_RESERVED 2
#define NO_NEXT_FRAME -1

/* locks for synchronisation */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock ft_lock = SPINLOCK_INITIALIZER;
struct spinlock hpt_lock = SPINLOCK_INITIALIZER;

/* ft table struct visible only to this file */
struct ft_entry {
        /* number of references to this frame */
        int ref;
        /* next free frame within the free frame list */
        int next;
        /* usage status of the current frame */
        short inuse;
};

static struct ft_entry * ft = NULL;
static int total_num_frames;
/* next free frame index within the ft */
static int ft_next_free;

struct hpt_entry * hpt = NULL;
int hpt_size;

/* total RAM */
static paddr_t total_mem_size;
/* bottom location of the ft. ft is at the top of memory. */
static paddr_t ft_bot_location;
/* bottom location of the hpt. hpt is below the ft. */
static paddr_t hpt_bot_location;
/* top paddr location of the os. os is at the bottom of memory. */
static paddr_t os_top_location;

/* top and bottom indexes of the usable mem region within the ft. */
static int usable_mem_top_index;
static int usable_mem_bot_index;




/* sets the new next free frame index, ref count, and usage status
 * of the frame table at the index */
static void set_ft_entry(int index, int new_next,
                         int new_ref, short new_status) {

        // TEST remove when submitting
        KASSERT(index >= 0 && index < total_num_frames);
        KASSERT(new_ref >= 0);
        KASSERT(new_status == FRAME_UNUSED ||
                new_status == FRAME_RESERVED ||
                new_status == FRAME_USED);
        KASSERT((new_next >= 0 && new_next < total_num_frames) ||
                new_next == NO_NEXT_FRAME);


        ft[index].ref = new_ref;
        ft[index].next = new_next;
        ft[index].inuse = new_status;
}



/* initialize frame table */
void init_ft_hpt() {
        spinlock_acquire(&hpt_lock);
        spinlock_acquire(&ft_lock);

        total_mem_size = ram_getsize();

        /* initialize frame table location (mem_top - ft_mem_size) */
        total_num_frames = (total_mem_size + (PAGE_SIZE - 1)) / PAGE_SIZE;
        paddr_t ft_mem_size = total_num_frames * sizeof(struct ft_entry);
        paddr_t ft_bot_location = total_mem_size - ft_mem_size;
        ft = (struct ft_entry *) PADDR_TO_KVADDR(ft_bot_location);

        /* initialize hpt location (ft_bottom_location - hpt_mem_size) */
        hpt_size = total_num_frames * 2; /* hpt_size = num entries in hpt */
        paddr_t hpt_mem_size = hpt_size * sizeof(struct hpt_entry);
        hpt_bot_location = ft_bot_location - hpt_mem_size;
        hpt = (struct hpt_entry *) PADDR_TO_KVADDR(hpt_bot_location);

        /* initialize the contents and next free frame indexes */
        for (int i = 0; i < total_num_frames - 1; i++) {
                set_ft_entry(i, i + 1, 0, FRAME_UNUSED);
        }
        set_ft_entry(total_num_frames - 1, NO_NEXT_FRAME, 0, FRAME_UNUSED);

        /* initialize each entry within the hpt as unused */
        for (int i = 0; i < hpt_size; i++) {
                hpt[i].inuse = false;
                hpt[i].next = NO_NEXT_PAGE;
                hpt[i].prev = NO_NEXT_PAGE;
        }

        int ft_hpt_num_entries = (ft_mem_size + hpt_mem_size + PAGE_SIZE - 1)
                                  / PAGE_SIZE;
        /* set ft and hpt as reserved within the frame table */
        for (int i = 0; i < ft_hpt_num_entries; i++) {
                set_ft_entry(total_num_frames - 1 - i, NO_NEXT_FRAME, 1,
                             FRAME_RESERVED);
        }
        /* get the highest index of the usable mem region */
        usable_mem_top_index = total_num_frames - 1 - ft_hpt_num_entries;

        /* set os161 as reserved within the frame table */
        paddr_t os_mem_size = ram_getfirstfree();
        os_top_location = os_mem_size;
        int os_num_entries = (os_mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (ft_next_free = 0; ft_next_free < os_num_entries; ft_next_free++) {
                set_ft_entry(ft_next_free, NO_NEXT_FRAME, 1, FRAME_RESERVED);
        }
        /* ft_next_free now refers to the first free frame index above os161 */
        usable_mem_bot_index = ft_next_free;

        spinlock_release(&ft_lock);
        spinlock_release(&hpt_lock);
}



vaddr_t alloc_kpages(unsigned int npages) {
        paddr_t paddr;

        spinlock_acquire(&ft_lock);

        if (ft == NULL) {
                spinlock_acquire(&stealmem_lock);
                paddr = ram_stealmem(npages);
                spinlock_release(&stealmem_lock);

        } else if (npages != 1 || ft_next_free == NO_NEXT_FRAME) {
                spinlock_release(&ft_lock);
                return 0;

        } else {
                KASSERT(ft_next_free >= usable_mem_bot_index && ft_next_free <= usable_mem_top_index);

                int curr_index = ft_next_free;

                KASSERT(ft_bot_location + (curr_index * sizeof(struct ft_entry)) >= ft_bot_location &&
                        ft_bot_location + (curr_index * sizeof(struct ft_entry)) < total_mem_size);

                ft_next_free = ft[curr_index].next;

                KASSERT(ft[curr_index].inuse == FRAME_UNUSED);

                KASSERT(ft_next_free >= usable_mem_bot_index && ft_next_free <= usable_mem_top_index);

                set_ft_entry(curr_index, NO_NEXT_FRAME, 1, FRAME_USED);
                /* zero out the page */
                paddr = curr_index * PAGE_SIZE;
                memset((void *)PADDR_TO_KVADDR(paddr), 0, PAGE_SIZE);
        }
        spinlock_release(&ft_lock);

        if (paddr == 0) {
                return 0;
        }

        return PADDR_TO_KVADDR(paddr);
}



void free_kpages(vaddr_t vaddr) {
        vaddr &= PAGE_FRAME;

        spinlock_acquire(&ft_lock);
        if (ft == NULL) {
                spinlock_release(&ft_lock);
                return;
        }

        if (vaddr == 0) {
                spinlock_release(&ft_lock);
                return;
        }

        KASSERT(vaddr >= MIPS_KSEG0 && vaddr < MIPS_KSEG1);

        int ft_index = KVADDR_TO_PADDR(vaddr) / PAGE_SIZE;

        KASSERT(ft_index <= usable_mem_top_index && ft_index >= usable_mem_bot_index);

        KASSERT(ft_bot_location + (ft_index * sizeof(struct ft_entry)) >= ft_bot_location &&
                ft_bot_location + (ft_index * sizeof(struct ft_entry)) < total_mem_size);

        KASSERT(ft[ft_index].inuse == FRAME_USED);
        KASSERT(ft[ft_index].ref >= 1);

        ft[ft_index].ref -= 1;
        if(ft[ft_index].ref == 0) {
                int prev_next_free = ft_next_free;
                ft_next_free = ft_index;
                set_ft_entry(ft_index, prev_next_free, 0, FRAME_UNUSED);
        }

        spinlock_release(&ft_lock);
}



void share_address(vaddr_t addr) {
        spinlock_acquire(&ft_lock);

        if (addr == 0) {
                spinlock_release(&ft_lock);
                return;
        }

        KASSERT(addr >= MIPS_KSEG0 && addr < MIPS_KSEG1);

        int ft_index = KVADDR_TO_PADDR(addr) / PAGE_SIZE;
        KASSERT(ft_index <= usable_mem_top_index && ft_index >= usable_mem_bot_index);
        KASSERT(ft_bot_location + (ft_index * sizeof(struct ft_entry)) >= ft_bot_location &&
                ft_bot_location + (ft_index * sizeof(struct ft_entry)) < total_mem_size);
        KASSERT(ft[ft_index].ref > 0);
        KASSERT(ft[ft_index].inuse == FRAME_USED);
        ft[ft_index].ref += 1;
        spinlock_release(&ft_lock);
}

