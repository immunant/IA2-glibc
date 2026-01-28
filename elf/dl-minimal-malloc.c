/* Minimal malloc implementation for dynamic linker and static
   initialization.
   Copyright (C) 1995-2025 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/* Mark symbols hidden in static PIE for early self relocation to work.
    Note: string.h may have ifuncs which cannot be hidden on i686.  */
#if BUILD_PIE_DEFAULT
# pragma GCC visibility push(hidden)
#endif
#include <assert.h>
#include <string.h>
#include <ldsodefs.h>
#include <malloc/malloc-internal.h>
#include <setvmaname.h>
/*
 * IA2_LDSO_PKEY: When defined to a positive value, enables MPK protection
 * for the loader's minimal malloc heap using that pkey. The loader allocates
 * this pkey before any application code runs. The IA2 runtime must be built
 * with the same IA2_LDSO_PKEY value so it knows to skip allocating that pkey.
 */
#if defined(__x86_64__) && defined(IA2_LDSO_PKEY) && IA2_LDSO_PKEY > 0
#include <bits/mman-shared.h>
#include <sysdep.h>
#include <sys/prctl.h>
#include "ia2_ldso_heap.h"

/* Whether the loader pkey was successfully allocated. */
static int ia2_ldso_pkey_ok;

/* Allocate the loader pkey on first use. The loader runs before any user
   code, so we're guaranteed to get pkey 1 on first allocation. */
static inline int
ia2_ensure_ldso_pkey (void)
{
  if (!ia2_ldso_pkey_ok)
    {
      long int ret = INTERNAL_SYSCALL_CALL (pkey_alloc, 0, 0);
      ia2_ldso_pkey_ok = (!INTERNAL_SYSCALL_ERROR_P (ret) && ret == IA2_LDSO_PKEY);
    }
  return ia2_ldso_pkey_ok;
}
#endif

static void *alloc_ptr, *alloc_end, *alloc_last_block;

/* Allocate an aligned memory block.  */
void *
__minimal_malloc (size_t n)
{
  if (alloc_end == NULL)
    {
      /* Consume any unused space in the last page of our data segment.  */
      extern int _end attribute_hidden;
      alloc_ptr = &_end;
      alloc_end = (void *) 0 + ((((uintptr_t) alloc_ptr)
				 + GLRO(dl_pagesize) - 1)
				& ~(GLRO(dl_pagesize) - 1));
    }

  /* Make sure the allocation pointer is ideally aligned.  */
  alloc_ptr = (void *) 0 + ((((uintptr_t) alloc_ptr) + MALLOC_ALIGNMENT - 1)
			    & ~(MALLOC_ALIGNMENT - 1));

  if (alloc_ptr + n >= alloc_end || n >= -(uintptr_t) alloc_ptr)
    {
      /* Insufficient space left; allocate another page plus one extra
	 page to reduce number of mmap calls.  */
      caddr_t page;
      size_t nup = (n + GLRO(dl_pagesize) - 1) & ~(GLRO(dl_pagesize) - 1);
      if (__glibc_unlikely (nup == 0 && n != 0))
	return NULL;
      nup += GLRO(dl_pagesize);
      page = __mmap (NULL, nup, PROT_READ|PROT_WRITE,
		     MAP_ANON|MAP_PRIVATE, -1, 0);
      if (page == MAP_FAILED)
	return NULL;
#if defined(__x86_64__) && defined(IA2_LDSO_PKEY) && IA2_LDSO_PKEY > 0
      if (ia2_ensure_ldso_pkey ())
        {
          if (__pkey_mprotect (page, nup, PROT_READ | PROT_WRITE, IA2_LDSO_PKEY) != 0)
            {
              __munmap (page, nup);
              return NULL;
            }
          /* Set VMA name directly because __set_vma_name checks the
             glibc.mem.decorate_maps tunable which defaults to off */
          INTERNAL_SYSCALL_CALL (prctl, PR_SET_VMA, PR_SET_VMA_ANON_NAME,
                                 page, nup, IA2_LDSO_HEAP_NAME);
        }
      else
        __set_vma_name (page, nup, " glibc: loader malloc");
#else
      __set_vma_name (page, nup, " glibc: loader malloc");
#endif
      if (page != alloc_end)
	alloc_ptr = page;
      alloc_end = page + nup;
    }

  alloc_last_block = (void *) alloc_ptr;
  alloc_ptr += n;
  return alloc_last_block;
}

/* We use this function occasionally since the real implementation may
   be optimized when it can assume the memory it returns already is
   set to NUL.  */
void *
__minimal_calloc (size_t nmemb, size_t size)
{
  /* New memory from the trivial malloc above is always already cleared.
     (We make sure that's true in the rare occasion it might not be,
     by clearing memory in free, below.)  */
  size_t bytes = nmemb * size;

#define HALF_SIZE_T (((size_t) 1) << (8 * sizeof (size_t) / 2))
  if (__builtin_expect ((nmemb | size) >= HALF_SIZE_T, 0)
      && size != 0 && bytes / size != nmemb)
    return NULL;

  return malloc (bytes);
}

/* This will rarely be called.  */
void
__minimal_free (void *ptr)
{
  /* We can free only the last block allocated.  */
  if (ptr == alloc_last_block)
    {
      /* Since this is rare, we clear the freed block here
	 so that calloc can presume malloc returns cleared memory.  */
      memset (alloc_last_block, '\0', alloc_ptr - alloc_last_block);
      alloc_ptr = alloc_last_block;
    }
}

/* This is only called with the most recent block returned by malloc.  */
void *
__minimal_realloc (void *ptr, size_t n)
{
  if (ptr == NULL)
    return malloc (n);
  assert (ptr == alloc_last_block);
  size_t old_size = alloc_ptr - alloc_last_block;
  alloc_ptr = alloc_last_block;
  void *new = malloc (n);
  return new != ptr ? memcpy (new, ptr, old_size) : new;
}
