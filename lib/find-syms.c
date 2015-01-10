#ifdef __APPLE__

#include <stdbool.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <dlfcn.h>
#include <pthread.h>

#include "substitute.h"
#include "substitute-internal.h"

extern const struct dyld_all_image_infos *_dyld_get_all_image_infos();

static pthread_once_t dyld_inspect_once = PTHREAD_ONCE_INIT;
/* and its fruits: */
static uintptr_t (*ImageLoaderMachO_getSlide)(void *);
static const struct mach_header *(*ImageLoaderMachO_machHeader)(void *);
#ifdef __LP64__
typedef struct mach_header_64 mach_header_x;
typedef struct segment_command_64 segment_command_x;
typedef struct section_64 section_x;
#define LC_SEGMENT_X LC_SEGMENT_64
#else
typedef struct mach_header mach_header_x;
typedef struct segment_command segment_command_x;
typedef struct section section_x;
#define LC_SEGMENT_X LC_SEGMENT
#endif

static void *sym_to_ptr(substitute_sym *sym, intptr_t slide) {
	uintptr_t addr = sym->n_value;
	addr += slide;
	if (sym->n_desc & N_ARM_THUMB_DEF)
		addr |= 1;
	return (void *) addr;
}

static void find_syms_raw(const void *hdr, intptr_t *slide, const char **names, substitute_sym **syms, size_t count) {
	memset(syms, 0, sizeof(*syms) * count);

	/* note: no verification at all */
	const mach_header_x *mh = hdr;
	uint32_t ncmds = mh->ncmds;
	struct load_command *lc = (void *) (mh + 1);
	struct symtab_command syc;
	for (uint32_t i = 0; i < ncmds; i++) {
		if (lc->cmd == LC_SYMTAB) {
			syc = *(struct symtab_command *) lc;
			goto ok;
		}
		lc = (void *) lc + lc->cmdsize;
	}
	return; /* no symtab, no symbols */
ok: ;
	substitute_sym *symtab = NULL;
	const char *strtab = NULL;
	lc = (void *) (mh + 1);
	for (uint32_t i = 0; i < ncmds; i++) {
		if (lc->cmd == LC_SEGMENT_X) {
			segment_command_x *sc = (void *) lc;
			if (syc.symoff - sc->fileoff < sc->filesize)
				symtab = (void *) sc->vmaddr + syc.symoff - sc->fileoff;
			if (syc.stroff - sc->fileoff < sc->filesize)
				strtab = (void *) sc->vmaddr + syc.stroff - sc->fileoff;
			if (*slide == -1 && sc->fileoff == 0) {
				// used only for dyld
				*slide = (uintptr_t) hdr - sc->vmaddr;
			}
			if (symtab && strtab)
				goto ok2;
		}
		lc = (void *) lc + lc->cmdsize;
	}
	return; /* uh... weird */
ok2: ;
	symtab = (void *) symtab + *slide;
	strtab = (void *) strtab + *slide;
	/* This could be optimized for efficiency with a large number of names... */
	for (uint32_t i = 0; i < syc.nsyms; i++) {
		substitute_sym *sym = &symtab[i];
		uint32_t strx = sym->n_un.n_strx;
		const char *name = strx == 0 ? "" : strtab + strx;
		for (size_t j = 0; j < count; j++) {
			if (!strcmp(name, names[j])) {
				syms[j] = sym;
				break;
			}
		}
	}
}

/* This is a mess because the usual _dyld_image_count loop is not thread safe.
 * Since it uses a std::vector and (a) erases from it (making it possible for a
 * loop to skip entries) and (b) and doesn't even lock it in
 * _dyld_get_image_header etc., this is true even if the image is guaranteed to
 * be found, including the possibility to crash.  How do we solve this?
 * Inception - we steal dyld's private symbols...  We could avoid the symbols
 * by calling the vtable of dlopen handles, but that seems unstable.  As is,
 * the method used is somewhat convoluted in an attempt to maximize stability.
 */

static void inspect_dyld() {
	const struct dyld_all_image_infos *aii = _dyld_get_all_image_infos();
	const void *dyld_hdr = aii->dyldImageLoadAddress;

	const char *names[2] = { "__ZNK16ImageLoaderMachO8getSlideEv", "__ZNK16ImageLoaderMachO10machHeaderEv" };
	substitute_sym *syms[2];
	intptr_t dyld_slide = -1;
	find_syms_raw(dyld_hdr, &dyld_slide, names, syms, 2);
	if (!syms[0] || !syms[1])
		panic("couldn't find ImageLoader methods\n");
	ImageLoaderMachO_getSlide = sym_to_ptr(syms[0], dyld_slide);
	ImageLoaderMachO_machHeader = sym_to_ptr(syms[1], dyld_slide);
}

/* 'dlopen_header' keeps the image alive */
struct substitute_image *substitute_open_image(const char *filename) {
	pthread_once(&dyld_inspect_once, inspect_dyld);

	void *dlhandle = dlopen(filename, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
	if (!dlhandle)
		return NULL;

	const void *image_header = ImageLoaderMachO_machHeader(dlhandle);
	intptr_t slide = ImageLoaderMachO_getSlide(dlhandle);

	struct substitute_image *im = malloc(sizeof(*im));
	if (!im)
		return NULL;
	im->slide = slide;
	im->dlhandle = dlhandle;
	im->image_header = image_header;
	return im;
}

void substitute_close_image(struct substitute_image *im) {
	dlclose(im->dlhandle); /* ignore errors */
	free(im);
}

int substitute_find_private_syms(struct substitute_image *im, const char **names,
                         substitute_sym **syms, size_t count) {
	find_syms_raw(im->image_header, &im->slide, names, syms, count);
	return SUBSTITUTE_OK;
}

void *substitute_sym_to_ptr(struct substitute_image *handle, substitute_sym *sym) {
	return sym_to_ptr(sym, handle->slide);
}

#endif
