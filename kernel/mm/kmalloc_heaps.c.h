
#ifndef _KMALLOC_C_

   #error This is NOT a header file and it is not meant to be included

/*
 * The only purpose of this file is to keep kmalloc.c shorter.
 * Yes, this file could be turned into a regular C source file, but at the price
 * of making several static functions and variables in kmalloc.c to be just
 * non-static. We don't want that. Code isolation is a GOOD thing.
 */

#endif

#define KMALLOC_MIN_HEAP_SIZE (64 * KB)
#define KMALLOC_FIRST_METADATA_SIZE (512 * KB)
#define KMALLOC_FIRST_METADATA (KERNEL_PA_TO_VA(0x10000)) // +64 KB

STATIC kmalloc_heap heaps[KMALLOC_HEAPS_COUNT];
STATIC int used_heaps;

static char extra_low_mem_heap_metadata[8 * KB];

bool pg_alloc_and_map(uptr vaddr, int page_count);
void pg_free_and_unmap(uptr vaddr, int page_count);

//////////////////////////////////////////////////////////

typedef struct {

   void *vaddr;
   size_t size;
   void *caller_eip;
   bool leaked;

} alloc_entry;

static bool leak_detector_enabled;
static u32 alloc_entries_count;
static alloc_entry alloc_entries[1024];

void debug_kmalloc_start_leak_detector(void)
{
   disable_preemption();

   bzero(alloc_entries, sizeof(alloc_entries));
   alloc_entries_count = 0;
   leak_detector_enabled = true;

   enable_preemption();
}

void debug_kmalloc_stop_leak_detector(bool show_leaks)
{
   leak_detector_enabled = false;

   if (!show_leaks)
      return;

   disable_preemption();

   u32 leak_count = 0;

   for (u32 i = 0; i < alloc_entries_count; i++) {

      if (!alloc_entries[i].leaked)
         continue;

      printk("Leaked block at %p (%u B), caller eip: %p\n",
             alloc_entries[i].vaddr,
             alloc_entries[i].size,
             alloc_entries[i].caller_eip);

      leak_count++;
   }

   printk("Total allocs: %u\n", alloc_entries_count);
   printk("Leak count: %u\n", leak_count);

   enable_preemption();
}

static void debug_kmalloc_register_alloc(void *vaddr, size_t s, void *eip)
{
   disable_preemption();

   VERIFY(alloc_entries_count < ARRAY_SIZE(alloc_entries) - 1);

   alloc_entries[alloc_entries_count++] = (alloc_entry){
      .vaddr = vaddr,
      .size = s,
      .caller_eip = eip,
      .leaked = true
   };

   enable_preemption();
}

static void debug_kmalloc_register_free(void *vaddr, size_t s)
{
   disable_preemption();

   for (u32 i = 0; i < alloc_entries_count; i++) {
      if (alloc_entries[i].vaddr == vaddr) {
         VERIFY(alloc_entries[i].size == s);
         alloc_entries[i].leaked = false;
         enable_preemption();
         return;
      }
   }

   panic("free block at %p, allocated (probably) "
         "before the start of the leak detector\n", vaddr);

   enable_preemption(); // in case 'panic' is replaced with a warning.
}

//////////////////////////////////////////////////////////

void *kmalloc(size_t s)
{
   ASSERT(kmalloc_initialized);

   void *ret = NULL;
   s = roundup_next_power_of_2(s);

   disable_preemption();

   // Iterate in reverse-order because the first heaps are the biggest ones.
   for (int i = used_heaps - 1; i >= 0; i--) {

      const size_t heap_size = heaps[i].size;
      const size_t heap_free = heap_size - heaps[i].mem_allocated;

      /*
       * The heap is too small (unlikely but possible) or the heap has not been
       * created yet, therefore has size = 0 or just there is not enough free
       * space in it.
       */
      if (heap_size < s || heap_free < s)
         continue;

      void *vaddr = internal_kmalloc(&heaps[i], s);

      if (vaddr) {
         s = MAX(s, heaps[i].min_block_size);
         heaps[i].mem_allocated += s;
         ret = vaddr;

         if (leak_detector_enabled) {
            void *eip = __builtin_extract_return_addr(__builtin_return_address(0));
            debug_kmalloc_register_alloc(vaddr, s, eip);
         }

         break;
      }
   }

   enable_preemption();
   return ret;
}

static size_t calculate_block_size(kmalloc_heap *h, uptr vaddr)
{
   block_node *nodes = h->metadata_nodes;
   int n = 0; /* root's node index */
   uptr va = h->vaddr; /* root's node data address == heap's address */
   size_t size = h->size; /* root's node size == heap's size */

   while (size > h->min_block_size) {

      if (!nodes[n].split)
         break;

      size >>= 1;

      if (vaddr >= (va + size)) {
         va += size;
         n = NODE_RIGHT(n);
      } else {
         n = NODE_LEFT(n);
      }
   }

   return size;
}

void kfree2(void *ptr, size_t user_size)
{
   const uptr vaddr = (uptr) ptr;
   size_t size;

   ASSERT(kmalloc_initialized);

   if (!ptr)
      return;

   disable_preemption();

   int hn = -1; /* the heap with the highest vaddr >= our block vaddr */

   for (int i = used_heaps - 1; i >= 0; i--) {

      uptr hva = heaps[i].vaddr;

      if (vaddr < hva)
         continue; /* not in this heap, for sure */

      if (hn < 0 || hva > heaps[hn].vaddr)
         hn = i;
   }

   if (hn < 0)
      goto out; /* no need to release the lock, we're going to panic */

   if (user_size) {

      size = roundup_next_power_of_2(MAX(user_size, heaps[hn].min_block_size));

#ifdef DEBUG
      size_t cs = calculate_block_size(&heaps[hn], vaddr);
      if (cs != size) {
         panic("cs[%u] != size[%u] for block at: %p\n", cs, size, vaddr);
      }
#endif

   } else {
      size = calculate_block_size(&heaps[hn], vaddr);
   }

   ASSERT(vaddr >= heaps[hn].vaddr && vaddr + size <= heaps[hn].heap_over_end);
   internal_kfree2(&heaps[hn], ptr, size);
   heaps[hn].mem_allocated -= size;

   if (KMALLOC_FREE_MEM_POISONING) {
      for (u32 i = 0; i < size / 4; i++)
         ((u32 *)ptr)[i] = KMALLOC_FREE_MEM_POISON_VAL;
   }


   if (leak_detector_enabled) {
      debug_kmalloc_register_free((void *)vaddr, size);
   }

   enable_preemption();
   return;

out:
   panic("[kfree] Heap not found for block: %p\n", ptr);
}

size_t kmalloc_get_total_heap_allocation(void)
{
   size_t tot = 0;
   disable_preemption();

   for (int i = 0; i < used_heaps; i++) {
      tot += heaps[i].mem_allocated;
   }

   enable_preemption();
   return tot;
}


bool kmalloc_create_heap(kmalloc_heap *h,
                         uptr vaddr,
                         size_t size,
                         size_t min_block_size,
                         size_t alloc_block_size,
                         bool linear_mapping,
                         void *metadata_nodes,
                         virtual_alloc_and_map_func valloc,
                         virtual_free_and_unmap_func vfree)
{
   // heap size has to be a multiple of KMALLOC_MIN_HEAP_SIZE
   ASSERT((size & (KMALLOC_MIN_HEAP_SIZE - 1)) == 0);

   if (!linear_mapping) {
      // alloc block size has to be a multiple of PAGE_SIZE
      ASSERT((alloc_block_size & (PAGE_SIZE - 1)) == 0);
   } else {
      ASSERT(alloc_block_size == 0);
   }

   bzero(h, sizeof(*h));
   h->metadata_size = calculate_heap_metadata_size(size, min_block_size);

   h->valloc_and_map = valloc ? valloc : pg_alloc_and_map;
   h->vfree_and_unmap = vfree ? vfree : pg_free_and_unmap;

   if (!metadata_nodes) {
      // It is OK to pass NULL as 'metadata_nodes' if at least one heap exists.
      ASSERT(heaps[0].vaddr != 0);

      metadata_nodes = kmalloc(h->metadata_size);

      if (!metadata_nodes)
         return false;
   }

   h->vaddr = vaddr;
   h->size = size;
   h->min_block_size = min_block_size;
   h->alloc_block_size = alloc_block_size;
   h->metadata_nodes = metadata_nodes;

   h->heap_over_end = vaddr + size;
   h->heap_data_size_log2 = log2_for_power_of_2(size);
   h->alloc_block_size_log2 = log2_for_power_of_2(alloc_block_size);

   bzero(h->metadata_nodes, calculate_heap_metadata_size(size, min_block_size));
   h->linear_mapping = linear_mapping;
   return true;
}

void kmalloc_destroy_heap(kmalloc_heap *h)
{
   kfree2(h->metadata_nodes, h->metadata_size);
   bzero(h, sizeof(kmalloc_heap));
}

kmalloc_heap *kmalloc_heap_dup(kmalloc_heap *h)
{
   if (!h)
      return NULL;

   kmalloc_heap *new_heap = kmalloc(sizeof(kmalloc_heap));

   if (!new_heap)
      return NULL;

   memcpy(new_heap, h, sizeof(kmalloc_heap));

   new_heap->metadata_nodes = kmalloc(h->metadata_size);

   if (!new_heap->metadata_nodes) {
      kfree2(new_heap, sizeof(kmalloc_heap));
      return NULL;
   }

   memcpy(new_heap->metadata_nodes, h->metadata_nodes, h->metadata_size);
   return new_heap;
}

static size_t find_biggest_heap_size(uptr vaddr, uptr limit)
{
   size_t curr_max = 512 * MB;

   while (curr_max) {

      if (vaddr + curr_max <= limit)
         break;

      curr_max >>= 1; // divide by 2.
   }

   return curr_max;
}

extern uptr ramdisk_paddr;
extern size_t ramdisk_size;

static void
debug_print_heap_info(uptr vaddr, u32 heap_size, u32 min_block_size)
{
#if KMALLOC_HEAPS_CREATION_DEBUG

   u32 metadata_size = calculate_heap_metadata_size(heap_size, min_block_size);

   if (heap_size >= 4 * MB)
      printk("[heap: %p] size: %u MB, "
             "min block: %u, metadata size: %u KB\n",
             vaddr, heap_size / MB, min_block_size, metadata_size / KB);
   else
      printk("[heap: %p] size: %u KB, "
             "min block: %u, metadata size: %u KB\n",
             vaddr, heap_size / KB, min_block_size, metadata_size / KB);

#endif
}

static inline bool extra_low_mem_system(void)
{
   return get_phys_mem_mb() <= 4;
}

void debug_kmalloc_dump_mem_usage(void)
{
   static size_t heaps_alloc[KMALLOC_HEAPS_COUNT];

   printk("\n-------------------- kmalloc heaps --------------------\n");

   for (u32 i = 0; i < ARRAY_SIZE(heaps); i++) {

      if (!heaps[i].size)
         break;

      uptr size_kb = heaps[i].size / KB;
      uptr allocated_kb = heaps[i].mem_allocated / KB;

      printk("[heap %d] size: %u KB, allocated: %u KB [%u %%], diff: %d B\n",
             i, size_kb, allocated_kb, allocated_kb * 100 / size_kb,
             heaps[i].mem_allocated - heaps_alloc[i]);
   }

   for (u32 i = 0; i < ARRAY_SIZE(heaps); i++) {
      heaps_alloc[i] = heaps[i].mem_allocated;
   }
}

void init_kmalloc(void)
{
   uptr vaddr;
   size_t min_block_size;
   size_t first_heap_size;
   size_t first_metadata_size = KMALLOC_FIRST_METADATA_SIZE;
   void *first_heap_metadata = KMALLOC_FIRST_METADATA;

   ASSERT(!kmalloc_initialized);

   const uptr limit =
      KERNEL_BASE_VA + MIN(get_phys_mem_mb(), LINEAR_MAPPING_MB) * MB;

   const uptr base_vaddr =
      ramdisk_size
         ? (uptr)KERNEL_PA_TO_VA(ramdisk_paddr) + ramdisk_size
         : (uptr)KERNEL_PA_TO_VA(KERNEL_PADDR) + KERNEL_MAX_SIZE;

   if (!extra_low_mem_system()) {

      vaddr = round_up_at(base_vaddr, KMALLOC_MIN_HEAP_SIZE);
      first_heap_size = find_biggest_heap_size(vaddr, limit);

   } else {

      /*
       * If we have so few memory (<= 4 MB), than use the metadata space
       * in the low-mem for the first heap and use extra_low_mem_heap_metadata
       * for its metadata nodes.
       */

      first_metadata_size = sizeof(extra_low_mem_heap_metadata);
      first_heap_metadata = extra_low_mem_heap_metadata;
      first_heap_size = KMALLOC_FIRST_METADATA_SIZE;
      vaddr = (uptr)KMALLOC_FIRST_METADATA;
   }

   min_block_size =
      calculate_heap_min_block_size(first_heap_size, first_metadata_size);

   debug_print_heap_info(vaddr, first_heap_size, min_block_size);

   ASSERT(calculate_heap_metadata_size(
            first_heap_size, min_block_size) == first_metadata_size);

   bool success =
      kmalloc_create_heap(&heaps[0],
                          vaddr,
                          first_heap_size,
                          min_block_size,
                          0,    /* alloc_block_size */
                          true, /* linear mapping */
                          first_heap_metadata,
                          NULL,
                          NULL);

   VERIFY(success);

   used_heaps = 1;
   kmalloc_initialized = true;

   if (!extra_low_mem_system()) {
      vaddr = heaps[0].vaddr + heaps[0].size;
   } else {
      vaddr = round_up_at(base_vaddr, KMALLOC_MIN_HEAP_SIZE);
   }

   for (size_t i = 1; i < ARRAY_SIZE(heaps); i++) {

      const size_t heap_size = find_biggest_heap_size(vaddr, limit);

      if (heap_size < KMALLOC_MIN_HEAP_SIZE)
         break;

      min_block_size = calculate_heap_min_block_size(heap_size, heap_size / 32);
      debug_print_heap_info(vaddr, heap_size, min_block_size);

      bool success =
         kmalloc_create_heap(&heaps[i],
                             vaddr,
                             heap_size,
                             min_block_size,
                             0,    /* alloc_block_size */
                             true, /* linear mapping */
                             NULL, NULL, NULL);

      VERIFY(success);

      vaddr = heaps[i].vaddr + heaps[i].size;
      used_heaps++;
   }
}
