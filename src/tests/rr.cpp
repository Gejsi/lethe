#include <cstdio>
#include <vector>

#include "../swapper.h"
#include "volimem/mapper.h"

void test_basic_swap() {
  printf("\n--- Test: Basic Swap ---\n");

  auto page0 = (uptr *)(HEAP_START + PAGE_SIZE * 50);
  auto page1 = (uptr *)(HEAP_START + PAGE_SIZE * 60);

  printf("Attempting write to 0x%lx (page mapped: %s)\n", (uptr)page0,
         mapper_t::is_mapped(page0) ? "true" : "false");
  *page0 = 0xDEADBEEF;
  printf("Write succeeded: 0x%lx\n", *page0);
  ASSERT_EQ(0xDEADBEEF, *page0, "[page0] First write-read match");

  printf("Attempting write to 0x%lx (page mapped: %s)\n", (uptr)page1,
         mapper_t::is_mapped(page1) ? "true" : "false");
  *page1 = 0xCAFEBABE;
  printf("Write succeeded: 0x%lx\n", *page1);
  ASSERT_EQ(0xCAFEBABE, *page1, "[page1] First write-read match");

  printf("Attempting write from 0x%lx (page mapped: %s)\n", (uptr)page0,
         mapper_t::is_mapped(page0) ? "true" : "false");
  printf("Read old content: 0x%lx\n", *page0);
  *page0 = 0x777;
  printf("Write succeeded: 0x%lx\n", *page0);
  ASSERT_EQ(0x777, *page0, "[page0] Second write-read match");

  printf("\n--- Test: Basic Swap PASSED ---\n");
}

// Test eviction: Access more pages than cache slots
void test_eviction() {
  printf("\n--- Test: Eviction (Cache Slots: %zu) ---\n", NUM_PAGES);
  // We need to access NUM_PAGES + 1 distinct pages to guarantee an eviction
  // with the simple round-robin find_victim.
  usize pages_to_access = NUM_PAGES + 1;
  std::vector<uptr *> pages_vec(pages_to_access);
  printf("Will access %zu distinct pages to force eviction.\n",
         pages_to_access);

  for (usize i = 0; i < pages_to_access; ++i) {
    pages_vec[i] = (uptr *)(HEAP_START + PAGE_SIZE * i);
  }

  // Fill the cache + 1 page to force eviction of the first page(s)
  for (usize i = 0; i < pages_to_access; ++i) {
    uptr value = 0x1000 + i;
    printf("\nWriting unique value (0x%lx) to page %zu (addr: %p)...\n", value,
           i, (void *)pages_vec[i]);
    *pages_vec[i] = value;
    printf("Reading back from page %zu...\n", i);
    ASSERT_EQ(value, *pages_vec[i], "Read back page after initial write");
  }

  printf(
      "\nCache should now be full, and the earliest accessed pages evicted.\n");
  printf("With round-robin victim selection, accessing %zu pages means\n",
         pages_to_access);
  printf("page 0 (in cache slot 0) should have been evicted when accessing "
         "page %zu.\n",
         NUM_PAGES);

  // Access page 0 again. Since it should have been evicted, this MUST trigger a
  // fault and swap-in. The data originally written (0x1000) should be retrieved
  // from the swap_area.
  uptr expected_page0_value = 0x1000 + 0;
  printf(
      "\nAccessing page 0 (addr: %p) again. Expecting fault and swap-in...\n",
      (void *)pages_vec[0]);
  printf("Reading value from page 0...\n");
  ASSERT_EQ(expected_page0_value, *pages_vec[0],
            "Read back page 0 after expected eviction and swap-in");

  // Now, accessing page 0 likely evicted page 1 (if NUM_PAGES > 0).
  // Let's verify page 1.
  if (NUM_PAGES > 0) {
    uptr expected_page1_value = 0x1000 + 1;
    printf(
        "\nAccessing page 1 (addr: %p) again. Expecting fault and swap-in...\n",
        (void *)pages_vec[1]);
    printf("Reading value from page 1...\n");
    ASSERT_EQ(expected_page1_value, *pages_vec[1],
              "Read back page 1 after expected eviction and swap-in");
  }

  printf("--- Test: Eviction PASSED ---\n");
}

void test_data_integrity(usize num_distinct_pages = NUM_PAGES * 2,
                         usize num_accesses = NUM_PAGES * 10) {
  printf("\n--- Test: Data Integrity (Cache Slots: %zu, Distinct Pages: %zu, "
         "Accesses: %zu) ---\n",
         NUM_PAGES, num_distinct_pages, num_accesses);

  if (NUM_PAGES == 0) {
    printf("SKIPPING DATA INTEGRITY TEST: NUM_PAGES is 0.\n");
    return;
  }
  if (num_distinct_pages == 0) {
    printf("SKIPPING DATA INTEGRITY TEST: num_distinct_pages is 0.\n");
    return;
  }

  std::vector<uptr *> pages_vec(num_distinct_pages);
  std::vector<uptr> initial_values(num_distinct_pages);
  for (usize i = 0; i < num_distinct_pages; ++i) {
    pages_vec[i] = (uptr *)(HEAP_START + PAGE_SIZE * i);
    initial_values[i] = 0xBADF00D + i; // Unique initial value
  }

  // Write unique initial values. This will fill the cache and cause evictions.
  printf("Performing initial writes to %zu pages...\n", num_distinct_pages);
  for (usize i = 0; i < num_distinct_pages; ++i) {
    // printf("Initial write to page %zu (addr: %p)...\n", i, pages_vec[i]);
    *pages_vec[i] = initial_values[i];
    // Read back immediately to confirm write worked
    ASSERT_EQ(initial_values[i], *pages_vec[i],
              "Read back after initial write");
  }
  printf("Initial writes completed.\n");

  // Access pages randomly many times to force swaps
  printf("Performing %zu random accesses...\n", num_accesses);
  srand(123);
  for (usize i = 0; i < num_accesses; ++i) {
    usize page_idx = (usize)rand() % num_distinct_pages;
    // expect the initially written value
    uptr expected_value = initial_values[page_idx];

    if ((i % (num_accesses / 10)) == 0 || i == num_accesses - 1) {
      printf(" Access %zu/%zu: Touching page %zu (addr: %p). Expecting value "
             "0x%lx\n",
             i + 1, num_accesses, page_idx, (void *)pages_vec[page_idx],
             expected_value);
    }

    // Read, check, write back the same value,
    // to potentially mark dirty if tracking
    uptr read_value = *pages_vec[page_idx];
    ASSERT_EQ(expected_value, read_value, "Read check during random access");
    *pages_vec[page_idx] = expected_value; // Write same value back
  }
  printf("Pseudo-random accesses completed.\n");

  // Final check: Verify all pages still have their correct initial values
  printf("Performing final data verification for all %zu pages...\n",
         num_distinct_pages);
  for (usize i = 0; i < num_distinct_pages; ++i) {
    uptr expected_value = initial_values[i];
    uptr final_read_value = *pages_vec[i]; // Access one last time
    ASSERT_EQ(expected_value, final_read_value, "Final verification");
  }

  printf("--- Test: Data Integrity PASSED ---\n");
}
