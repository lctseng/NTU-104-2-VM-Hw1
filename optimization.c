/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdlib.h>
#include "exec-all.h"
#include "tcg-op.h"
#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"
#include "optimization.h"

#define SHACK_INDEX_BITS 16


extern uint8_t *optimization_ret_addr;

/*
 * Shadow Stack
 */
list_t *shadow_hash_list;


static inline void init_shack_hash(CPUState *env){
    int i;
    env->shadow_hash_list = malloc( (2 << SHACK_INDEX_BITS) * sizeof(struct shadow_pair*) );
    for(i=0;i< (2 << SHACK_INDEX_BITS);i++){
        struct shadow_pair* head = malloc(sizeof(struct shadow_pair));
        head->guest_eip = 0;
        head->l.next = NULL;
        head->l.prev = NULL;
        ((struct shadow_pair**)env->shadow_hash_list)[i] = head;
    }
}

static inline void shack_init(CPUState *env)
{
    int i;
    env->shack = (uint64_t*)malloc(SHACK_SIZE * sizeof(uint64_t));
    env->shack_top = env->shack;
    env->shack_end = env->shack + SHACK_SIZE;
    env->shadow_ret_count = 0;
    env->shadow_ret_addr = (unsigned long*)malloc(SHACK_SIZE * sizeof(unsigned long));
    for(i = 0; i< SHACK_SIZE; ++i){ // i : store the position for host addr
        env->shack[i] = (uint64_t)(unsigned long)(env->shadow_ret_addr + i); // the lower 32-bit will be the address of host slot
        env->shadow_ret_addr[i] = 0;
    }
    // hash table
    init_shack_hash(env);
}

struct shadow_pair** get_shadow_pair_head_from_hash(CPUState *env, target_ulong guest_eip){
    int index = guest_eip >> SHACK_INDEX_BITS | ((guest_eip << SHACK_INDEX_BITS) >> SHACK_INDEX_BITS);
    return (((struct shadow_pair **)env->shadow_hash_list) + index);
}


unsigned long lookup_shadow_ret_addr(CPUState *env, target_ulong pc){
    static int count = 0;
    struct shadow_pair *sp = *get_shadow_pair_head_from_hash(env, pc);
    // search the head
    int current_count = 0;
    while( sp->l.next){ // has next (not the ending one)
        ++count;
        ++current_count;
        if(sp->guest_eip == pc){
            //fprintf(stderr,"lookup count: %d, total count: %d\n", current_count ,count);
            return *sp->shadow_slot;
        }
        // go to next
        sp = list_entry(sp->l.next, struct shadow_pair, l);
    }
    //fprintf(stderr,"lookup count: %d, total count: %d\n", current_count ,count);
    return 0;
}


/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
 void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip)
{
    static int count = 0;
    struct shadow_pair *sp = *get_shadow_pair_head_from_hash(env, guest_eip);
    // search the head
    int current_count = 0;
    while( sp->l.next){ // has next (not the ending one)
        ++current_count;
        ++count;
        if(sp->guest_eip == guest_eip){
            *sp->shadow_slot = (unsigned long)(host_eip);
            break;
        }
        // go to next
        sp = list_entry(sp->l.next, struct shadow_pair, l);
    }
    //fprintf(stderr,"search count: %d, total count: %d\n", current_count ,count);
}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env)
{
}

void insert_unresolved_eip(CPUState *env, target_ulong next_eip, unsigned long *slot){
    /*
    // check duplicate
    static int count = 0;
    {
        struct shadow_pair *sp = get_shadow_pair_head_from_hash(env, next_eip);
        // search the head
        int current_count = 0;
        while( sp->l.next){ // has next (not the ending one)
            if(sp->guest_eip == next_eip){
                ++current_count;
                ++count;
            }
            // go to next
            sp = list_entry(sp->l.next, struct shadow_pair, l);
        }
        fprintf(stderr,"duplicate guest eip count: %d, total count: %d\n", current_count ,count);
    }
    */
    struct shadow_pair* sp = (struct shadow_pair*)malloc(sizeof(struct shadow_pair));
    struct shadow_pair** old_sp_ptr = get_shadow_pair_head_from_hash(env, next_eip);
    sp->guest_eip = next_eip;
    sp->shadow_slot = slot;
    sp->l.prev = NULL;
    sp->l.next = &((*old_sp_ptr)->l);
    (*old_sp_ptr)->l.prev = &sp->l;
    *old_sp_ptr = sp;
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    // label
    int label_do_push = gen_new_label(); 
    // prepare registers
    TCGv_ptr temp_shack_end = tcg_temp_local_new_ptr(); // store shack end
    TCGv_ptr temp_shack_top = tcg_temp_local_new_ptr(); // store shack top
    // load common values
    tcg_gen_ld_ptr(temp_shack_end, cpu_env, offsetof(CPUState, shack_end));
    tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
    // check shack full?
    tcg_gen_brcond_ptr(TCG_COND_NE,temp_shack_top,temp_shack_end,label_do_push); // if not full
    // flush here
    TCGv_ptr temp_shack_start = tcg_temp_new_ptr(); // store shack start
    //tcg_en_st_tl(tcg_const_tl(0), cpu_env, offsetof(CPUState, shadow_ret_count)); // reset ret count
    tcg_gen_ld_ptr(temp_shack_start, cpu_env, offsetof(CPUState, shack));
    tcg_gen_mov_tl(temp_shack_top, temp_shack_start);
    tcg_temp_free_ptr(temp_shack_start);
    // end of flush
    gen_set_label(label_do_push);
    // do push here
    // push guest eip
    tcg_gen_st_ptr(tcg_const_tl(next_eip), temp_shack_top, 0); // store guest eip
    tcg_gen_addi_ptr(temp_shack_top, temp_shack_top, sizeof(uint64_t)); // increase top
    // push host addr slot
    if(env->shadow_ret_count == SHACK_SIZE){
        fprintf(stderr,"Max shadow_ret_count exceeds: %d, return to 0\n", env->shadow_ret_count);
        env->shadow_ret_count = 0;
    }
    else{
        //fprintf(stderr,"shadow_ret_count: %d\n", env->shadow_ret_count);
    }
    unsigned long *slot = &env->shadow_ret_addr[env->shadow_ret_count++];
    tcg_gen_st_ptr(tcg_const_ptr((unsigned long)slot), temp_shack_top, sizeof(unsigned long)); // store host addr slot
    tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top)); // store back top
    // check if we need to translate this addr?
    unsigned long host_eip = lookup_shadow_ret_addr(env, next_eip);
    if(host_eip > 0){
        // just add the result into slot
        *slot = host_eip;
    }
    else{
        // insert unresolved eip
        insert_unresolved_eip(env, next_eip, slot);
    }
    // clean up
    tcg_temp_free_ptr(temp_shack_top);
    tcg_temp_free_ptr(temp_shack_end);
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip)
{
    // labels
    int label_end = gen_new_label();
    // prepare registers
    TCGv_ptr temp_shack_start = tcg_temp_local_new_ptr(); // store shack start
    TCGv_ptr temp_shack_top = tcg_temp_local_new_ptr(); // store shack top
    TCGv eip_on_shack = tcg_temp_local_new();
    TCGv_ptr host_slot_addr = tcg_temp_local_new();
    TCGv_ptr host_addr = tcg_temp_new();
    // load common values
    tcg_gen_ld_ptr(temp_shack_start, cpu_env, offsetof(CPUState, shack));
    tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
    // check if stack empty?
    tcg_gen_brcond_ptr(TCG_COND_EQ,temp_shack_top,temp_shack_start,label_end);
    // stack not empty, pop one 
    tcg_gen_subi_ptr(temp_shack_top, temp_shack_top, sizeof(uint64_t)); // decrease top
    tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top)); // store back top
    tcg_gen_ld_tl(eip_on_shack, temp_shack_top, 0); // get eip
    // check if the same?
    tcg_gen_brcond_ptr(TCG_COND_NE,tcg_const_tl(next_eip),eip_on_shack,label_end); // go to "end" if not the same
    tcg_gen_ld_tl(host_slot_addr, temp_shack_top, sizeof(unsigned long)); // get slot addr
    tcg_gen_ld_ptr(host_addr, host_slot_addr, 0) ; // get host addr
    // jump!
    *gen_opc_ptr++ = INDEX_op_jmp;
    *gen_opparam_ptr++ = GET_TCGV_I32(host_addr);
    // label: end
    gen_set_label(label_end);
    tcg_temp_free(eip_on_shack);
    tcg_temp_free_ptr(host_slot_addr);
    tcg_temp_free_ptr(host_addr);
    tcg_temp_free_ptr(temp_shack_top);
    tcg_temp_free_ptr(temp_shack_start);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip)
{
    return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb)
{
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env)
{
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env)
{
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
