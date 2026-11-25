#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <aarch64/intrinsic.h>

static PTEntriesPtr new_table(void)
{
    PTEntriesPtr table = kalloc_page();
    if (table)
        memset(table, 0, PAGE_SIZE);
    return table;
}

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc)
{
    if (va >> 48)
        return NULL;
    if (!pgdir->pt && alloc)
        pgdir->pt = new_table();
    PTEntriesPtr table = pgdir->pt;
    if (!table)
        return NULL;
    u64 index[] = { VA_PART0(va), VA_PART1(va), VA_PART2(va) };
    for (unsigned level = 0; level < 3; level++) {
        PTEntry *entry = &table[index[level]];
        if (!(*entry & PTE_VALID)) {
            if (!alloc)
                return NULL;
            PTEntriesPtr child = new_table();
            if (!child)
                return NULL;
            *entry = K2P(child) | PTE_TABLE;
        }
        if ((*entry & 3) != PTE_TABLE)
            return NULL;
        table = (PTEntriesPtr)P2K(PTE_ADDRESS(*entry));
    }
    return &table[VA_PART3(va)];
}

void init_pgdir(struct pgdir *pgdir)
{
    pgdir->pt = NULL;
}

static void free_table(PTEntriesPtr table, unsigned level)
{
    if (level < 3) {
        for (unsigned i = 0; i < N_PTE_PER_TABLE; i++)
            if ((table[i] & 3) == PTE_TABLE)
                free_table((PTEntriesPtr)P2K(PTE_ADDRESS(table[i])), level + 1);
    }
    kfree_page(table);
}

void free_pgdir(struct pgdir *pgdir)
{
    PTEntriesPtr root = pgdir->pt;
    if (!root)
        return;
    pgdir->pt = NULL;
    if (arch_get_ttbr0() == K2P(root))
        attach_pgdir(pgdir);
    free_table(root, 0);
}

void attach_pgdir(struct pgdir *pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}
