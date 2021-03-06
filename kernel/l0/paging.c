#include <paging.h>
#include <frame.h>
#include <idt.h>
#include <dbglog.h>
#include <region.h>
#include <mutex.h>
#include <thread.h>
#include <kmalloc.h>

#define PAGE_OF_ADDR(x)		(((size_t)x >> PAGE_SHIFT) % N_PAGES_IN_PT)
#define PT_OF_ADDR(x)		((size_t)x >> (PAGE_SHIFT + PT_SHIFT))

#define PTE_PRESENT			(1<<0)
#define PTE_RW				(1<<1)
#define PTE_USER			(1<<2)
#define PTE_WRITE_THROUGH	(1<<3)
#define PTE_DISABLE_CACHE	(1<<4)
#define PTE_ACCESSED		(1<<5)
#define PTE_DIRTY			(1<<6)		// only PTE
#define PTE_SIZE_4M			(1<<7)		// only PDE
#define PTE_GLOBAL			(1<<8)		// only PTE
#define PTE_FRAME_SHIFT		12

typedef struct page_table {
	uint32_t page[1024];
} pagetable_t;

struct page_directory {
	uint32_t phys_addr;		// physical address of page directory
	// to modify a page directory, we first map it
	// then we can use mirroring to edit it
	// (the last 4M of the address space are mapped to the PD itself)

	mutex_t mutex;
};


// access kernel page directory page defined in loader.s
// (this is a correct higher-half address)
extern pagetable_t kernel_pd;

// pre-allocate a page table so that we can map the first 4M of kernel memory
static pagetable_t __attribute__((aligned(PAGE_SIZE))) kernel_pt0;

extern char kernel_stack_protector;

static pagedir_t kernel_pd_d;

#define current_pt ((pagetable_t*)PD_MIRROR_ADDR)
#define current_pd ((pagetable_t*)(PD_MIRROR_ADDR + (N_PAGES_IN_PT-1)*PAGE_SIZE))

void page_fault_handler(registers_t *regs) {
	void* vaddr;
	asm volatile("movl %%cr2, %0":"=r"(vaddr));

	if ((size_t)vaddr >= K_HIGHHALF_ADDR) {
		uint32_t pt = PT_OF_ADDR(vaddr);

		if (current_pd != &kernel_pd && current_pd->page[pt] != kernel_pd.page[pt]) {
			current_pd->page[pt] = kernel_pd.page[pt];
			invlpg(&current_pt[pt]);
			return;
		}
		if (regs->eflags & EFLAGS_IF) asm volatile("sti");	// from now on we are preemptible

		if (vaddr >= (void*)&kernel_stack_protector && vaddr < (void*)&kernel_stack_protector + PAGE_SIZE) {
			dbg_printf("Kernel stack overflow at 0x%p\n", vaddr);
			PANIC("Kernel stack overflow.");
		}

		if ((size_t)vaddr >= PD_MIRROR_ADDR) {
			dbg_printf("Fault on access to mirrorred PD at 0x%p\n", vaddr);
			dbg_print_region_info();
			PANIC("Unhandled kernel space page fault");
		}

		region_info_t *i = find_region(vaddr);
		if (i == 0) {
			dbg_printf("Kernel pagefault in non-existing region at 0x%p\n", vaddr);
			dbg_dump_registers(regs);
			PANIC("Unhandled kernel space page fault");
		}
		if (i->pf == 0) {
			dbg_printf("Kernel pagefault in region with no handler at 0x%p\n", vaddr);
			dbg_dump_registers(regs);
			dbg_print_region_info();
			PANIC("Unhandled kernel space page fault");
		}
		i->pf(get_current_pagedir(), i, vaddr);
	} else {
		if (regs->eflags & EFLAGS_IF) asm volatile("sti");	// userspace PF handlers should always be preemptible

		dbg_printf("Userspace page fault at 0x%p\n", vaddr);
		PANIC("Unhandled userspace page fault");
		// not handled yet
		// TODO
	}
}

void paging_setup(void* kernel_data_end) {
	size_t n_kernel_pages =
		PAGE_ALIGN_UP((size_t)kernel_data_end - K_HIGHHALF_ADDR)/PAGE_SIZE;

	ASSERT(n_kernel_pages <= 1024);	// we use less than 4M for kernel

	// setup kernel_pd_d structure
	kernel_pd_d.phys_addr = (size_t)&kernel_pd - K_HIGHHALF_ADDR;
	kernel_pd_d.mutex = MUTEX_UNLOCKED;

	// setup kernel_pt0
	ASSERT(PAGE_OF_ADDR(K_HIGHHALF_ADDR) == 0);	// kernel is 4M-aligned
	ASSERT(FIRST_KERNEL_PT == 768);
	for (size_t i = 0; i < n_kernel_pages; i++) {
		if ((i * PAGE_SIZE) + K_HIGHHALF_ADDR == (size_t)&kernel_stack_protector) {
			kernel_pt0.page[i] = 0;	// don't map kernel stack protector page
			frame_free(i, 1);
		} else {
			kernel_pt0.page[i] = (i << PTE_FRAME_SHIFT) | PTE_PRESENT | PTE_RW | PTE_GLOBAL;
		}
	}
	for (size_t i = n_kernel_pages; i < 1024; i++){
		kernel_pt0.page[i] = 0;
	}

	// replace 4M mapping by kernel_pt0
	kernel_pd.page[FIRST_KERNEL_PT] =
		(((size_t)&kernel_pt0 - K_HIGHHALF_ADDR) & PAGE_MASK) | PTE_PRESENT | PTE_RW;
	// set up mirroring
	kernel_pd.page[N_PAGES_IN_PT-1] =
		(((size_t)&kernel_pd - K_HIGHHALF_ADDR) & PAGE_MASK) | PTE_PRESENT | PTE_RW;

	invlpg((void*)K_HIGHHALF_ADDR);

	// paging already enabled in loader, nothing to do.

	// disable 4M pages (remove PSE bit in CR4)
	uint32_t cr4;
	asm volatile("movl %%cr4, %0": "=r"(cr4));
	cr4 &= ~0x00000010;
	asm volatile("movl %0, %%cr4":: "r"(cr4));

	idt_set_ex_handler(EX_PAGE_FAULT, page_fault_handler);
}

pagedir_t *get_current_pagedir() {
	if (current_thread == 0) return &kernel_pd_d;
	return current_thread->current_pd_d;
}

pagedir_t *get_kernel_pagedir() {
	return &kernel_pd_d;
}

void switch_pagedir(pagedir_t *pd) {
	asm volatile("movl %0, %%cr3":: "r"(pd->phys_addr));
	if (current_thread != 0) current_thread->current_pd_d = pd;
}

// ============================== //
// Mapping and unmapping of pages //
// ============================== //

uint32_t pd_get_frame(void* vaddr) {
	uint32_t pt = PT_OF_ADDR(vaddr);
	uint32_t page = PAGE_OF_ADDR(vaddr);

	pagetable_t *pd = ((size_t)vaddr >= K_HIGHHALF_ADDR ? &kernel_pd : current_pd);

	if (!pd->page[pt] & PTE_PRESENT) return 0;
	if (!current_pt[pt].page[page] & PTE_PRESENT) return 0;
	return current_pt[pt].page[page] >> PTE_FRAME_SHIFT;
}

int pd_map_page(void* vaddr, uint32_t frame_id, bool rw) {
	uint32_t pt = PT_OF_ADDR(vaddr);
	uint32_t page = PAGE_OF_ADDR(vaddr);

	ASSERT((size_t)vaddr < PD_MIRROR_ADDR);
	
	pagedir_t *pdd = ((size_t)vaddr >= K_HIGHHALF_ADDR || current_thread == 0
							? &kernel_pd_d : current_thread->current_pd_d);
	pagetable_t *pd = ((size_t)vaddr >= K_HIGHHALF_ADDR ? &kernel_pd : current_pd);
	mutex_lock(&pdd->mutex);

	if (!pd->page[pt] & PTE_PRESENT) {
		uint32_t new_pt_frame = frame_alloc(1);
		if (new_pt_frame == 0) {
			mutex_unlock(&pdd->mutex);
			return 1;	// OOM
		}

		current_pd->page[pt] = pd->page[pt] =
			(new_pt_frame << PTE_FRAME_SHIFT) | PTE_PRESENT | PTE_RW;
		invlpg(&current_pt[pt]);
	}
	current_pt[pt].page[page] =
		(frame_id << PTE_FRAME_SHIFT)
			| PTE_PRESENT
			| ((size_t)vaddr < K_HIGHHALF_ADDR ? PTE_USER : PTE_GLOBAL)
			| (rw ? PTE_RW : 0);
	invlpg(vaddr);

	mutex_unlock(&pdd->mutex);
	return 0;
} 

void pd_unmap_page(void* vaddr) {
	uint32_t pt = PT_OF_ADDR(vaddr);
	uint32_t page = PAGE_OF_ADDR(vaddr);

	pagetable_t *pd = ((size_t)vaddr >= K_HIGHHALF_ADDR ? &kernel_pd : current_pd);
	// no need to lock the PD's mutex

	if (!pd->page[pt] & PTE_PRESENT) return;
	if (!current_pt[pt].page[page] & PTE_PRESENT) return;

	current_pt[pt].page[page] = 0;
	invlpg(vaddr);

	// If the page table is completely empty we might want to free
	// it, but we would actually lose a lot of time checking if
	// the PT is really empty (since we don't store the
	// number of used pages in each PT), so it's probably not worth it
}

// Creation and deletion of page directories

pagedir_t *create_pagedir() {
	uint32_t pd_phys = 0;
	pagedir_t *pd = 0;
	void* temp = 0;

	pd_phys = frame_alloc(1);
	if (pd_phys == 0) goto error;

	pd = (pagedir_t*)kmalloc(sizeof(pagedir_t));
	if (pd == 0) goto error;

	temp = region_alloc(PAGE_SIZE, 0, 0);
	if (temp == 0) goto error;

	int error = pd_map_page(temp, pd_phys, true);
	if (error) goto error;

	pd->phys_addr = pd_phys * PAGE_SIZE;
	pd->mutex = MUTEX_UNLOCKED;

	// initialize PD with zeroes
	pagetable_t *pt = (pagetable_t*)temp;
	for (size_t i = 0; i < N_PAGES_IN_PT; i++) {
		pt->page[i] = 0;
	}
	// use kernel page tables
	for(size_t i = FIRST_KERNEL_PT; i < N_PAGES_IN_PT-1; i++) {
		pt->page[i] = kernel_pd.page[i];
	}
	// set up mirroring
	pt->page[N_PAGES_IN_PT-1] = pd->phys_addr | PTE_PRESENT | PTE_RW;

	region_free_unmap(temp);

	return pd;

	error:
	if (pd_phys != 0) frame_free(pd_phys, 1);
	if (pd != 0) kfree(pd);
	if (temp != 0) region_free(temp);
	return 0;
}

void delete_pagedir(pagedir_t *pd) {
	pagedir_t *restore_pd = get_current_pagedir();
	if (restore_pd == pd) restore_pd = &kernel_pd_d;

	// make a copy of page directory on the stack
	switch_pagedir(pd);
	pagetable_t backup;
	for (size_t i = 0; i < N_PAGES_IN_PT; i++) {
		backup.page[i] = current_pd->page[i];
	}
	switch_pagedir(restore_pd);
	
	// free the page tables
	for (size_t i = 0; i < FIRST_KERNEL_PT; i++) {
		if (backup.page[i] & PTE_PRESENT)
			frame_free(backup.page[i] >> PTE_FRAME_SHIFT, 1);
	}
	// free the page directory page
	uint32_t pd_phys = pd->phys_addr / PAGE_SIZE;
	ASSERT(pd_phys == (backup.page[N_PAGES_IN_PT-1] >> PTE_FRAME_SHIFT));
	frame_free(pd_phys, 1);
	// free the pagedir_t structure
	kfree(pd);

	return;
}

/* vim: set ts=4 sw=4 tw=0 noet :*/
