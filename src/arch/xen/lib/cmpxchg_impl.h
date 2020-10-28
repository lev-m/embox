/**
 * @file
 * @brief Test and Set Lock for x86
 *
 * @date 19.07.13
 * @author Ilia Vaprol
 */

#ifndef ARCH_X86_LIB_CMPXCHG_IMPL_H_
#define ARCH_X86_LIB_CMPXCHG_IMPL_H_

#ifndef __ASSEMBLER__

#define __HAVE_ARCH_CMPXCHG

static inline unsigned long cmpxchg(unsigned long *ptr,
		unsigned long old_val, unsigned long new_val) {
	unsigned long ret;

	__asm__ __volatile__ (
			"lock cmpxchgl %2, %1"
			: "=a" (ret), "+m" (*ptr)
			: "r" (new_val), "0" (old_val)
			: "memory"
	);

	return ret;
}

#define cas(_ptr, _old, _new) ({ \
	unsigned long* __ptr = (unsigned long*)_ptr; \
	unsigned long __old = (unsigned long)_old; \
	unsigned long __new = (unsigned long)_new; \
	__old == cmpxchg(__ptr, __old, __new); \
})

#endif /* !__ASSEMBLER__ */

#endif /* !ARCH_X86_LIB_CMPXCHG_IMPL_H_ */
