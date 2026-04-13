#include <sched.h>
#include <err.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include "../include/cache.hpp"

void store(void *addr)
{
        int cpu_id = sched_getcpu();
}

void load(void *addr)
{
        int cpu_id = sched_getcpu();

}

void get_cache_params(int cpuid, int ix, int level, cache_type type, )
{
}

void get_cpu_params(int cpuid)
{
        DIR             *dirp;
        struct dirent   *dir;
        char            buf[256];

        snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu%d/cache", 
                 cpuid);
        if ((dirp = opendir(buf)) == nullptr)
                err(EXIT_FAILURE, "opendir");

        while ((dir = readdir(dirp)) != nullptr) {
                if (!strncmp(dir->d_name, "index", 5)) {
                        int ix = dir->d_name[5] - '0';
                        get_cache_params(cpuid, ix);
                }
        }
}

__attribute__((constructor))
void setup()
{
        DIR             *cache_dirp, *ix_dirp;
        struct dirent   *cache_dir;
        int             nr_cpus = get_nprocs();

        /* Probe /sys/devices/system/cpu */
        for (int i = 0; i < nr_cpus; i++) {

        }
}

__attribute__((destructor))
void cleanup()
{
}
