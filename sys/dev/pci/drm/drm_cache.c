#include <dev/pci/drm/drmP.h>

static void
drm_clflush_page(struct vm_page *page)
{
	void *addr;

	if (page == NULL)
		return;

	addr = kmap_atomic(page);
	pmap_flush_cache((vaddr_t)addr, PAGE_SIZE);
	kunmap_atomic(addr);
}

void
drm_clflush_pages(struct vm_page *pages[], unsigned long num_pages)
{
	unsigned long i;

	for (i = 0; i < num_pages; i++)
		drm_clflush_page(*pages++);
}

void
drm_clflush_sg(struct sg_table *st)
{
	struct sg_page_iter sg_iter;

	for_each_sg_page(st->sgl, &sg_iter, st->nents, 0)
		drm_clflush_page(sg_page_iter_page(&sg_iter));
}

void
drm_clflush_virt_range(void *addr, unsigned long length)
{
	pmap_flush_cache((vaddr_t)addr, length);
}
