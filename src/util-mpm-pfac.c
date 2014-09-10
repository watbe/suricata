/* Copyright (C) 2007-2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 *         First iteration of aho-corasick MPM from -
 *
 *         Efficient String Matching: An Aid to Bibliographic Search
 *         Alfred V. Aho and Margaret J. Corasick
 *
 *         - Uses the delta table for calculating transitions, instead of having
 *           separate goto and failure transitions.
 *         - If we cross 2 ** 16 states, we use 4 bytes in the transition table
 *           to hold each state, otherwise we use 2 bytes.
 *         - This version of the MPM is heavy on memory, but it performs well.
 *           If you can fit the ruleset with this mpm on your box without hitting
 *           swap, this is the MPM to go for.
 *
 * \todo - Do a proper analyis of our existing MPMs and suggest a good one based
 *         on the pattern distribution and the expected traffic(say http).
 *       - Tried out loop unrolling without any perf increase.  Need to dig deeper.
 *       - Irrespective of whether we cross 2 ** 16 states or not,shift to using
 *         uint32_t for state type, so that we can integrate it's status as a
 *         final state or not in the topmost byte.  We are already doing it if
 *         state_count is > 2 ** 16.
 *       - Test case-senstive patterns if they have any ascii chars.  If they
 *         don't treat them as nocase.
 *       - Carry out other optimizations we are working on.  hashes, compression.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"

#include "conf.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-memcmp.h"
#include "util-mpm-pfac.h"
#include "util-memcpy.h"

#ifdef __SC_CUDA_SUPPORT__

#include "util-mpm.h"
#include "tm-threads.h"
#include "detect-engine-mpm.h"
#include "util-cuda.h"
#include "util-cuda-handlers.h"
#endif /* __SC_CUDA_SUPPORT__ */

void SCPFACInitCtx(MpmCtx *);
void SCPFACInitThreadCtx(MpmCtx *, MpmThreadCtx *, uint32_t);
void SCPFACDestroyCtx(MpmCtx *);
void SCPFACDestroyThreadCtx(MpmCtx *, MpmThreadCtx *);
int SCPFACAddPatternCI(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t,
                     uint32_t, uint32_t, uint8_t);
int SCPFACAddPatternCS(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t,
                     uint32_t, uint32_t, uint8_t);
int SCPFACPreparePatterns(MpmCtx *mpm_ctx);
uint32_t SCPFACSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                    PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen);
void SCPFACPrintInfo(MpmCtx *mpm_ctx);
void SCPFACPrintSearchStats(MpmThreadCtx *mpm_thread_ctx);
void SCPFACRegisterTests(void);

/* a placeholder to denote a failure transition in the goto table */
#define SC_PFAC_FAIL (-1)
/* size of the hash table used to speed up pattern insertions initially */
#define INIT_HASH_SIZE 65536

#define STATE_QUEUE_CONTAINER_SIZE 65536

static int construct_both_16_and_32_state_tables = 0;

/**
 * \brief Helper structure used by AC during state table creation
 */
typedef struct StateQueue_ {
    int32_t store[STATE_QUEUE_CONTAINER_SIZE];
    int top;
    int bot;
} StateQueue;

/**
 * \internal
 * \brief Initialize the AC context with user specified conf parameters.  We
 *        aren't retrieving anything for AC conf now, but we will certainly
 *        need it, when we customize AC.
 */
static void SCPFACGetConfig()
{
    //ConfNode *ac_conf;
    //const char *hash_val = NULL;

    //ConfNode *pm = ConfGetNode("pattern-matcher");

    return;
}

/**
 * \internal
 * \brief Creates a hash of the pattern.  We use it for the hashing process
 *        during the initial pattern insertion time, to cull duplicate sigs.
 *
 * \param pat    Pointer to the pattern.
 * \param patlen Pattern length.
 *
 * \retval hash A 32 bit unsigned hash.
 */
static inline uint32_t SCPFACInitHashRaw(uint8_t *pat, uint16_t patlen)
{
    uint32_t hash = patlen * pat[0];
    if (patlen > 1)
        hash += pat[1];

    return (hash % INIT_HASH_SIZE);
}

/**
 * \internal
 * \brief Looks up a pattern.  We use it for the hashing process during the
 *        the initial pattern insertion time, to cull duplicate sigs.
 *
 * \param ctx    Pointer to the AC ctx.
 * \param pat    Pointer to the pattern.
 * \param patlen Pattern length.
 * \param flags  Flags.  We don't need this.
 *
 * \retval hash A 32 bit unsigned hash.
 */
static inline SCPFACPattern *SCPFACInitHashLookup(SCPFACCtx *ctx, uint8_t *pat,
                                              uint16_t patlen, char flags,
                                              uint32_t pid)
{
    uint32_t hash = SCPFACInitHashRaw(pat, patlen);

    if (ctx->init_hash == NULL) {
        return NULL;
    }

    SCPFACPattern *t = ctx->init_hash[hash];
    for ( ; t != NULL; t = t->next) {
        if (t->id == pid)
            return t;
    }

    return NULL;
}

/**
 * \internal
 * \brief Allocs a new pattern instance.
 *
 * \param mpm_ctx Pointer to the mpm context.
 *
 * \retval p Pointer to the newly created pattern.
 */
static inline SCPFACPattern *SCPFACAllocPattern(MpmCtx *mpm_ctx)
{
    SCPFACPattern *p = SCMalloc(sizeof(SCPFACPattern));
    if (unlikely(p == NULL)) {
        exit(EXIT_FAILURE);
    }
    memset(p, 0, sizeof(SCPFACPattern));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(SCPFACPattern);

    return p;
}

/**
 * \internal
 * \brief Used to free SCPFACPattern instances.
 *
 * \param mpm_ctx Pointer to the mpm context.
 * \param p       Pointer to the SCPFACPattern instance to be freed.
 * \param free    Free the above pointer or not.
 */
static inline void SCPFACFreePattern(MpmCtx *mpm_ctx, SCPFACPattern *p)
{
    if (p != NULL && p->cs != NULL && p->cs != p->ci) {
        SCFree(p->cs);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p != NULL && p->ci != NULL) {
        SCFree(p->ci);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p != NULL && p->original_pat != NULL) {
        SCFree(p->original_pat);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p != NULL) {
        SCFree(p);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= sizeof(SCPFACPattern);
    }
    return;
}

static inline uint32_t SCPFACInitHash(SCPFACPattern *p)
{
    uint32_t hash = p->len * p->original_pat[0];
    if (p->len > 1)
        hash += p->original_pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline int SCPFACInitHashAdd(SCPFACCtx *ctx, SCPFACPattern *p)
{
    uint32_t hash = SCPFACInitHash(p);

    if (ctx->init_hash == NULL) {
        return 0;
    }

    if (ctx->init_hash[hash] == NULL) {
        ctx->init_hash[hash] = p;
        return 0;
    }

    SCPFACPattern *tt = NULL;
    SCPFACPattern *t = ctx->init_hash[hash];

    /* get the list tail */
    do {
        tt = t;
        t = t->next;
    } while (t != NULL);

    tt->next = p;

    return 0;
}

/**
 * \internal
 * \brief Add a pattern to the mpm-ac context.
 *
 * \param mpm_ctx Mpm context.
 * \param pat     Pointer to the pattern.
 * \param patlen  Length of the pattern.
 * \param pid     Pattern id
 * \param sid     Signature id (internal id).
 * \param flags   Pattern's MPM_PATTERN_* flags.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int SCPFACAddPattern(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
                          uint16_t offset, uint16_t depth, uint32_t pid,
                          uint32_t sid, uint8_t flags)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;

    SCLogDebug("Adding pattern for ctx %p, patlen %"PRIu16" and pid %" PRIu32,
               ctx, patlen, pid);

    if (patlen == 0) {
        SCLogWarning(SC_ERR_INVALID_ARGUMENTS, "pattern length 0");
        return 0;
    }

    /* check if we have already inserted this pattern */
    SCPFACPattern *p = SCPFACInitHashLookup(ctx, pat, patlen, flags, pid);
    if (p == NULL) {
        SCLogDebug("Allocing new pattern");

        /* p will never be NULL */
        p = SCPFACAllocPattern(mpm_ctx);

        p->len = patlen;
        p->flags = flags;
        p->id = pid;

        p->original_pat = SCMalloc(patlen);
        if (p->original_pat == NULL)
            goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy(p->original_pat, pat, patlen);

        p->ci = SCMalloc(patlen);
        if (p->ci == NULL)
            goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy_tolower(p->ci, pat, patlen);

        /* setup the case sensitive part of the pattern */
        if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
            /* nocase means no difference between cs and ci */
            p->cs = p->ci;
        } else {
            if (memcmp(p->ci, pat, p->len) == 0) {
                /* no diff between cs and ci: pat is lowercase */
                p->cs = p->ci;
            } else {
                p->cs = SCMalloc(patlen);
                if (p->cs == NULL)
                    goto error;
                mpm_ctx->memory_cnt++;
                mpm_ctx->memory_size += patlen;
                memcpy(p->cs, pat, patlen);
            }
        }

        /* put in the pattern hash */
        SCPFACInitHashAdd(ctx, p);

        //if (mpm_ctx->pattern_cnt == 65535) {
        //    SCLogError(SC_ERR_AHO_CORASICK, "Max search words reached.  Can't "
        //               "insert anymore.  Exiting");
        //    exit(EXIT_FAILURE);
        //}
        mpm_ctx->pattern_cnt++;

        if (mpm_ctx->maxlen < patlen)
            mpm_ctx->maxlen = patlen;

        if (mpm_ctx->minlen == 0) {
            mpm_ctx->minlen = patlen;
        } else {
            if (mpm_ctx->minlen > patlen)
                mpm_ctx->minlen = patlen;
        }

        /* we need the max pat id */
        if (pid > ctx->max_pat_id)
            ctx->max_pat_id = pid;
    }

    return 0;

error:
    SCPFACFreePattern(mpm_ctx, p);
    return -1;
}

/**
 * \internal
 * \brief Initialize a new state in the goto and output tables.
 *
 * \param mpm_ctx Pointer to the mpm context.
 *
 * \retval The state id, of the newly created state.
 */
static inline int SCPFACInitNewState(MpmCtx *mpm_ctx)
{
    void *ptmp;
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int ascii_code = 0;
    int size = 0;

    /* reallocate space in the goto table to include a new state */
    size = (ctx->state_count + 1) * ctx->single_state_size;
    ptmp = SCRealloc(ctx->goto_table, size);
    if (ptmp == NULL) {
        SCFree(ctx->goto_table);
        ctx->goto_table = NULL;
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    ctx->goto_table = ptmp;

    /* set all transitions for the newly assigned state as FAIL transitions */
    for (ascii_code = 0; ascii_code < 256; ascii_code++) {
        ctx->goto_table[ctx->state_count][ascii_code] = SC_PFAC_FAIL;
    }

    /* reallocate space in the output table for the new state */
    size = (ctx->state_count + 1) * sizeof(SCPFACOutputTable);
    ptmp = SCRealloc(ctx->output_table, size);
    if (ptmp == NULL) {
        SCFree(ctx->output_table);
        ctx->output_table = NULL;
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    ctx->output_table = ptmp;

    memset(ctx->output_table + ctx->state_count, 0, sizeof(SCPFACOutputTable));

    /* \todo using it temporarily now during dev, since I have restricted
     *       state var in SCPFACCtx->state_table to uint16_t. */
    //if (ctx->state_count > 65536) {
    //    printf("state count exceeded\n");
    //    exit(EXIT_FAILURE);
    //}

    return ctx->state_count++;
}

/**
 * \internal
 * \brief Adds a pid to the output table for a state.
 *
 * \param state   The state to whose output table we should add the pid.
 * \param pid     The pattern id to add.
 * \param mpm_ctx Pointer to the mpm context.
 */
static void SCPFACSetOutputState(int32_t state, uint32_t pid, MpmCtx *mpm_ctx)
{
    void *ptmp;
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    SCPFACOutputTable *output_state = &ctx->output_table[state];
    uint32_t i = 0;

    for (i = 0; i < output_state->no_of_entries; i++) {
        if (output_state->pids[i] == pid)
            return;
    }

    output_state->no_of_entries++;
    ptmp = SCRealloc(output_state->pids,
                     output_state->no_of_entries * sizeof(uint32_t));
    if (ptmp == NULL) {
        SCFree(output_state->pids);
        output_state->pids = NULL;
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    output_state->pids = ptmp;

    output_state->pids[output_state->no_of_entries - 1] = pid;

    return;
}

/**
 * \brief Helper function used by SCPFACCreateGotoTable.  Adds a pattern to the
 *        goto table.
 *
 * \param pattern     Pointer to the pattern.
 * \param pattern_len Pattern length.
 * \param pid         The pattern id, that corresponds to this pattern.  We
 *                    need it to updated the output table for this pattern.
 * \param mpm_ctx     Pointer to the mpm context.
 */
static inline void SCPFACEnter(uint8_t *pattern, uint16_t pattern_len, uint32_t pid,
                             MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int32_t state = 0;
    int32_t newstate = 0;
    int i = 0;
    int p = 0;

    /* walk down the trie till we have a match for the pattern prefix */
    state = 0;
    for (i = 0; i < pattern_len; i++) {
        if (ctx->goto_table[state][pattern[i]] != SC_PFAC_FAIL) {
            state = ctx->goto_table[state][pattern[i]];
        } else {
            break;
        }
    }

    /* add the non-matching pattern suffix to the trie, from the last state
     * we left off */
    for (p = i; p < pattern_len; p++) {
        newstate = SCPFACInitNewState(mpm_ctx);
        ctx->goto_table[state][pattern[p]] = newstate;
        state = newstate;
    }

    /* add this pattern id, to the output table of the last state, where the
     * pattern ends in the trie */
    SCPFACSetOutputState(state, pid, mpm_ctx);

    return;
}

/**
 * \internal
 * \brief Create the goto table.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
static inline void SCPFACCreateGotoTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    uint32_t i = 0;

    /* add each pattern to create the goto table */
    for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
        SCPFACEnter(ctx->parray[i]->ci, ctx->parray[i]->len,
                  ctx->parray[i]->id, mpm_ctx);
    }

    int ascii_code = 0;
    for (ascii_code = 0; ascii_code < 256; ascii_code++) {
        if (ctx->goto_table[0][ascii_code] == SC_PFAC_FAIL) {
            ctx->goto_table[0][ascii_code] = 0;
        }
    }

    return;
}

static inline void SCPFACDetermineLevel1Gap(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    uint32_t u = 0;

    int map[256];
    memset(map, 0, sizeof(map));

    for (u = 0; u < mpm_ctx->pattern_cnt; u++)
        map[ctx->parray[u]->ci[0]] = 1;

    for (u = 0; u < 256; u++) {
        if (map[u] == 0)
            continue;
        int32_t newstate = SCPFACInitNewState(mpm_ctx);
        ctx->goto_table[0][u] = newstate;
    }

    return;
}

static inline int SCPFACStateQueueIsEmpty(StateQueue *q)
{
    if (q->top == q->bot)
        return 1;
    else
        return 0;
}

static inline void SCPFACEnqueue(StateQueue *q, int32_t state)
{
    int i = 0;

    /*if we already have this */
    for (i = q->bot; i < q->top; i++) {
        if (q->store[i] == state)
            return;
    }

    q->store[q->top++] = state;

    if (q->top == STATE_QUEUE_CONTAINER_SIZE)
        q->top = 0;

    if (q->top == q->bot) {
        SCLogCritical(SC_ERR_AHO_CORASICK, "Just ran out of space in the queue.  "
                      "Fatal Error.  Exiting.  Please file a bug report on this");
        exit(EXIT_FAILURE);
    }

    return;
}

static inline int32_t SCPFACDequeue(StateQueue *q)
{
    if (q->bot == STATE_QUEUE_CONTAINER_SIZE)
        q->bot = 0;

    if (q->bot == q->top) {
        SCLogCritical(SC_ERR_AHO_CORASICK, "StateQueue behaving weirdly.  "
                      "Fatal Error.  Exiting.  Please file a bug report on this");
        exit(EXIT_FAILURE);
    }

    return q->store[q->bot++];
}

/*
#define SCPFACStateQueueIsEmpty(q) (((q)->top == (q)->bot) ? 1 : 0)

#define SCPFACEnqueue(q, state) do { \
                                  int i = 0; \
                                             \
                                  for (i = (q)->bot; i < (q)->top; i++) { \
                                      if ((q)->store[i] == state)       \
                                      return; \
                                  } \
                                    \
                                  (q)->store[(q)->top++] = state;   \
                                                                \
                                  if ((q)->top == STATE_QUEUE_CONTAINER_SIZE) \
                                      (q)->top = 0;                     \
                                                                        \
                                  if ((q)->top == (q)->bot) {           \
                                  SCLogCritical(SC_ERR_AHO_CORASICK, "Just ran out of space in the queue.  " \
                                                "Fatal Error.  Exiting.  Please file a bug report on this"); \
                                  exit(EXIT_FAILURE);                   \
                                  }                                     \
                              } while (0)

#define SCPFACDequeue(q) ( (((q)->bot == STATE_QUEUE_CONTAINER_SIZE)? ((q)->bot = 0): 0), \
                         (((q)->bot == (q)->top) ?                      \
                          (printf("StateQueue behaving "                \
                                         "weirdly.  Fatal Error.  Exiting.  Please " \
                                         "file a bug report on this"), \
                           exit(EXIT_FAILURE)) : 0), \
                         (q)->store[(q)->bot++])     \
*/

/**
 * \internal
 * \brief Club the output data from 2 states and store it in the 1st state.
 *        dst_state_data = {dst_state_data} UNION {src_state_data}
 *
 * \param dst_state First state(also the destination) for the union operation.
 * \param src_state Second state for the union operation.
 * \param mpm_ctx Pointer to the mpm context.
 */
static inline void SCPFACClubOutputStates(int32_t dst_state, int32_t src_state,
                                        MpmCtx *mpm_ctx)
{
    void *ptmp;
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    uint32_t i = 0;
    uint32_t j = 0;

    SCPFACOutputTable *output_dst_state = &ctx->output_table[dst_state];
    SCPFACOutputTable *output_src_state = &ctx->output_table[src_state];

    for (i = 0; i < output_src_state->no_of_entries; i++) {
        for (j = 0; j < output_dst_state->no_of_entries; j++) {
            if (output_src_state->pids[i] == output_dst_state->pids[j]) {
                break;
            }
        }
        if (j == output_dst_state->no_of_entries) {
            output_dst_state->no_of_entries++;

            ptmp = SCRealloc(output_dst_state->pids,
                             (output_dst_state->no_of_entries * sizeof(uint32_t)));
            if (ptmp == NULL) {
                SCFree(output_dst_state->pids);
                output_dst_state->pids = NULL;
                SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
                exit(EXIT_FAILURE);
            }
            output_dst_state->pids = ptmp;

            output_dst_state->pids[output_dst_state->no_of_entries - 1] =
                output_src_state->pids[i];
        }
    }

    return;
}

/**
 * \internal
 * \brief Create the failure table.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
static inline void SCPFACCreateFailureTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int ascii_code = 0;
    int32_t state = 0;
    int32_t r_state = 0;

    StateQueue q;
    memset(&q, 0, sizeof(StateQueue));

    /* allot space for the failure table.  A failure entry in the table for
     * every state(SCPFACCtx->state_count) */
    ctx->failure_table = SCMalloc(ctx->state_count * sizeof(int32_t));
    if (ctx->failure_table == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memset(ctx->failure_table, 0, ctx->state_count * sizeof(int32_t));

    /* add the failure transitions for the 0th state, and add every non-fail
     * transition from the 0th state to the queue for further processing
     * of failure states */
    for (ascii_code = 0; ascii_code < 256; ascii_code++) {
        int32_t temp_state = ctx->goto_table[0][ascii_code];
        if (temp_state != 0) {
            SCPFACEnqueue(&q, temp_state);
            ctx->failure_table[temp_state] = 0;
        }
    }

    while (!SCPFACStateQueueIsEmpty(&q)) {
        /* pick up every state from the queue and add failure transitions */
        r_state = SCPFACDequeue(&q);
        for (ascii_code = 0; ascii_code < 256; ascii_code++) {
            int32_t temp_state = ctx->goto_table[r_state][ascii_code];
            if (temp_state == SC_PFAC_FAIL)
                continue;
            SCPFACEnqueue(&q, temp_state);
            state = ctx->failure_table[r_state];

            while(ctx->goto_table[state][ascii_code] == SC_PFAC_FAIL)
                state = ctx->failure_table[state];
            ctx->failure_table[temp_state] = ctx->goto_table[state][ascii_code];
            SCPFACClubOutputStates(temp_state, ctx->failure_table[temp_state],
                                 mpm_ctx);
        }
    }

    return;
}

/**
 * \internal
 * \brief Create the delta table.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
static inline void SCPFACCreateDeltaTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int ascii_code = 0;
    int32_t r_state = 0;

    if ((ctx->state_count < 32767) || construct_both_16_and_32_state_tables) {
        ctx->state_table_u16 = SCMalloc(ctx->state_count *
                                        sizeof(SC_PFAC_STATE_TYPE_U16) * 256);
        if (ctx->state_table_u16 == NULL) {
            SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
            exit(EXIT_FAILURE);
        }
        memset(ctx->state_table_u16, 0,
               ctx->state_count * sizeof(SC_PFAC_STATE_TYPE_U16) * 256);

        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += (ctx->state_count *
                                 sizeof(SC_PFAC_STATE_TYPE_U16) * 256);

        StateQueue q;
        memset(&q, 0, sizeof(StateQueue));

        for (ascii_code = 0; ascii_code < 256; ascii_code++) {
            SC_PFAC_STATE_TYPE_U16 temp_state = ctx->goto_table[0][ascii_code];
            ctx->state_table_u16[0][ascii_code] = temp_state;
            if (temp_state != 0)
                SCPFACEnqueue(&q, temp_state);
        }

        while (!SCPFACStateQueueIsEmpty(&q)) {
            r_state = SCPFACDequeue(&q);

            for (ascii_code = 0; ascii_code < 256; ascii_code++) {
                int32_t temp_state = ctx->goto_table[r_state][ascii_code];
                if (temp_state != SC_PFAC_FAIL) {
                    SCPFACEnqueue(&q, temp_state);
                    ctx->state_table_u16[r_state][ascii_code] = temp_state;
                } else {
                    /* ctx->state_table_u16[r_state][ascii_code] =
                        ctx->state_table_u16[ctx->failure_table[r_state]][ascii_code]; */
                }
            }
        }
    }

    if (!(ctx->state_count < 32767) || construct_both_16_and_32_state_tables) {
        /* create space for the state table.  We could have used the existing goto
         * table, but since we have it set to hold 32 bit state values, we will create
         * a new state table here of type SC_PFAC_STATE_TYPE(current set to uint16_t) */
        ctx->state_table_u32 = SCMalloc(ctx->state_count *
                                        sizeof(SC_PFAC_STATE_TYPE_U32) * 256);
        if (ctx->state_table_u32 == NULL) {
            SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
            exit(EXIT_FAILURE);
        }
        memset(ctx->state_table_u32, 0,
               ctx->state_count * sizeof(SC_PFAC_STATE_TYPE_U32) * 256);

        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += (ctx->state_count *
                                 sizeof(SC_PFAC_STATE_TYPE_U32) * 256);

        StateQueue q;
        memset(&q, 0, sizeof(StateQueue));

        for (ascii_code = 0; ascii_code < 256; ascii_code++) {
            SC_PFAC_STATE_TYPE_U32 temp_state = ctx->goto_table[0][ascii_code];
            ctx->state_table_u32[0][ascii_code] = temp_state;
            if (temp_state != 0)
                SCPFACEnqueue(&q, temp_state);
        }

        while (!SCPFACStateQueueIsEmpty(&q)) {
            r_state = SCPFACDequeue(&q);

            for (ascii_code = 0; ascii_code < 256; ascii_code++) {
                int32_t temp_state = ctx->goto_table[r_state][ascii_code];
                if (temp_state != SC_PFAC_FAIL) {
                    SCPFACEnqueue(&q, temp_state);
                    ctx->state_table_u32[r_state][ascii_code] = temp_state;
                } else {
                   /* ctx->state_table_u32[r_state][ascii_code] =
                        ctx->state_table_u32[ctx->failure_table[r_state]][ascii_code]; */
                }
            }
        }
    }

    return;
}

static inline void SCPFACClubOutputStatePresenceWithDeltaTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int ascii_code = 0;
    uint32_t state = 0;
    uint32_t temp_state = 0;

    if ((ctx->state_count < 32767) || construct_both_16_and_32_state_tables) {
        for (state = 0; state < ctx->state_count; state++) {
            for (ascii_code = 0; ascii_code < 256; ascii_code++) {
                temp_state = ctx->state_table_u16[state & 0x7FFF][ascii_code];
                if (ctx->output_table[temp_state & 0x7FFF].no_of_entries != 0)
                    ctx->state_table_u16[state & 0x7FFF][ascii_code] |= (1 << 15);
            }
        }
    }

    if (!(ctx->state_count < 32767) || construct_both_16_and_32_state_tables) {
        for (state = 0; state < ctx->state_count; state++) {
            for (ascii_code = 0; ascii_code < 256; ascii_code++) {
                temp_state = ctx->state_table_u32[state & 0x00FFFFFF][ascii_code];
                if (ctx->output_table[temp_state & 0x00FFFFFF].no_of_entries != 0)
                    ctx->state_table_u32[state & 0x00FFFFFF][ascii_code] |= (1 << 24);
            }
        }
    }

    return;
}

static inline void SCPFACInsertCaseSensitiveEntriesForPatterns(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    uint32_t state = 0;
    uint32_t k = 0;

    for (state = 0; state < ctx->state_count; state++) {
        if (ctx->output_table[state].no_of_entries == 0)
            continue;

        for (k = 0; k < ctx->output_table[state].no_of_entries; k++) {
            if (ctx->pid_pat_list[ctx->output_table[state].pids[k]].cs != NULL) {
                ctx->output_table[state].pids[k] &= 0x0000FFFF;
                ctx->output_table[state].pids[k] |= 1 << 16;
            }
        }
    }

    return;
}

#if 0
static void SCPFACPrintDeltaTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int i = 0, j = 0;

    printf("##############Delta Table##############\n");
    for (i = 0; i < ctx->state_count; i++) {
        printf("%d: \n", i);
        for (j = 0; j < 256; j++) {
            if (SCPFACGetDelta(i, j, mpm_ctx) != 0) {
                printf("  %c -> %d\n", j, SCPFACGetDelta(i, j, mpm_ctx));
            }
        }
    }

    return;
}
#endif

/**
 * \brief Process the patterns and prepare the state table.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
static inline void SCPFACPrepareStateTable(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;

    /* create the 0th state in the goto table and output_table */
    SCPFACInitNewState(mpm_ctx);

    SCPFACDetermineLevel1Gap(mpm_ctx);

    /* create the goto table */
    SCPFACCreateGotoTable(mpm_ctx);
    /* create the failure table */
 /* SCPFACCreateFailureTable(mpm_ctx); */
    /* create the final state(delta) table */
    SCPFACCreateDeltaTable(mpm_ctx);
    /* club the output state presence with delta transition entries */
    SCPFACClubOutputStatePresenceWithDeltaTable(mpm_ctx);

    /* club nocase entries */
    SCPFACInsertCaseSensitiveEntriesForPatterns(mpm_ctx);

#if 0
    SCPFACPrintDeltaTable(mpm_ctx);
#endif

    /* we don't need these anymore */
    SCFree(ctx->goto_table);
    ctx->goto_table = NULL;
/*    SCFree(ctx->failure_table); 
    ctx->failure_table = NULL; */

    return;
}

/**
 * \brief Process the patterns added to the mpm, and create the internal tables.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
int SCPFACPreparePatterns(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;

    if (mpm_ctx->pattern_cnt == 0 || ctx->init_hash == NULL) {
        SCLogDebug("no patterns supplied to this mpm_ctx");
        return 0;
    }

    /* alloc the pattern array */
    ctx->parray = (SCPFACPattern **)SCMalloc(mpm_ctx->pattern_cnt *
                                           sizeof(SCPFACPattern *));
    if (ctx->parray == NULL)
        goto error;
    memset(ctx->parray, 0, mpm_ctx->pattern_cnt * sizeof(SCPFACPattern *));
    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (mpm_ctx->pattern_cnt * sizeof(SCPFACPattern *));

    /* populate it with the patterns in the hash */
    uint32_t i = 0, p = 0;
    for (i = 0; i < INIT_HASH_SIZE; i++) {
        SCPFACPattern *node = ctx->init_hash[i], *nnode = NULL;
        while(node != NULL) {
            nnode = node->next;
            node->next = NULL;
            ctx->parray[p++] = node;
            node = nnode;
        }
    }

    /* we no longer need the hash, so free it's memory */
    SCFree(ctx->init_hash);
    ctx->init_hash = NULL;

    /* the memory consumed by a single state in our goto table */
    ctx->single_state_size = sizeof(int32_t) * 256;

    /* handle no case patterns */
    ctx->pid_pat_list = SCMalloc((ctx->max_pat_id + 1)* sizeof(SCPFACPatternList));
    if (ctx->pid_pat_list == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memset(ctx->pid_pat_list, 0, (ctx->max_pat_id + 1) * sizeof(SCPFACPatternList));

    for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
        if (!(ctx->parray[i]->flags & MPM_PATTERN_FLAG_NOCASE)) {
            ctx->pid_pat_list[ctx->parray[i]->id].cs = SCMalloc(ctx->parray[i]->len);
            if (ctx->pid_pat_list[ctx->parray[i]->id].cs == NULL) {
                SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
                exit(EXIT_FAILURE);
            }
            memcpy(ctx->pid_pat_list[ctx->parray[i]->id].cs,
                   ctx->parray[i]->original_pat, ctx->parray[i]->len);
            ctx->pid_pat_list[ctx->parray[i]->id].patlen = ctx->parray[i]->len;
        }
    }

    /* prepare the state table required by AC */
    SCPFACPrepareStateTable(mpm_ctx);

#ifdef __SC_CUDA_SUPPORT__
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        int r = SCCudaMemAlloc(&ctx->state_table_u32_cuda,
                               ctx->state_count * sizeof(unsigned int) * 256);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
            exit(EXIT_FAILURE);
        }

        r = SCCudaMemcpyHtoD(ctx->state_table_u32_cuda,
                             ctx->state_table_u32,
                             ctx->state_count * sizeof(unsigned int) * 256);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemcpyHtoD failure.");
            exit(EXIT_FAILURE);
        }
    }
#endif

    /* free all the stored patterns.  Should save us a good 100-200 mbs */
    for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
        if (ctx->parray[i] != NULL) {
            SCPFACFreePattern(mpm_ctx, ctx->parray[i]);
        }
    }
    SCFree(ctx->parray);
    ctx->parray = NULL;
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(SCPFACPattern *));

    return 0;

error:
    return -1;
}

/**
 * \brief Init the mpm thread context.
 *
 * \param mpm_ctx        Pointer to the mpm context.
 * \param mpm_thread_ctx Pointer to the mpm thread context.
 * \param matchsize      We don't need this.
 */
void SCPFACInitThreadCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, uint32_t matchsize)
{
    memset(mpm_thread_ctx, 0, sizeof(MpmThreadCtx));

    mpm_thread_ctx->ctx = SCMalloc(sizeof(SCPFACThreadCtx));
    if (mpm_thread_ctx->ctx == NULL) {
        exit(EXIT_FAILURE);
    }
    memset(mpm_thread_ctx->ctx, 0, sizeof(SCPFACThreadCtx));
    mpm_thread_ctx->memory_cnt++;
    mpm_thread_ctx->memory_size += sizeof(SCPFACThreadCtx);

    return;
}

/**
 * \brief Initialize the AC context.
 *
 * \param mpm_ctx       Mpm context.
 */
void SCPFACInitCtx(MpmCtx *mpm_ctx)
{
    if (mpm_ctx->ctx != NULL)
        return;

    mpm_ctx->ctx = SCMalloc(sizeof(SCPFACCtx));
    if (mpm_ctx->ctx == NULL) {
        exit(EXIT_FAILURE);
    }
    memset(mpm_ctx->ctx, 0, sizeof(SCPFACCtx));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(SCPFACCtx);

    /* initialize the hash we use to speed up pattern insertions */
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    ctx->init_hash = SCMalloc(sizeof(SCPFACPattern *) * INIT_HASH_SIZE);
    if (ctx->init_hash == NULL) {
        exit(EXIT_FAILURE);
    }
    memset(ctx->init_hash, 0, sizeof(SCPFACPattern *) * INIT_HASH_SIZE);

    /* get conf values for AC from our yaml file.  We have no conf values for
     * now.  We will certainly need this, as we develop the algo */
    SCPFACGetConfig();

    SCReturn;
}

/**
 * \brief Destroy the mpm thread context.
 *
 * \param mpm_ctx        Pointer to the mpm context.
 * \param mpm_thread_ctx Pointer to the mpm thread context.
 */
void SCPFACDestroyThreadCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx)
{
    SCPFACPrintSearchStats(mpm_thread_ctx);

    if (mpm_thread_ctx->ctx != NULL) {
        SCFree(mpm_thread_ctx->ctx);
        mpm_thread_ctx->ctx = NULL;
        mpm_thread_ctx->memory_cnt--;
        mpm_thread_ctx->memory_size -= sizeof(SCPFACThreadCtx);
    }

    return;
}

/**
 * \brief Destroy the mpm context.
 *
 * \param mpm_ctx Pointer to the mpm context.
 */
void SCPFACDestroyCtx(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    if (ctx == NULL)
        return;

    if (ctx->init_hash != NULL) {
        SCFree(ctx->init_hash);
        ctx->init_hash = NULL;
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (INIT_HASH_SIZE * sizeof(SCPFACPattern *));
    }

    if (ctx->parray != NULL) {
        uint32_t i;
        for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
            if (ctx->parray[i] != NULL) {
                SCPFACFreePattern(mpm_ctx, ctx->parray[i]);
            }
        }

        SCFree(ctx->parray);
        ctx->parray = NULL;
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(SCPFACPattern *));
    }

    if (ctx->state_table_u16 != NULL) {
        SCFree(ctx->state_table_u16);
        ctx->state_table_u16 = NULL;

        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size -= (ctx->state_count *
                                 sizeof(SC_PFAC_STATE_TYPE_U16) * 256);
    }
    if (ctx->state_table_u32 != NULL) {
        SCFree(ctx->state_table_u32);
        ctx->state_table_u32 = NULL;

        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size -= (ctx->state_count *
                                 sizeof(SC_PFAC_STATE_TYPE_U32) * 256);
    }

    if (ctx->output_table != NULL) {
        uint32_t state_count;
        for (state_count = 0; state_count < ctx->state_count; state_count++) {
            if (ctx->output_table[state_count].pids != NULL) {
                SCFree(ctx->output_table[state_count].pids);
            }
        }
        SCFree(ctx->output_table);
    }

    if (ctx->pid_pat_list != NULL) {
        int i;
        for (i = 0; i < (ctx->max_pat_id + 1); i++) {
            if (ctx->pid_pat_list[i].cs != NULL)
                SCFree(ctx->pid_pat_list[i].cs);
        }
        SCFree(ctx->pid_pat_list);
    }

    SCFree(mpm_ctx->ctx);
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(SCPFACCtx);

    return;
}

/**
 * \brief The aho corasick search function.
 *
 * \param mpm_ctx        Pointer to the mpm context.
 * \param mpm_thread_ctx Pointer to the mpm thread context.
 * \param pmq            Pointer to the Pattern Matcher Queue to hold
 *                       search matches.
 * \param buf            Buffer to be searched.
 * \param buflen         Buffer length.
 *
 * \retval matches Match count.
 */
uint32_t SCPFACSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                    PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
    int i = 0;
    int matches = 0;

    /* \todo tried loop unrolling with register var, with no perf increase.  Need
     * to dig deeper */
    /* \todo Change it for stateful MPM.  Supply the state using mpm_thread_ctx */
    SCPFACPatternList *pid_pat_list = ctx->pid_pat_list;

    if (ctx->state_count < 32767) {
        register SC_PFAC_STATE_TYPE_U16 state = 0;
        SC_PFAC_STATE_TYPE_U16 (*state_table_u16)[256] = ctx->state_table_u16;

        for (i = 0; i < buflen; i++) {
            int j;
            for (j = i; j < buflen; j++) {
                SC_PFAC_STATE_TYPE_U16 prev = state;
                state = state_table_u16[state & 0x7FFF][u8_tolower(buf[j])];
                
                SCLogDebug("inspecting character: %x getting state: %d\n", (int) u8_tolower(buf[j]), (int) state);

                if (state == 0) {
                    break;
                }

                /* if-match */        
                if (state & 0x8000) {
                    SCLogDebug("FOUND MATCH\n");
                    uint32_t no_of_entries = ctx->output_table[state & 0x7FFF].no_of_entries;
                    uint32_t *pids = ctx->output_table[state & 0x7FFF].pids;
                    uint32_t k;
                    matches++;
                    for (k = 0; k < no_of_entries; k++) {
                        if (pids[k] & 0xFFFF0000) {
                            if (SCMemcmp(pid_pat_list[pids[k] & 0x0000FFFF].cs,
                                         buf + i - pid_pat_list[pids[k] & 0x0000FFFF].patlen + 1,
                                         pid_pat_list[pids[k] & 0x0000FFFF].patlen) != 0) {
                                /* inside loop */
                                break;
                            }
                            if (pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] & (1 << ((pids[k] & 0x0000FFFF) % 8))) {
                                ;
                            } else {
                                pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] |= (1 << ((pids[k] & 0x0000FFFF) % 8));
                                pmq->pattern_id_array[pmq->pattern_id_array_cnt++] = pids[k] & 0x0000FFFF;
                            }
                            //matches++;
                        } else {
                            if (pmq->pattern_id_bitarray[pids[k] / 8] & (1 << (pids[k] % 8))) {
                                ;
                            } else {
                                pmq->pattern_id_bitarray[pids[k] / 8] |= (1 << (pids[k] % 8));
                                pmq->pattern_id_array[pmq->pattern_id_array_cnt++] = pids[k];
                            }
                            //matches++;
                        }
                        //loop1:
                        //;
                    }
                } /* end if-match */
            } /* for (j = i; j < buflen; j++) */
        } /* for (i = 0; i < buflen; i++) */

    } else {
        register SC_PFAC_STATE_TYPE_U32 state = 0;
        SC_PFAC_STATE_TYPE_U32 (*state_table_u32)[256] = ctx->state_table_u32;
        for (i = 0; i < buflen; i++) {
            int j = 0;
            for (j = i; j < buflen; j++) {
                state = state_table_u32[state & 0x00FFFFFF][u8_tolower(buf[i])];
                if (state & 0xFF000000) {
                    uint32_t no_of_entries = ctx->output_table[state & 0x00FFFFFF].no_of_entries;
                    uint32_t *pids = ctx->output_table[state & 0x00FFFFFF].pids;
                    uint32_t k;
                    for (k = 0; k < no_of_entries; k++) {
                        if (pids[k] & 0xFFFF0000) {
                            if (SCMemcmp(pid_pat_list[pids[k] & 0x0000FFFF].cs,
                                         buf + i - pid_pat_list[pids[k] & 0x0000FFFF].patlen + 1,
                                         pid_pat_list[pids[k] & 0x0000FFFF].patlen) != 0) {
                                /* inside loop */
                                break;
                            }
                            if (pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] & (1 << ((pids[k] & 0x0000FFFF) % 8))) {
                                ;
                            } else {
                                pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] |= (1 << ((pids[k] & 0x0000FFFF) % 8));
                                pmq->pattern_id_array[pmq->pattern_id_array_cnt++] = pids[k] & 0x0000FFFF;
                            }
                            matches++;
                        } else {
                            if (pmq->pattern_id_bitarray[pids[k] / 8] & (1 << (pids[k] % 8))) {
                                ;
                            } else {
                                pmq->pattern_id_bitarray[pids[k] / 8] |= (1 << (pids[k] % 8));
                                pmq->pattern_id_array[pmq->pattern_id_array_cnt++] = pids[k];
                            }
                            matches++;
                        }
                        //loop1:
                        //;
                    }
                }
            } /* end inner for */
        } /* for (i = 0; i < buflen; i++) */
    }

    return matches;
}

/**
 * \brief Add a case insensitive pattern.  Although we have different calls for
 *        adding case sensitive and insensitive patterns, we make a single call
 *        for either case.  No special treatment for either case.
 *
 * \param mpm_ctx Pointer to the mpm context.
 * \param pat     The pattern to add.
 * \param patnen  The pattern length.
 * \param offset  Ignored.
 * \param depth   Ignored.
 * \param pid     The pattern id.
 * \param sid     Ignored.
 * \param flags   Flags associated with this pattern.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int SCPFACAddPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
                     uint16_t offset, uint16_t depth, uint32_t pid,
                     uint32_t sid, uint8_t flags)
{
    flags |= MPM_PATTERN_FLAG_NOCASE;
    return SCPFACAddPattern(mpm_ctx, pat, patlen, offset, depth, pid, sid, flags);
}

/**
 * \brief Add a case sensitive pattern.  Although we have different calls for
 *        adding case sensitive and insensitive patterns, we make a single call
 *        for either case.  No special treatment for either case.
 *
 * \param mpm_ctx Pointer to the mpm context.
 * \param pat     The pattern to add.
 * \param patnen  The pattern length.
 * \param offset  Ignored.
 * \param depth   Ignored.
 * \param pid     The pattern id.
 * \param sid     Ignored.
 * \param flags   Flags associated with this pattern.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int SCPFACAddPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
                     uint16_t offset, uint16_t depth, uint32_t pid,
                     uint32_t sid, uint8_t flags)
{
    return SCPFACAddPattern(mpm_ctx, pat, patlen, offset, depth, pid, sid, flags);
}

void SCPFACPrintSearchStats(MpmThreadCtx *mpm_thread_ctx)
{

#ifdef SC_PFAC_COUNTERS
    SCPFACThreadCtx *ctx = (SCPFACThreadCtx *)mpm_thread_ctx->ctx;
    printf("AC Thread Search stats (ctx %p)\n", ctx);
    printf("Total calls: %" PRIu32 "\n", ctx->total_calls);
    printf("Total matches: %" PRIu64 "\n", ctx->total_matches);
#endif /* SC_PFAC_COUNTERS */

    return;
}

void SCPFACPrintInfo(MpmCtx *mpm_ctx)
{
    SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;

    printf("MPM AC Information:\n");
    printf("Memory allocs:   %" PRIu32 "\n", mpm_ctx->memory_cnt);
    printf("Memory alloced:  %" PRIu32 "\n", mpm_ctx->memory_size);
    printf(" Sizeof:\n");
    printf("  MpmCtx         %" PRIuMAX "\n", (uintmax_t)sizeof(MpmCtx));
    printf("  SCPFACCtx:         %" PRIuMAX "\n", (uintmax_t)sizeof(SCPFACCtx));
    printf("  SCPFACPattern      %" PRIuMAX "\n", (uintmax_t)sizeof(SCPFACPattern));
    printf("  SCPFACPattern     %" PRIuMAX "\n", (uintmax_t)sizeof(SCPFACPattern));
    printf("Unique Patterns: %" PRIu32 "\n", mpm_ctx->pattern_cnt);
    printf("Smallest:        %" PRIu32 "\n", mpm_ctx->minlen);
    printf("Largest:         %" PRIu32 "\n", mpm_ctx->maxlen);
    printf("Total states in the state table:    %" PRIu32 "\n", ctx->state_count);
    printf("\n");

    return;
}

/****************************Cuda side of things****************************/

#ifdef __SC_CUDA_SUPPORT__

/* \todo Technically it's generic to all mpms, but since we use ac only, the
 *       code internally directly references ac and hence it has found its
 *       home in this file, instead of util-mpm.c
 */
void PFACDetermineCudaStateTableSize(DetectEngineCtx *de_ctx)
{
    MpmCtx *mpm_ctx = NULL;

    int ac_16_tables = 0;
    int ac_32_tables = 0;

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_proto_tcp_packet, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_proto_tcp_packet, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_proto_udp_packet, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_proto_udp_packet, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_proto_other_packet, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_uri, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_uri, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hcbd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hcbd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hhd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hhd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hrhd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hrhd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hmd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hmd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hcd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hcd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hrud, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hrud, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_stream, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_stream, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hsmd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hsmd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hscd, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_hscd, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_huad, 0);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }
    mpm_ctx = MpmFactoryGetMpmCtxForProfile(de_ctx, de_ctx->sgh_mpm_context_huad, 1);
    if (mpm_ctx->mpm_type == MPM_PFAC_CUDA) {
        SCPFACCtx *ctx = (SCPFACCtx *)mpm_ctx->ctx;
        if (ctx->state_count < 32767)
            ac_16_tables++;
        else
            ac_32_tables++;
    }

    if (ac_16_tables > 0 && ac_32_tables > 0)
        SCPFACConstructBoth16and32StateTables();


    SCLogDebug("Total mpm ac 16 bit state tables - %d\n", ac_16_tables);
    SCLogDebug("Total mpm ac 32 bit state tables - %d\n", ac_32_tables);

}

/* \todos
 * - Use texture memory - Can we fit all the arrays into a 3d texture.
 *   Texture memory definitely offers slightly better performance even
 *   on gpus that offer cache for global memory.
 * - Packetpool - modify to support > 65k max pending packets.  We are
 *   hitting packetpool limit currently even with 65k packets.
 * - Use streams.  We have tried overlapping parsing results from the
 *   previous call with invoking the next call.
 * - Offer higher priority to decode threads.
 * - Modify pcap file mode to support reading from multiple pcap files
 *   and hence we will have multiple receive threads.
 * - Split state table into many small pieces and have multiple threads
 *   run each small state table on the same payload.
 * - Used a config peference of l1 over shared memory with no noticeable
 *   perf increase.  Explore it in detail over cards/architectures.
 * - Constant memory performance sucked.  Explore it in detail.
 * - Currently all our state tables are small.  Implement 16 bit state
 *   tables on priority.
 * - Introduce profiling.
 * - Retrieve sgh before buffer packet.
 * - Buffer smsgs too.
 */

void SCPFACConstructBoth16and32StateTables(void)
{
    construct_both_16_and_32_state_tables = 1;

    return;
}

/* \todo Reduce offset buffer size.  Probably a 100,000 entry would be sufficient. */
static void *SCPFACCudaDispatcher(void *arg)
{
#define BLOCK_SIZE 32

    int r = 0;
    ThreadVars *tv = (ThreadVars *)arg;
    MpmCudaConf *conf = CudaHandlerGetCudaProfile("mpm");
    uint32_t sleep_interval_ms = conf->batching_timeout;

    SCLogInfo("AC Cuda Mpm Dispatcher using a timeout of "
              "\"%"PRIu32"\" micro-seconds", sleep_interval_ms);

    CudaBufferData *cb_data =
        CudaHandlerModuleGetData(MPM_PFAC_CUDA_MODULE_NAME,
                                 MPM_PFAC_CUDA_MODULE_CUDA_BUFFER_NAME);

    CUcontext cuda_context =
        CudaHandlerModuleGetContext(MPM_PFAC_CUDA_MODULE_NAME, conf->device_id);
    if (cuda_context == 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "context is NULL.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaCtxPushCurrent(cuda_context);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "context push failed.");
        exit(EXIT_FAILURE);
    }
    CUmodule cuda_module = 0;
    if (CudaHandlerGetCudaModule(&cuda_module, "util-mpm-ac-cuda-kernel") < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error retrieving cuda module.");
        exit(EXIT_FAILURE);
    }
    CUfunction kernel = 0;
#if __WORDSIZE==64
    if (SCCudaModuleGetFunction(&kernel, cuda_module, "SCPFACCudaSearch64") == -1) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error retrieving kernel");
        exit(EXIT_FAILURE);
    }
#else
    if (SCCudaModuleGetFunction(&kernel, cuda_module, "SCPFACCudaSearch32") == -1) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error retrieving kernel");
        exit(EXIT_FAILURE);
    }
#endif

    uint8_t g_u8_lowercasetable[256];
    for (int c = 0; c < 256; c++)
        g_u8_lowercasetable[c] = tolower((uint8_t)c);
    CUdeviceptr cuda_g_u8_lowercasetable_d = 0;
    CUdeviceptr cuda_packets_buffer_d = 0;
    CUdeviceptr cuda_offset_buffer_d = 0;
    CUdeviceptr cuda_results_buffer_d = 0;
    uint32_t *cuda_results_buffer_h = NULL;
    r = SCCudaMemAlloc(&cuda_g_u8_lowercasetable_d, sizeof(g_u8_lowercasetable));
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemcpyHtoD(cuda_g_u8_lowercasetable_d, g_u8_lowercasetable, sizeof(g_u8_lowercasetable));
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemcpyHtoD failure.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemAlloc(&cuda_packets_buffer_d, conf->gpu_transfer_size);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemAlloc(&cuda_offset_buffer_d, conf->gpu_transfer_size * 4);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemAlloc(&cuda_results_buffer_d, conf->gpu_transfer_size * 8);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemAllocHost((void **)&cuda_results_buffer_h, conf->gpu_transfer_size * 8);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemAlloc failure.");
        exit(EXIT_FAILURE);
    }

    CudaBufferCulledInfo cb_culled_info;
    memset(&cb_culled_info, 0, sizeof(cb_culled_info));

    TmThreadsSetFlag(tv, THV_INIT_DONE);
    while (1) {
        if (TmThreadsCheckFlag(tv, THV_KILL))
            break;

        usleep(sleep_interval_ms);

        /**************** 1 SEND ****************/
        CudaBufferCullCompletedSlices(cb_data, &cb_culled_info, conf->gpu_transfer_size);
        if (cb_culled_info.no_of_items == 0)
            continue;
#if 0
        SCLogInfo("1 - cb_culled_info.no_of_items-%"PRIu32" "
                  "cb_culled_info.buffer_len - %"PRIu32" "
                  "cb_culled_info.average size - %f "
                  "cb_culled_info.d_buffer_start_offset - %"PRIu32" "
                  "cb_culled_info.op_buffer_start_offset - %"PRIu32" "
                  "cb_data.no_of_items - %"PRIu32"  "
                  "cb_data.d_buffer_read - %"PRIu32" "
                  "cb_data.d_buffer_write - %"PRIu32" "
                  "cb_data.op_buffer_read - %"PRIu32" "
                  "cb_data.op_buffer_write - %"PRIu32"\n",
                  cb_culled_info.no_of_items,
                  cb_culled_info.d_buffer_len,
                  cb_culled_info.d_buffer_len / (float)cb_culled_info.no_of_items,
                  cb_culled_info.d_buffer_start_offset,
                  cb_culled_info.op_buffer_start_offset,
                  cb_data->no_of_items,
                  cb_data->d_buffer_read,
                  cb_data->d_buffer_write,
                  cb_data->op_buffer_read,
                  cb_data->op_buffer_write);
#endif
        r = SCCudaMemcpyHtoDAsync(cuda_packets_buffer_d, (cb_data->d_buffer + cb_culled_info.d_buffer_start_offset), cb_culled_info.d_buffer_len, 0);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemcpyHtoD failure.");
            exit(EXIT_FAILURE);
        }
        r = SCCudaMemcpyHtoDAsync(cuda_offset_buffer_d, (cb_data->o_buffer + cb_culled_info.op_buffer_start_offset), sizeof(uint32_t) * cb_culled_info.no_of_items, 0);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemcpyHtoD failure.");
            exit(EXIT_FAILURE);
        }
        void *args[] = { &cuda_packets_buffer_d,
                         &cb_culled_info.d_buffer_start_offset,
                         &cuda_offset_buffer_d,
                         &cuda_results_buffer_d,
                         &cb_culled_info.no_of_items,
                         &cuda_g_u8_lowercasetable_d };
        r = SCCudaLaunchKernel(kernel,
                               (cb_culled_info.no_of_items / BLOCK_SIZE) + 1, 1, 1,
                               BLOCK_SIZE, 1, 1,
                               0, 0,
                               args, NULL);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaLaunchKernel failure.");
            exit(EXIT_FAILURE);
        }
        r = SCCudaMemcpyDtoHAsync(cuda_results_buffer_h, cuda_results_buffer_d, sizeof(uint32_t) * (cb_culled_info.d_buffer_len * 2), 0);
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaMemcpyDtoH failure.");
            exit(EXIT_FAILURE);
        }



        /**************** 1 SYNCHRO ****************/
        r = SCCudaCtxSynchronize();
        if (r < 0) {
            SCLogError(SC_ERR_AC_CUDA_ERROR, "SCCudaCtxSynchronize failure.");
            exit(EXIT_FAILURE);
        }

        /************* 1 Parse Results ************/
        uint32_t i_op_start_offset = cb_culled_info.op_buffer_start_offset;
        uint32_t no_of_items = cb_culled_info.no_of_items;
        uint32_t *o_buffer = cb_data->o_buffer;
        uint32_t d_buffer_start_offset = cb_culled_info.d_buffer_start_offset;
        for (uint32_t i = 0; i < no_of_items; i++, i_op_start_offset++) {
            Packet *p = (Packet *)cb_data->p_buffer[i_op_start_offset];

            p->cuda_pkt_vars.cuda_gpu_matches =
                cuda_results_buffer_h[((o_buffer[i_op_start_offset] - d_buffer_start_offset) * 2)];
            if (p->cuda_pkt_vars.cuda_gpu_matches != 0) {
                memcpy(p->cuda_pkt_vars.cuda_results,
                       cuda_results_buffer_h +
                       ((o_buffer[i_op_start_offset] - d_buffer_start_offset) * 2),
                       (cuda_results_buffer_h[((o_buffer[i_op_start_offset] -
                                                d_buffer_start_offset) * 2)] * sizeof(uint32_t)) + 4);
            }

            SCMutexLock(&p->cuda_pkt_vars.cuda_mutex);
            p->cuda_pkt_vars.cuda_done = 1;
            SCMutexUnlock(&p->cuda_pkt_vars.cuda_mutex);
            SCCondSignal(&p->cuda_pkt_vars.cuda_cond);
        }
        if (no_of_items != 0)
            CudaBufferReportCulledConsumption(cb_data, &cb_culled_info);
    } /* while (1) */

    r = SCCudaModuleUnload(cuda_module);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error unloading cuda module.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemFree(cuda_packets_buffer_d);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda device memory.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemFree(cuda_offset_buffer_d);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda device memory.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemFree(cuda_results_buffer_d);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda device memory.");
        exit(EXIT_FAILURE);
    }
    r = SCCudaMemFreeHost(cuda_results_buffer_h);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda host memory.");
        exit(EXIT_FAILURE);
    }

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);
    TmThreadsSetFlag(tv, THV_CLOSED);

    return NULL;

#undef BLOCK_SIZE
}

uint32_t SCPFACCudaPacketResultsProcessing(Packet *p, MpmCtx *mpm_ctx,
                                          PatternMatcherQueue *pmq)
{
    uint32_t u = 0;

    while (!p->cuda_pkt_vars.cuda_done) {
        SCMutexLock(&p->cuda_pkt_vars.cuda_mutex);
        if (p->cuda_pkt_vars.cuda_done) {
            SCMutexUnlock(&p->cuda_pkt_vars.cuda_mutex);
            break;
        } else {
            SCCondWait(&p->cuda_pkt_vars.cuda_cond, &p->cuda_pkt_vars.cuda_mutex);
            SCMutexUnlock(&p->cuda_pkt_vars.cuda_mutex);
        }
    } /* while */
    p->cuda_pkt_vars.cuda_done = 0;
    p->cuda_pkt_vars.cuda_mpm_enabled = 0;

    uint32_t cuda_matches = p->cuda_pkt_vars.cuda_gpu_matches;
    if (cuda_matches == 0)
        return 0;

    uint32_t matches = 0;
    uint32_t *results = p->cuda_pkt_vars.cuda_results + 1;
    uint8_t *buf = p->payload;
    SCPFACCtx *ctx = mpm_ctx->ctx;
    SCPFACOutputTable *output_table = ctx->output_table;
    SCPFACPatternList *pid_pat_list = ctx->pid_pat_list;

    for (u = 0; u < cuda_matches; u += 2) {
        uint32_t offset = results[u];
        uint32_t state = results[u + 1];
        /* we should technically be doing state & 0x00FFFFFF, but we don't
         * since the cuda kernel does that for us */
        uint32_t no_of_entries = output_table[state].no_of_entries;
        /* we should technically be doing state & 0x00FFFFFF, but we don't
         * since the cuda kernel does that for us */
        uint32_t *pids = output_table[state].pids;
        uint32_t k;
        /* note that this is not a verbatim copy from SCPFACSearch().  We
         * don't copy the pattern id into the pattern_id_array.  That's
         * the only change */
        for (k = 0; k < no_of_entries; k++) {
            if (pids[k] & 0xFFFF0000) {
                if (SCMemcmp(pid_pat_list[pids[k] & 0x0000FFFF].cs,
                             buf + offset - pid_pat_list[pids[k] & 0x0000FFFF].patlen + 1,
                             pid_pat_list[pids[k] & 0x0000FFFF].patlen) != 0) {
                    /* inside loop */
                    continue;
                }
                if (pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] & (1 << ((pids[k] & 0x0000FFFF) % 8))) {
                    ;
                } else {
                    pmq->pattern_id_bitarray[(pids[k] & 0x0000FFFF) / 8] |= (1 << ((pids[k] & 0x0000FFFF) % 8));
                }
                matches++;
            } else {
                if (pmq->pattern_id_bitarray[pids[k] / 8] & (1 << (pids[k] % 8))) {
                    ;
                } else {
                    pmq->pattern_id_bitarray[pids[k] / 8] |= (1 << (pids[k] % 8));
                }
                matches++;
            }
        }
    }

    return matches;
}

void SCPFACCudaStartDispatcher(void)
{
    /* create the threads */
    ThreadVars *tv = TmThreadCreate("Cuda_Mpm_PFAC_Dispatcher",
                                    NULL, NULL,
                                    NULL, NULL,
                                    "custom", SCPFACCudaDispatcher, 0);
    if (tv == NULL) {
        SCLogError(SC_ERR_THREAD_CREATE, "Error creating a thread for "
                   "ac cuda dispatcher.  Killing engine.");
        exit(EXIT_FAILURE);
    }
    if (TmThreadSpawn(tv) != 0) {
        SCLogError(SC_ERR_THREAD_SPAWN, "Failed to spawn thread for "
                   "ac cuda dispatcher.  Killing engine.");
        exit(EXIT_FAILURE);
    }

    return;
}

int PFACMpmCudaBufferSetup(void)
{
    int r = 0;
    MpmCudaConf *conf = CudaHandlerGetCudaProfile("mpm");
    if (conf == NULL) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error obtaining cuda mpm profile.");
        return -1;
    }

    CUcontext cuda_context = CudaHandlerModuleGetContext(MPM_PFAC_CUDA_MODULE_NAME, conf->device_id);
    if (cuda_context == 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error retrieving cuda context.");
        return -1;
    }
    r = SCCudaCtxPushCurrent(cuda_context);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error pushing cuda context.");
        return -1;
    }

    uint8_t *d_buffer = NULL;
    uint32_t *o_buffer = NULL;
    void **p_buffer = NULL;

    r = SCCudaMemAllocHost((void *)&d_buffer, conf->cb_buffer_size);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Cuda alloc host failure.");
        return -1;
    }
    SCLogInfo("Allocated a cuda d_buffer - %"PRIu32" bytes", conf->cb_buffer_size);
    r = SCCudaMemAllocHost((void *)&o_buffer, sizeof(uint32_t) * UTIL_MPM_CUDA_CUDA_BUFFER_OPBUFFER_ITEMS_DEFAULT);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Cuda alloc host failue.");
        return -1;
    }
    r = SCCudaMemAllocHost((void *)&p_buffer, sizeof(void *) * UTIL_MPM_CUDA_CUDA_BUFFER_OPBUFFER_ITEMS_DEFAULT);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Cuda alloc host failure.");
        return -1;
    }

    r = SCCudaCtxPopCurrent(NULL);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "cuda context pop failure.");
        return -1;
    }

    CudaBufferData *cb = CudaBufferRegisterNew(d_buffer, conf->cb_buffer_size, o_buffer, p_buffer, UTIL_MPM_CUDA_CUDA_BUFFER_OPBUFFER_ITEMS_DEFAULT);
    if (cb == NULL) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error registering new cb instance.");
        return -1;
    }
    CudaHandlerModuleStoreData(MPM_PFAC_CUDA_MODULE_NAME, MPM_PFAC_CUDA_MODULE_CUDA_BUFFER_NAME, cb);

    return 0;
}

int PFACMpmCudaBufferDeSetup(void)
{
    int r = 0;
    MpmCudaConf *conf = CudaHandlerGetCudaProfile("mpm");
    if (conf == NULL) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error obtaining cuda mpm profile.");
        return -1;
    }

    CudaBufferData *cb_data = CudaHandlerModuleGetData(MPM_PFAC_CUDA_MODULE_NAME, MPM_PFAC_CUDA_MODULE_CUDA_BUFFER_NAME);
    BUG_ON(cb_data == NULL);

    CUcontext cuda_context = CudaHandlerModuleGetContext(MPM_PFAC_CUDA_MODULE_NAME, conf->device_id);
    if (cuda_context == 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error retrieving cuda context.");
        return -1;
    }
    r = SCCudaCtxPushCurrent(cuda_context);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error pushing cuda context.");
        return -1;
    }

    r = SCCudaMemFreeHost(cb_data->d_buffer);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda host memory.");
        return -1;
    }
    r = SCCudaMemFreeHost(cb_data->o_buffer);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda host memory.");
        return -1;
    }
    r = SCCudaMemFreeHost(cb_data->p_buffer);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "Error freeing cuda host memory.");
        return -1;
    }

    r = SCCudaCtxPopCurrent(NULL);
    if (r < 0) {
        SCLogError(SC_ERR_AC_CUDA_ERROR, "cuda context pop failure.");
        return -1;
    }

    CudaBufferDeRegister(cb_data);

    return 0;
}

#endif /* __SC_CUDA_SUPPORT */

/************************** Mpm Registration ***************************/

/**
 * \brief Register the aho-corasick mpm.
 */
void MpmPFACRegister(void)
{
    mpm_table[MPM_PFAC].name = "pfac";
    /* don't need this.  isn't that awesome?  no more chopping and blah blah */
    mpm_table[MPM_PFAC].max_pattern_length = 0;

    mpm_table[MPM_PFAC].InitCtx = SCPFACInitCtx;
    mpm_table[MPM_PFAC].InitThreadCtx = SCPFACInitThreadCtx;
    mpm_table[MPM_PFAC].DestroyCtx = SCPFACDestroyCtx;
    mpm_table[MPM_PFAC].DestroyThreadCtx = SCPFACDestroyThreadCtx;
    mpm_table[MPM_PFAC].AddPattern = SCPFACAddPatternCS;
    mpm_table[MPM_PFAC].AddPatternNocase = SCPFACAddPatternCI;
    mpm_table[MPM_PFAC].Prepare = SCPFACPreparePatterns;
    mpm_table[MPM_PFAC].Search = SCPFACSearch;
    mpm_table[MPM_PFAC].Cleanup = NULL;
    mpm_table[MPM_PFAC].PrintCtx = SCPFACPrintInfo;
    mpm_table[MPM_PFAC].PrintThreadCtx = SCPFACPrintSearchStats;
    mpm_table[MPM_PFAC].RegisterUnittests = SCPFACRegisterTests;

    return;
}

#ifdef __SC_CUDA_SUPPORT__

/**
 * \brief Register the aho-corasick cuda mpm.
 */
void MpmPFACCudaRegister(void)
{
    mpm_table[MPM_PFAC_CUDA].name = "pfac-cuda";
    /* don't need this.  isn't that awesome?  no more chopping and blah blah */
    mpm_table[MPM_PFAC_CUDA].max_pattern_length = 0;

    mpm_table[MPM_PFAC_CUDA].InitCtx = SCPFACInitCtx;
    mpm_table[MPM_PFAC_CUDA].InitThreadCtx = SCPFACInitThreadCtx;
    mpm_table[MPM_PFAC_CUDA].DestroyCtx = SCPFACDestroyCtx;
    mpm_table[MPM_PFAC_CUDA].DestroyThreadCtx = SCPFACDestroyThreadCtx;
    mpm_table[MPM_PFAC_CUDA].AddPattern = SCPFACAddPatternCS;
    mpm_table[MPM_PFAC_CUDA].AddPatternNocase = SCPFACAddPatternCI;
    mpm_table[MPM_PFAC_CUDA].Prepare = SCPFACPreparePatterns;
    mpm_table[MPM_PFAC_CUDA].Search = SCPFACSearch;
    mpm_table[MPM_PFAC_CUDA].Cleanup = NULL;
    mpm_table[MPM_PFAC_CUDA].PrintCtx = SCPFACPrintInfo;
    mpm_table[MPM_PFAC_CUDA].PrintThreadCtx = SCPFACPrintSearchStats;
    mpm_table[MPM_PFAC_CUDA].RegisterUnittests = SCPFACRegisterTests;

    return;
}

#endif /* __SC_CUDA_SUPPORT__ */

/*************************************Unittests********************************/

#ifdef UNITTESTS

static int SCPFACTest01(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghjiklmnopqrstuvwxyz";

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest02(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abce", 4, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghjiklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest03(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"bcde", 4, 0, 0, 1, 0, 0);
    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"fghj", 4, 0, 0, 2, 0, 0);
    PmqSetup(&pmq, 3);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghjiklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest04(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"bcdegh", 6, 0, 0, 1, 0, 0);
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"fghjxyz", 7, 0, 0, 2, 0, 0);
    PmqSetup(&pmq, 3);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghjiklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest05(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"ABCD", 4, 0, 0, 0, 0, 0);
    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"bCdEfG", 6, 0, 0, 1, 0, 0);
    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"fghJikl", 7, 0, 0, 2, 0, 0);
    PmqSetup(&pmq, 3);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghjiklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest06(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcd";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest07(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* should match 30 times */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0);
    /* should match 29 times */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0);
    /* should match 28 times */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0);
    /* 26 */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0);
    /* 21 */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0);
    /* 1 */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                     30, 0, 0, 5, 0, 0);
    PmqSetup(&pmq, 6);
    /* total matches: 135 */

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest08(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)"a", 1);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest09(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"ab", 2, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)"ab", 2);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest10(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcdefgh", 8, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "01234567890123456789012345678901234567890123456789"
                "01234567890123456789012345678901234567890123456789"
                "abcdefgh"
                "01234567890123456789012345678901234567890123456789"
                "01234567890123456789012345678901234567890123456789";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest11(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    if (MpmAddPatternCS(&mpm_ctx, (uint8_t *)"he", 2, 0, 0, 1, 0, 0) == -1)
        goto end;
    if (MpmAddPatternCS(&mpm_ctx, (uint8_t *)"she", 3, 0, 0, 2, 0, 0) == -1)
        goto end;
    if (MpmAddPatternCS(&mpm_ctx, (uint8_t *)"his", 3, 0, 0, 3, 0, 0) == -1)
        goto end;
    if (MpmAddPatternCS(&mpm_ctx, (uint8_t *)"hers", 4, 0, 0, 4, 0, 0) == -1)
        goto end;
    PmqSetup(&pmq, 5);

    if (SCPFACPreparePatterns(&mpm_ctx) == -1)
        goto end;

    result = 1;

    char *buf = "he";
    result &= (SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq, (uint8_t *)buf,
                          strlen(buf)) == 1);
    buf = "she";
    result &= (SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq, (uint8_t *)buf,
                          strlen(buf)) == 2);
    buf = "his";
    result &= (SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq, (uint8_t *)buf,
                          strlen(buf)) == 1);
    buf = "hers";
    result &= (SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq, (uint8_t *)buf,
                          strlen(buf)) == 2);

 end:
    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest12(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"wxyz", 4, 0, 0, 0, 0, 0);
    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"vwxyz", 5, 0, 0, 1, 0, 0);
    PmqSetup(&pmq, 2);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest13(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcdefghijklmnopqrstuvwxyzABCD";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyzABCD";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest14(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcdefghijklmnopqrstuvwxyzABCDE";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyzABCDE";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest15(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcdefghijklmnopqrstuvwxyzABCDEF";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyzABCDEF";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest16(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcdefghijklmnopqrstuvwxyzABC";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyzABC";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest17(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcdefghijklmnopqrstuvwxyzAB";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyzAB";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest18(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    char *pat = "abcde""fghij""klmno""pqrst""uvwxy""z";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcde""fghij""klmno""pqrst""uvwxy""z";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest19(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 */
    char *pat = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest20(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 */
    char *pat = "AAAAA""AAAAA""AAAAA""AAAAA""AAAAA""AAAAA""AA";
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)pat, strlen(pat), 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "AAAAA""AAAAA""AAAAA""AAAAA""AAAAA""AAAAA""AA";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest21(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                              (uint8_t *)"AA", 2);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest22(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0);
    /* 1 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"abcde", 5, 0, 0, 1, 0, 0);
    PmqSetup(&pmq, 2);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "abcdefghijklmnopqrstuvwxyz";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                              (uint8_t *)buf, strlen(buf));

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest23(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                              (uint8_t *)"aa", 2);

    if (cnt == 0)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest24(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 1 */
    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                              (uint8_t *)"aa", 2);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest25(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"ABCD", 4, 0, 0, 0, 0, 0);
    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"bCdEfG", 6, 0, 0, 1, 0, 0);
    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"fghiJkl", 7, 0, 0, 2, 0, 0);
    PmqSetup(&pmq, 3);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest26(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    MpmAddPatternCI(&mpm_ctx, (uint8_t *)"Works", 5, 0, 0, 0, 0, 0);
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"Works", 5, 0, 0, 1, 0, 0);
    PmqSetup(&pmq, 2);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "works";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 1)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest27(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 0 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"ONE", 3, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "tone";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest28(void)
{
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    PatternMatcherQueue pmq;

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    memset(&mpm_thread_ctx, 0, sizeof(MpmThreadCtx));
    MpmInitCtx(&mpm_ctx, MPM_PFAC);
    SCPFACInitThreadCtx(&mpm_ctx, &mpm_thread_ctx, 0);

    /* 0 match */
    MpmAddPatternCS(&mpm_ctx, (uint8_t *)"one", 3, 0, 0, 0, 0, 0);
    PmqSetup(&pmq, 1);

    SCPFACPreparePatterns(&mpm_ctx);

    char *buf = "tONE";
    uint32_t cnt = SCPFACSearch(&mpm_ctx, &mpm_thread_ctx, &pmq,
                               (uint8_t *)buf, strlen(buf));

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    SCPFACDestroyCtx(&mpm_ctx);
    SCPFACDestroyThreadCtx(&mpm_ctx, &mpm_thread_ctx);
    PmqFree(&pmq);
    return result;
}

static int SCPFACTest29(void)
{
    uint8_t *buf = (uint8_t *)"onetwothreefourfivesixseveneightnine";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf, buflen, IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(content:\"onetwothreefourfivesixseveneightnine\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    de_ctx->sig_list->next = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(content:\"onetwothreefourfivesixseveneightnine\"; fast_pattern:3,3; sid:2;)");
    if (de_ctx->sig_list->next == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1) != 1) {
        printf("if (PacketAlertCheck(p, 1) != 1) failure\n");
        goto end;
    }
    if (PacketAlertCheck(p, 2) != 1) {
        printf("if (PacketAlertCheck(p, 1) != 2) failure\n");
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);

        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    UTHFreePackets(&p, 1);
    return result;
}

#endif /* UNITTESTS */

void SCPFACRegisterTests(void)
{

#ifdef UNITTESTS
    UtRegisterTest("SCPFACTest01", SCPFACTest01, 1);
    UtRegisterTest("SCPFACTest02", SCPFACTest02, 1);
    UtRegisterTest("SCPFACTest03", SCPFACTest03, 1);
    UtRegisterTest("SCPFACTest04", SCPFACTest04, 1);
    UtRegisterTest("SCPFACTest05", SCPFACTest05, 1);
    UtRegisterTest("SCPFACTest06", SCPFACTest06, 1);
    UtRegisterTest("SCPFACTest07", SCPFACTest07, 1);
    UtRegisterTest("SCPFACTest08", SCPFACTest08, 1);
    UtRegisterTest("SCPFACTest09", SCPFACTest09, 1);
    UtRegisterTest("SCPFACTest10", SCPFACTest10, 1);
    UtRegisterTest("SCPFACTest11", SCPFACTest11, 1);
    UtRegisterTest("SCPFACTest12", SCPFACTest12, 1);
    UtRegisterTest("SCPFACTest13", SCPFACTest13, 1);
    UtRegisterTest("SCPFACTest14", SCPFACTest14, 1);
    UtRegisterTest("SCPFACTest15", SCPFACTest15, 1);
    UtRegisterTest("SCPFACTest16", SCPFACTest16, 1);
    UtRegisterTest("SCPFACTest17", SCPFACTest17, 1);
    UtRegisterTest("SCPFACTest18", SCPFACTest18, 1);
    UtRegisterTest("SCPFACTest19", SCPFACTest19, 1);
    UtRegisterTest("SCPFACTest20", SCPFACTest20, 1);
    UtRegisterTest("SCPFACTest21", SCPFACTest21, 1);
    UtRegisterTest("SCPFACTest22", SCPFACTest22, 1);
    UtRegisterTest("SCPFACTest23", SCPFACTest23, 1);
    UtRegisterTest("SCPFACTest24", SCPFACTest24, 1);
    UtRegisterTest("SCPFACTest25", SCPFACTest25, 1);
    UtRegisterTest("SCPFACTest26", SCPFACTest26, 1);
    UtRegisterTest("SCPFACTest27", SCPFACTest27, 1);
    UtRegisterTest("SCPFACTest28", SCPFACTest28, 1);
    UtRegisterTest("SCPFACTest29", SCPFACTest29, 1);
#endif

    return;
}
