/* PANDABEGINCOMMENT
 * 
 * Authors:
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <map>
#include <vector>

#include "panda/plugin.h"

// #include "../callstack_instr/callstack_instr.h"
// #include "../callstack_instr/callstack_instr_ext.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C

#define TAG_LEN 64

extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
void phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr);
void phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr);
// void virt_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr);
// void virt_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr);
bool guest_hypercall_callback(CPUState *cpu);
}

static FILE *mem_report = NULL;
static uint32_t mem_bin = 16 * 1024 * 1024;
static uint32_t acc_bin = 100;
static const uint32_t total_mem = 1 * 1024 * 1024 * 1024;

static std::vector<uint32_t> kmap, umap;
static uint32_t acc_cnt = 0;

static std::map<std::string, uint32_t> tags;

// static void mem_callback(CPUState *env, target_ulong pc, target_ulong addr,
//                          size_t size, uint8_t *buf,
//                          std::map<prog_point, text_counter> &tracker) {
//     prog_point p = {};
// 
//     get_prog_point(env, &p);
// 
//     text_counter &tc = tracker[p];
//     for (unsigned int i = 0; i < size; i++) {
//         uint8_t val = ((uint8_t *)buf)[i];
//         tc.hist[val]++;
//     }
// 
//     return;
// }

// void virt_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
//                         size_t size, uint8_t *buf) {
//     fprintf(mem_report, "%d %c W 0x" TARGET_FMT_lx, env->nr_cores, panda_in_kernel(env) ? 'K' : 'U', addr);
// }
// 
// void virt_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
//                        size_t size, uint8_t *buf) {
//     fprintf(mem_report, "%d %c R 0x" TARGET_FMT_lx, env->nr_cores, panda_in_kernel(env) ? 'K' : 'U', addr);
// }

static
void flush_to_file(std::vector<uint32_t> &map) {
    for (uint32_t i = 0; i < map.size(); i ++) {
        if (map[i] != 0)
            fprintf(mem_report, "%u: %u, ", i, map[i]);

        fflush(mem_report);
        map[i] = 0;
    }
}

static
void record(target_ulong addr, bool in_kernel) {
    if (acc_cnt ++ % acc_bin == 0) {
        fprintf(mem_report, "K = {");
        flush_to_file(kmap);
        fprintf(mem_report, "}; U = {");
        flush_to_file(umap);
        fprintf(mem_report, "}\n");
    }

    if (addr > (target_ulong) total_mem)
        return;

    if (in_kernel)
        kmap[addr / mem_bin] ++;
    else
        umap[addr / mem_bin] ++;
}

void phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                        size_t size, uint8_t *buf) {
    record(addr, panda_in_kernel(env));
}

void phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       size_t size, uint8_t *buf) {
    record(addr, panda_in_kernel(env));
}

bool guest_hypercall_callback(CPUState *cpu) {
#if defined(TARGET_I386)
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    char tag[TAG_LEN] = "";
    if (env->regs[R_EAX] == 0x80001214) {
        // printf("value in ebx: " TARGET_FMT_lx "\n", env->regs[R_EBX]);
        if (panda_virtual_memory_rw(cpu, env->regs[R_EBX],
                                    (uint8_t *) tag, TAG_LEN, false)) {
            LOG_ERROR("Invalid name pointer in ebx");
            return true;
        }
        printf("PANDA[%s]:add tag %s.\n", PLUGIN_NAME, tag);
        if (mem_report)
            tags[tag] = acc_cnt;
        return true;
    }
#endif
    return false;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    // panda_require("callstack_instr");
    // if (!init_callstack_instr_api()) return false;

    // Need this to get EIP with our callbacks
    // panda_enable_precise_pc();

    /* Since instrumenting memory instructions is expensive, only enable that
     * in replay mode. */

    panda_arg_list *args = panda_get_args(PLUGIN_NAME);
    bool rr_on = panda_parse_bool_opt(args, "on", "enable memory instrumentation");
    acc_bin = panda_parse_uint32_opt(args, "acc_bin", 100, "number of accesses in one bin");
    mem_bin = panda_parse_uint32_opt(args, "mem_bin", 16, "size of memory bin (MB)") * 1024 * 1024;
    if (rr_on) {
        printf("Total Memory: %u, Memory bin size: %u, Access bin size: %u\n",
               total_mem, mem_bin, acc_bin);
        printf("Number of memory bins: %u\n", total_mem / mem_bin);

        mem_report = fopen("memtrace.txt", "w");
        if(!mem_report) {
            printf("Couldn't write to memtrace.txt:\n");
            perror("fopen");
            return false;
        }
        fprintf(mem_report, "acc_bin = %u; mem_bin = %u\n", acc_bin, mem_bin);
        kmap.resize(total_mem / mem_bin);
        umap.resize(total_mem / mem_bin);

        // Enable memory logging
        panda_enable_memcb();

        pcb.phys_mem_after_read = phys_mem_read_callback;
        panda_register_callback(self, PANDA_CB_PHYS_MEM_AFTER_READ, pcb);
        pcb.phys_mem_after_write = phys_mem_write_callback;
        panda_register_callback(self, PANDA_CB_PHYS_MEM_AFTER_WRITE, pcb);
    }

    // pcb.virt_mem_after_read = virt_mem_read_callback;
    // panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_READ, pcb);
    // pcb.virt_mem_after_write = virt_mem_write_callback;
    // panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_WRITE, pcb);

    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);

    return true;
}

// void write_report(FILE *report, std::map<prog_point,text_counter> &tracker) {
//     // Cross platform support: need to know how big a target_ulong is
//     uint32_t target_ulong_size = sizeof(target_ulong);
//     fwrite(&target_ulong_size, sizeof(uint32_t), 1, report);
//     uint32_t stack_type_size = sizeof(stack_type);
//     fwrite(&stack_type_size, sizeof(uint32_t), 1, report);
// 
//     std::map<prog_point,text_counter>::iterator it;
//     for(it = tracker.begin(); it != tracker.end(); it++) {
//         // prog_point parts
//         fwrite(&it->first.stackKind, stack_type_size, 1, report);
//         fwrite(&it->first.caller, target_ulong_size, 1, report);
//         fwrite(&it->first.pc, target_ulong_size, 1, report);
//         fwrite(&it->first.sidFirst, target_ulong_size, 1, report);
//         fwrite(&it->first.sidSecond, target_ulong_size, 1, report);
//         fwrite(&it->first.isKernelMode, sizeof(bool), 1, report);
// 
//         unsigned int hist[256] = {};
//         for(int i = 0; i < 256; i++) {
//             if (it->second.hist.find(i) != it->second.hist.end())
//                 hist[i] = it->second.hist[i];
//         }
//         fwrite(hist, sizeof(hist), 1, report);
//     }
// }

void uninit_plugin(void *self) {

    if (mem_report) {
        fprintf(mem_report, "tags = {");
        for (const auto &tag : tags)
            fprintf(mem_report, "\"%s\": %u, ", tag.first.c_str(), tag.second);
        fprintf(mem_report, "}");
        fclose(mem_report);
    }
}
