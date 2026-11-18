// List processes with resident memory, CPU time (s) and page-ins (the iPod has no ps).
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <stdint.h>
#define PROC_PIDTASKINFO 4
struct proc_taskinfo { uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system, pti_threads_user, pti_threads_system;
    int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent, pti_messages_received, pti_syscalls_mach, pti_syscalls_unix, pti_csw, pti_threadnum, pti_numrunning, pti_priority; };
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int main(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0}; size_t sz;
    sysctl(mib, 4, NULL, &sz, NULL, 0);
    struct kinfo_proc *p = malloc(sz); sysctl(mib, 4, p, &sz, NULL, 0);
    int n = sz / sizeof *p; double tot = 0;
    for (int i = 0; i < n; i++) {
        struct proc_taskinfo ti;
        if (proc_pidinfo(p[i].kp_proc.p_pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) <= 0) continue;
        // on this ARM, task times are mach absolute time units (24 MHz)
        printf("%7.1f MB  %5d %-16s cpu %8.2f  pageins %d\n", ti.pti_resident_size / 1048576.0, p[i].kp_proc.p_pid, p[i].kp_proc.p_comm,
               (ti.pti_total_user + ti.pti_total_system) / 24e6, ti.pti_pageins);
        tot += ti.pti_resident_size / 1048576.0;
    }
    printf("sum %.1f MB over %d procs\n", tot, n);
    return 0;
}
