#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#define BLOCK_ALIGN 8
#define MAX_BLOCK_SIZE (PAGE_SIZE / 2)
#define SIZE_CLASS_COUNT (MAX_BLOCK_SIZE / BLOCK_ALIGN)

typedef struct FreePage {
    struct FreePage* next;
} FreePage;

typedef struct FreeBlock {
    struct FreeBlock* next;
} FreeBlock;

/* A page holds blocks of one size. Only non-full pages are on a class list. */
typedef struct SlabPage {
    struct SlabPage *prev, *next;
    FreeBlock* free_blocks;
    u16 size_class;
    u16 in_use;
    u16 next_offset;
} SlabPage;

typedef struct SizeClass {
    SpinLock lock;
    SlabPage* available;
} SizeClass;

_Static_assert(sizeof(SlabPage) % BLOCK_ALIGN == 0, "unaligned slab header");
_Static_assert(sizeof(FreeBlock) <= BLOCK_ALIGN, "free block is too large");
_Static_assert(sizeof(SlabPage) + MAX_BLOCK_SIZE <= PAGE_SIZE,
               "largest block does not fit in a slab");

static SpinLock page_lock;
static FreePage* free_pages;
static SizeClass size_classes[SIZE_CLASS_COUNT];

RefCount kalloc_page_cnt;

/* Both helpers require the corresponding size-class lock. */
static void add_available(SizeClass* cls, SlabPage* page) {
    page->prev = NULL;
    page->next = cls->available;
    if (page->next)
        page->next->prev = page;
    cls->available = page;
}

static void remove_available(SizeClass* cls, SlabPage* page) {
    if (page->prev)
        page->prev->next = page->next;
    else
        cls->available = page->next;
    if (page->next)
        page->next->prev = page->prev;
    page->prev = page->next = NULL;
}

void kinit() {
    init_rc(&kalloc_page_cnt);

    init_spinlock(&page_lock);
    free_pages = NULL;
    for (unsigned i = 0; i < SIZE_CLASS_COUNT; i++) {
        init_spinlock(&size_classes[i].lock);
        size_classes[i].available = NULL;
    }

    extern char end[];
    /* Compare physical addresses; access the pages through their kernel VAs. */
    for (u64 pa = round_up(K2P(end), PAGE_SIZE);
         pa + PAGE_SIZE <= PHYSTOP; pa += PAGE_SIZE) {
        FreePage* page = (FreePage*)P2K(pa);
        page->next = free_pages;
        free_pages = page;
    }
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&page_lock);
    FreePage* page = free_pages;
    if (page)
        free_pages = page->next;
    release_spinlock(&page_lock);

    /* An exhausted pool must not count a failed allocation as a live page. */
    if (!page)
        decrement_rc(&kalloc_page_cnt);
    return page;
}

void kfree_page(void* p) {
    decrement_rc(&kalloc_page_cnt);

    FreePage* page = p;
    acquire_spinlock(&page_lock);
    page->next = free_pages;
    free_pages = page;
    release_spinlock(&page_lock);
}

void* kalloc(unsigned long long size) {
    if (size == 0 || size > MAX_BLOCK_SIZE)
        return NULL;

    unsigned index = (size - 1) / BLOCK_ALIGN;
    unsigned block_size = (index + 1) * BLOCK_ALIGN;
    unsigned capacity = (PAGE_SIZE - sizeof(SlabPage)) / block_size;
    SizeClass* cls = &size_classes[index];

    /* Lock order is always size class -> page pool, never the reverse. */
    acquire_spinlock(&cls->lock);
    SlabPage* page = cls->available;
    if (!page) {
        page = kalloc_page();
        if (!page) {
            release_spinlock(&cls->lock);
            return NULL;
        }
        page->free_blocks = NULL;
        page->size_class = index;
        page->in_use = 0;
        page->next_offset = sizeof(SlabPage);
        add_available(cls, page);
    }

    void* block;
    if (page->free_blocks) {
        block = page->free_blocks;
        page->free_blocks = page->free_blocks->next;
    } else {
        /* Carve out one block at a time instead of initializing a whole page. */
        block = (u8*)page + page->next_offset;
        page->next_offset += block_size;
    }
    page->in_use++;
    if (page->in_use == capacity)
        remove_available(cls, page);
    release_spinlock(&cls->lock);
    return block;
}

void kfree(void* ptr) {
    if (!ptr)
        return;

    /* This live block keeps its page and immutable size_class valid. */
    SlabPage* page = (SlabPage*)PAGE_BASE(ptr);
    unsigned index = page->size_class;
    unsigned block_size = (index + 1) * BLOCK_ALIGN;
    unsigned capacity = (PAGE_SIZE - sizeof(SlabPage)) / block_size;
    SizeClass* cls = &size_classes[index];

    acquire_spinlock(&cls->lock);
    if (page->in_use == capacity)
        add_available(cls, page);
    FreeBlock* block = ptr;
    block->next = page->free_blocks;
    page->free_blocks = block;
    page->in_use--;

    if (page->in_use == 0) {
        remove_available(cls, page);
        kfree_page(page);
    }
    release_spinlock(&cls->lock);
}
