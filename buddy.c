#include "buddy.h"

#include <stdint.h>
#include <stdlib.h>

#define PAGE_SIZE 4096u
#define MIN_RANK 1
#define MAX_RANK 16
#define NO_PAGE (-1)

/*
 * The supplied pages are entirely available to callers, so allocator
 * bookkeeping is kept separately instead of placing headers in the pool.
 */
static unsigned char *pool_base;
static size_t pool_pages;
static int *page_rank;
static unsigned char *allocated_start;
static int *next_free;
static int *prev_free;
static int free_head[MAX_RANK + 1];
static int free_count[MAX_RANK + 1];

static size_t pages_for_rank(int rank)
{
    return (size_t)1 << (rank - 1);
}

static void clear_state(void)
{
    free(page_rank);
    free(allocated_start);
    free(next_free);
    free(prev_free);
    pool_base = NULL;
    pool_pages = 0;
    page_rank = NULL;
    allocated_start = NULL;
    next_free = NULL;
    prev_free = NULL;
}

/* Lists are kept in address order, making allocation deterministic. */
static void insert_free(int rank, int index)
{
    int current = free_head[rank];
    int previous = NO_PAGE;

    while (current != NO_PAGE && current < index) {
        previous = current;
        current = next_free[current];
    }

    prev_free[index] = previous;
    next_free[index] = current;
    if (previous == NO_PAGE)
        free_head[rank] = index;
    else
        next_free[previous] = index;
    if (current != NO_PAGE)
        prev_free[current] = index;
    free_count[rank]++;
}

static void remove_free(int rank, int index)
{
    int previous = prev_free[index];
    int next = next_free[index];

    if (previous == NO_PAGE)
        free_head[rank] = next;
    else
        next_free[previous] = next;
    if (next != NO_PAGE)
        prev_free[next] = previous;
    next_free[index] = NO_PAGE;
    prev_free[index] = NO_PAGE;
    free_count[rank]--;
}

static void set_block_rank(int index, int rank)
{
    size_t count = pages_for_rank(rank);
    size_t page;

    for (page = 0; page < count; ++page)
        page_rank[index + page] = rank;
}

static void add_free_block(int index, int rank)
{
    set_block_rank(index, rank);
    allocated_start[index] = 0;
    insert_free(rank, index);
}

int init_page(void *p, int pgcount)
{
    size_t remaining;
    size_t index;
    int rank;

    if (p == NULL || pgcount <= 0)
        return -EINVAL;

    clear_state();
    pool_base = p;
    pool_pages = (size_t)pgcount;
    page_rank = calloc(pool_pages, sizeof(*page_rank));
    allocated_start = calloc(pool_pages, sizeof(*allocated_start));
    next_free = malloc(pool_pages * sizeof(*next_free));
    prev_free = malloc(pool_pages * sizeof(*prev_free));
    if (page_rank == NULL || allocated_start == NULL || next_free == NULL ||
        prev_free == NULL) {
        clear_state();
        return -ENOSPC;
    }

    for (index = 0; index < pool_pages; ++index) {
        next_free[index] = NO_PAGE;
        prev_free[index] = NO_PAGE;
    }
    for (rank = MIN_RANK; rank <= MAX_RANK; ++rank) {
        free_head[rank] = NO_PAGE;
        free_count[rank] = 0;
    }

    /* Decompose a non-power-of-two pool into aligned buddy blocks. */
    index = 0;
    remaining = pool_pages;
    while (remaining != 0) {
        rank = MAX_RANK;
        while (rank > MIN_RANK && pages_for_rank(rank) > remaining)
            rank--;
        add_free_block((int)index, rank);
        index += pages_for_rank(rank);
        remaining -= pages_for_rank(rank);
    }
    return OK;
}

void *alloc_pages(int rank)
{
    int found_rank;
    int index;

    if (rank < MIN_RANK || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);
    if (pool_base == NULL)
        return ERR_PTR(-ENOSPC);

    for (found_rank = rank; found_rank <= MAX_RANK; ++found_rank) {
        if (free_head[found_rank] != NO_PAGE)
            break;
    }
    if (found_rank > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    index = free_head[found_rank];
    remove_free(found_rank, index);
    while (found_rank > rank) {
        size_t half;

        found_rank--;
        half = pages_for_rank(found_rank);
        add_free_block(index + (int)half, found_rank);
    }

    set_block_rank(index, rank);
    allocated_start[index] = (unsigned char)rank;
    return pool_base + (size_t)index * PAGE_SIZE;
}

static int pointer_to_page(void *p, int *index)
{
    uintptr_t base;
    uintptr_t address;
    uintptr_t bytes;

    if (pool_base == NULL || p == NULL)
        return 0;
    base = (uintptr_t)pool_base;
    address = (uintptr_t)p;
    if (address < base)
        return 0;
    bytes = address - base;
    if (bytes % PAGE_SIZE != 0 || bytes / PAGE_SIZE >= pool_pages)
        return 0;
    *index = (int)(bytes / PAGE_SIZE);
    return 1;
}

int return_pages(void *p)
{
    int index;
    int rank;

    if (!pointer_to_page(p, &index) || allocated_start[index] == 0)
        return -EINVAL;

    rank = allocated_start[index];
    allocated_start[index] = 0;
    while (rank < MAX_RANK) {
        int buddy = index ^ (int)pages_for_rank(rank);
        int parent;

        if ((size_t)buddy >= pool_pages || page_rank[buddy] != rank ||
            allocated_start[buddy] != 0)
            break;
        /* A free block only has a list link at its starting page. */
        if (buddy != 0 && prev_free[buddy] == NO_PAGE &&
            free_head[rank] != buddy)
            break;
        if (buddy == 0 && free_head[rank] != buddy)
            break;

        remove_free(rank, buddy);
        parent = index < buddy ? index : buddy;
        index = parent;
        rank++;
    }
    add_free_block(index, rank);
    return OK;
}

int query_ranks(void *p)
{
    int index;

    if (!pointer_to_page(p, &index))
        return -EINVAL;
    return page_rank[index];
}

int query_page_counts(int rank)
{
    if (rank < MIN_RANK || rank > MAX_RANK)
        return -EINVAL;
    return free_count[rank];
}
