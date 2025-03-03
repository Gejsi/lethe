#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "volimem/mapper.h"
#include "volimem/vcpu.h"
#include "volimem/volimem.h"

#define UNUSED(x) (void)(x)

double start;
double end;

double gettime() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1e0 +
         static_cast<double>(ts.tv_nsec) * 1e-9;
}

constexpr s_volimem_config_t voli_config{.log_level = DEBUG,
                                         .host_page_type = VOLIMEM_NORMAL_PAGES,
                                         .guest_page_type = VOLIMEM_LARGE_PAGES,
                                         .ring_level = 0};

void start_fun(void *args) {
  UNUSED(args);

  end = gettime();
  printf("Hello from the VM => %lf\n", end - start);
  printf("I'm running on the vCPU apic %d\n", local_vcpu->lapic_id);
  printf("And my root page table is at %p\n", (void *)mapper_t::get_root());
  printf("And start is at %p virtually, and 0x%lx physically\n", (void *)&start,
         mapper_t::gva_to_gpa(&start));
  printf("End of callback\n");
}

int main() {
  printf("Entering, start is at %p\n", (void *)&start);
  start = gettime();

  volimem_set_config(&voli_config);
  int res = volimem_start(nullptr, start_fun);

  return res;
}
