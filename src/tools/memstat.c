// Print iOS VM page counts (the iPod has no vm_stat).
#include <stdio.h>
#include <mach/mach.h>
int main(void) {
    vm_statistics_data_t s; mach_msg_type_number_t n = HOST_VM_INFO_COUNT;
    host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&s, &n);
    vm_size_t ps; host_page_size(mach_host_self(), &ps);
    double mb = ps / 1048576.0;
    printf("free %.1f MB  active %.1f  inactive %.1f  wired %.1f  purgeable %.1f  (page %lu)\n",
           s.free_count * mb, s.active_count * mb, s.inactive_count * mb, s.wire_count * mb, s.purgeable_count * mb, (unsigned long)ps);
    return 0;
}
