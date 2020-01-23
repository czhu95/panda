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

#include "panda/plugin.h"

// #include "../callstack_instr/callstack_instr.h"
// #include "../callstack_instr/callstack_instr_ext.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
bool guest_hypercall_callback(CPUState *cpu);
}

static const char* name;
#if defined(TARGET_I386)
static bool in_record = false;
#endif

bool guest_hypercall_callback(CPUState *cpu) {
#if defined(TARGET_I386)
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    if (env->regs[R_EAX] == 0x80001212) {
        if (!in_record) {
            printf("PANDA[%s]:begin record %s.\n", PLUGIN_NAME, name);
            panda_record_begin(name, NULL);
            in_record = true;
        } else {
            printf("PANDA[%s]:already in record.\n", PLUGIN_NAME);
        }
        return true;
    }
    if (env->regs[R_EAX] == 0x80001213) {
        if (in_record) {
            printf("PANDA[%s]:end record %s.\n", PLUGIN_NAME, name);
            panda_record_end();
            in_record = false;
        } else {
            printf("PANDA[%s]:not in record.\n", PLUGIN_NAME);
        }
        return true;
    }
    return false;
#else
    return false;
#endif
}

bool init_plugin(void *self) {
    panda_cb pcb;

    panda_arg_list *args = panda_get_args(PLUGIN_NAME);
    name = panda_parse_string(args, "name", NULL);

    if (!name) {
        LOG_ERROR("No name argument provided.");
    }

    // panda_require("callstack_instr");
    // if (!init_callstack_instr_api()) return false;

    // Need this to get EIP with our callbacks
    // panda_enable_precise_pc();
    // Enable memory logging
    // panda_enable_memcb();

    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);

    LOG_INFO(PLUGIN_NAME " initialization complete.");
    return true;
}

void uninit_plugin(void *self) {

}
