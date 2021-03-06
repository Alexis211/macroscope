#include <multiboot.h>
#include <config.h>
#include <dbglog.h>
#include <sys.h>

#include <gdt.h>
#include <idt.h>
#include <frame.h>
#include <paging.h>
#include <region.h>
#include <kmalloc.h>

#include <thread.h>

#include <slab_alloc.h>
#include <hashtbl.h>

extern const void k_end_addr;	// defined in linker script : 0xC0000000 plus kernel stuff

void breakpoint_handler(registers_t *regs) {
	dbg_printf("Breakpoint! (int3)\n");
	BOCHS_BREAKPOINT;
}

void region_test1() {
	void* p = region_alloc(0x1000, "Test region", 0);
	dbg_printf("Allocated one-page region: 0x%p\n", p);
	dbg_print_region_info();
	void* q = region_alloc(0x1000, "Test region", 0);
	dbg_printf("Allocated one-page region: 0x%p\n", q);
	dbg_print_region_info();
	void* r = region_alloc(0x2000, "Test region", 0);
	dbg_printf("Allocated two-page region: 0x%p\n", r);
	dbg_print_region_info();
	void* s = region_alloc(0x10000, "Test region", 0);
	dbg_printf("Allocated 16-page region: 0x%p\n", s);
	dbg_print_region_info();
	region_free(p);
	dbg_printf("Freed region 0x%p\n", p);
	dbg_print_region_info();
	region_free(q);
	dbg_printf("Freed region 0x%p\n", q);
	dbg_print_region_info();
	region_free(r);
	dbg_printf("Freed region 0x%p\n", r);
	dbg_print_region_info();
	region_free(s);
	dbg_printf("Freed region 0x%p\n", s);
	dbg_print_region_info();
}

void region_test2() {
	// allocate a big region and try to write into it
	dbg_printf("Begin region test 2...");
	const size_t n = 200;
	void* p0 = region_alloc(n * PAGE_SIZE, "Test big region", default_allocator_pf_handler);
	for (size_t i = 0; i < n; i++) {
		uint32_t *x = (uint32_t*)(p0 + i * PAGE_SIZE);
		x[0] = 12;
		x[1] = (i * 20422) % 122;
	}
	// unmap memory
	for (size_t i = 0; i < n; i++) {
		void* p = p0 + i * PAGE_SIZE;
		uint32_t *x = (uint32_t*)p;
		ASSERT(x[1] == (i * 20422) % 122);

		uint32_t f = pd_get_frame(p);
		ASSERT(f != 0);
		pd_unmap_page(p);
		ASSERT(pd_get_frame(p) == 0);

		frame_free(f, 1);
	}
	region_free(p0);
	dbg_printf("OK\n");
}

void kmalloc_test(void* kernel_data_end) {
	// Test kmalloc !
	dbg_print_region_info();
	dbg_printf("Begin kmalloc test...\n");
	const int m = 200;
	uint16_t** ptr = kmalloc(m * sizeof(uint32_t));
	for (int i = 0; i < m; i++) {
		size_t s = 1 << ((i * 7) % 11 + 2);
		ptr[i] = (uint16_t*)kmalloc(s);
		ASSERT((void*)ptr[i] >= kernel_data_end && (size_t)ptr[i] < 0xFFC00000);
		*ptr[i] = ((i * 211) % 1024);
	}
	dbg_printf("Fully allocated.\n");
	dbg_print_region_info();
	for (int i = 0; i < m; i++) {
		for (int j = i; j < m; j++) {
			ASSERT(*ptr[j] == (j * 211) % 1024);
		}
		kfree(ptr[i]);
	}
	kfree(ptr);
	dbg_printf("Kmalloc test OK.\n");
	dbg_print_region_info();
}

void test_hashtbl_1() {
	// hashtable test
	hashtbl_t *ht = create_hashtbl(str_key_eq_fun, str_hash_fun, 0);
	hashtbl_add(ht, "test1", "Hello, world [test1]");
	hashtbl_add(ht, "test2", "Hello, world [test2]");
	dbg_printf("ht[test1] = %s\n", hashtbl_find(ht, "test1"));
	dbg_printf("ht[test] = %s\n", hashtbl_find(ht, "test"));
	dbg_printf("ht[test2] = %s\n", hashtbl_find(ht, "test2"));
	dbg_printf("adding test...\n");
	hashtbl_add(ht, "test", "Forever alone");
	dbg_printf("ht[test1] = %s\n", hashtbl_find(ht, "test1"));
	dbg_printf("ht[test] = %s\n", hashtbl_find(ht, "test"));
	dbg_printf("ht[test2] = %s\n", hashtbl_find(ht, "test2"));
	dbg_printf("removing test1...\n");
	hashtbl_remove(ht, "test1");
	dbg_printf("ht[test1] = %s\n", hashtbl_find(ht, "test1"));
	dbg_printf("ht[test] = %s\n", hashtbl_find(ht, "test"));
	dbg_printf("ht[test2] = %s\n", hashtbl_find(ht, "test2"));
	delete_hashtbl(ht);
}

void test_hashtbl_2() {
	hashtbl_t *ht = create_hashtbl(id_key_eq_fun, id_hash_fun, 0);
	hashtbl_add(ht, (void*)12, "Hello, world [12]");
	hashtbl_add(ht, (void*)777, "Hello, world [777]");
	dbg_printf("ht[12] = %s\n", hashtbl_find(ht, (void*)12));
	dbg_printf("ht[144] = %s\n", hashtbl_find(ht, (void*)144));
	dbg_printf("ht[777] = %s\n", hashtbl_find(ht, (void*)777));
	dbg_printf("adding 144...\n");
	hashtbl_add(ht, (void*)144, "Forever alone");
	dbg_printf("ht[12] = %s\n", hashtbl_find(ht, (void*)12));
	dbg_printf("ht[144] = %s\n", hashtbl_find(ht, (void*)144));
	dbg_printf("ht[777] = %s\n", hashtbl_find(ht, (void*)777));
	dbg_printf("removing 12...\n");
	hashtbl_remove(ht, (void*)12);
	dbg_printf("ht[12] = %s\n", hashtbl_find(ht, (void*)12));
	dbg_printf("ht[144] = %s\n", hashtbl_find(ht, (void*)144));
	dbg_printf("ht[777] = %s\n", hashtbl_find(ht, (void*)777));
	delete_hashtbl(ht);
}

void test_thread(void* a) {
	for(int i = 0; i < 120; i++) {
		dbg_printf("b");
		for (int x = 0; x < 100000; x++) asm volatile("xor %%ebx, %%ebx":::"%ebx");
		if (i % 8 == 0) yield();
	}
}
void kernel_init_stage2(void* data) {
	dbg_print_region_info();
	dbg_print_frame_stats();

	test_hashtbl_1();
	test_hashtbl_2();

	thread_t *tb = new_thread(test_thread, 0);
	resume_thread(tb, false);

	for (int i = 0; i < 120; i++) {
		dbg_printf("a");
		for (int x = 0; x < 100000; x++) asm volatile("xor %%ebx, %%ebx":::"%ebx");
	}
	PANIC("Reached kmain end! Falling off the edge.");
}
 
void kmain(struct multiboot_info_t *mbd, int32_t mb_magic) {
	dbglog_setup();

	dbg_printf("Hello, kernel world!\n");
	dbg_printf("This is %s, version %s.\n", OS_NAME, OS_VERSION);

	ASSERT(mb_magic == MULTIBOOT_BOOTLOADER_MAGIC);

	gdt_init(); dbg_printf("GDT set up.\n");

	idt_init(); dbg_printf("IDT set up.\n");
	idt_set_ex_handler(EX_BREAKPOINT, breakpoint_handler);
	asm volatile("int $0x3");	// test breakpoint

	size_t total_ram = ((mbd->mem_upper + mbd->mem_lower) * 1024);
	dbg_printf("Total ram: %d Kb\n", total_ram / 1024);

	// used for allocation of data structures before malloc is set up
	// a pointer to this pointer is passed to the functions that might have
	// to allocate memory ; they just increment it of the allocated quantity
	void* kernel_data_end = (void*)&k_end_addr;

	frame_init_allocator(total_ram, &kernel_data_end);
	dbg_printf("kernel_data_end: 0x%p\n", kernel_data_end);
	dbg_print_frame_stats();

	paging_setup(kernel_data_end);
	dbg_printf("Paging seems to be working!\n");

	BOCHS_BREAKPOINT;

	region_allocator_init(kernel_data_end);
	region_test1();
	region_test2();

	kmalloc_setup();
	kmalloc_test(kernel_data_end);

	// enter multi-threading mode
	// interrupts are enabled at this moment, so all
	// code run from now on should be preemtible (ie thread-safe)
	threading_setup(kernel_init_stage2, 0);
	PANIC("Should never come here.");
}

/* vim: set ts=4 sw=4 tw=0 noet :*/
