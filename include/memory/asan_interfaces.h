// NOTE:
// Copied from #include <sentinizer/asan_interface.h>
// Clangd has problems in recognizing this file.
//===-- sanitizer/asan_interface.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer (ASan).
//
// Public interface header.
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_ASAN_INTERFACE_H
#define SANITIZER_ASAN_INTERFACE_H

#ifndef SANITIZER_COMMON_INTERFACE_DEFS_H
#define SANITIZER_COMMON_INTERFACE_DEFS_H

#include <stddef.h>
#include <stdint.h>

// GCC does not understand __has_feature.
#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    // Arguments for __sanitizer_sandbox_on_notify() below.
    typedef struct
    {
        // Enable sandbox support in sanitizer coverage.
        int coverage_sandboxed;
        // File descriptor to write coverage data to. If -1 is passed, a file
        // will be pre-opened by __sanitizer_sandobx_on_notify(). This field has
        // no effect if coverage_sandboxed == 0.
        intptr_t coverage_fd;
        // If non-zero, split the coverage data into well-formed blocks. This is
        // useful when coverage_fd is a socket descriptor. Each block will
        // contain a header, allowing data from multiple processes to be sent
        // over the same socket.
        unsigned int coverage_max_block_size;
    } __sanitizer_sandbox_arguments;

    // Tell the tools to write their reports to "path.<pid>" instead of stderr.
    void __sanitizer_set_report_path(const char *path);
    // Tell the tools to write their reports to the provided file descriptor
    // (casted to void *).
    void __sanitizer_set_report_fd(void *fd);

    // Notify the tools that the sandbox is going to be turned on. The reserved
    // parameter will be used in the future to hold a structure with functions
    // that the tools may call to bypass the sandbox.
    void __sanitizer_sandbox_on_notify(__sanitizer_sandbox_arguments *args);

    // This function is called by the tool when it has just finished reporting
    // an error. 'error_summary' is a one-line string that summarizes
    // the error message. This function can be overridden by the client.
    void __sanitizer_report_error_summary(const char *error_summary);

    // Some of the sanitizers (for example ASan/TSan) could miss bugs that
    // happen in unaligned loads/stores. To find such bugs reliably, you need to
    // replace plain unaligned loads/stores with these calls.

    /// Loads a 16-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    ///
    /// \returns Loaded value.
    uint16_t __sanitizer_unaligned_load16(const void *p);

    /// Loads a 32-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    ///
    /// \returns Loaded value.
    uint32_t __sanitizer_unaligned_load32(const void *p);

    /// Loads a 64-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    ///
    /// \returns Loaded value.
    uint64_t __sanitizer_unaligned_load64(const void *p);

    /// Stores a 16-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    /// \param x 16-bit value to store.
    void __sanitizer_unaligned_store16(void *p, uint16_t x);

    /// Stores a 32-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    /// \param x 32-bit value to store.
    void __sanitizer_unaligned_store32(void *p, uint32_t x);

    /// Stores a 64-bit unaligned value.
    ///
    /// \param p Pointer to unaligned memory.
    /// \param x 64-bit value to store.
    void __sanitizer_unaligned_store64(void *p, uint64_t x);

    // Returns 1 on the first call, then returns 0 thereafter.  Called by the
    // tool to ensure only one report is printed when multiple errors occur
    // simultaneously.
    int __sanitizer_acquire_crash_state();

    /// Annotates the current state of a contiguous container, such as
    /// <c>std::vector</c>, <c>std::string</c>, or similar.
    ///
    /// A contiguous container is a container that keeps all of its elements
    /// in a contiguous region of memory. The container owns the region of
    /// memory <c>[beg, end)</c>; the memory <c>[beg, mid)</c> is used to store
    /// the current elements, and the memory <c>[mid, end)</c> is reserved for
    /// future elements (<c>beg <= mid <= end</c>). For example, in
    /// <c>std::vector<> v</c>:
    ///
    /// \code
    ///   beg = &v[0];
    ///   end = beg + v.capacity() * sizeof(v[0]);
    ///   mid = beg + v.size()     * sizeof(v[0]);
    /// \endcode
    ///
    /// This annotation tells the Sanitizer tool about the current state of the
    /// container so that the tool can report errors when memory from
    /// <c>[mid, end)</c> is accessed. Insert this annotation into methods like
    /// <c>push_back()</c> or <c>pop_back()</c>. Supply the old and new values
    /// of <c>mid</c>(<c><i>old_mid</i></c> and <c><i>new_mid</i></c>). In the
    /// initial state <c>mid == end</c>, so that should be the final state when
    /// the container is destroyed or when the container reallocates the
    /// storage.
    ///
    /// For ASan, <c><i>beg</i></c> should be 8-aligned and <c><i>end</i></c>
    /// should be either 8-aligned or it should point to the end of a separate
    /// heap-, stack-, or global-allocated buffer. So the following example will
    /// not work:
    ///
    /// \code
    ///   int64_t x[2]; // 16 bytes, 8-aligned
    ///   char *beg = (char *)&x[0];
    ///   char *end = beg + 12; // Not 8-aligned, not the end of the buffer
    /// \endcode
    ///
    /// The following, however, will work:
    /// \code
    ///   int32_t x[3]; // 12 bytes, but 8-aligned under ASan.
    ///   char *beg = (char*)&x[0];
    ///   char *end = beg + 12; // Not 8-aligned, but is the end of the buffer
    /// \endcode
    ///
    /// \note  Use this function with caution and do not use for anything other
    /// than vector-like classes.
    ///
    /// \param beg Beginning of memory region.
    /// \param end End of memory region.
    /// \param old_mid Old middle of memory region.
    /// \param new_mid New middle of memory region.
    void __sanitizer_annotate_contiguous_container(const void *beg,
                                                   const void *end,
                                                   const void *old_mid,
                                                   const void *new_mid);

    /// Returns true if the contiguous container <c>[beg, end)</c> is properly
    /// poisoned.
    ///
    /// Proper poisoning could occur, for example, with
    /// <c>__sanitizer_annotate_contiguous_container</c>), that is, if
    /// <c>[beg, mid)</c> is addressable and <c>[mid, end)</c> is unaddressable.
    /// Full verification requires O (<c>end - beg</c>) time; this function
    /// tries to avoid such complexity by touching only parts of the container
    /// around <c><i>beg</i></c>, <c><i>mid</i></c>, and <c><i>end</i></c>.
    ///
    /// \param beg Beginning of memory region.
    /// \param mid Middle of memory region.
    /// \param end Old end of memory region.
    ///
    /// \returns True if the contiguous container <c>[beg, end)</c> is properly
    ///  poisoned.
    int __sanitizer_verify_contiguous_container(const void *beg,
                                                const void *mid,
                                                const void *end);

    /// Similar to <c>__sanitizer_verify_contiguous_container()</c> but also
    /// returns the address of the first improperly poisoned byte.
    ///
    /// Returns NULL if the area is poisoned properly.
    ///
    /// \param beg Beginning of memory region.
    /// \param mid Middle of memory region.
    /// \param end Old end of memory region.
    ///
    /// \returns The bad address or NULL.
    const void *__sanitizer_contiguous_container_find_bad_address(
        const void *beg, const void *mid, const void *end);

    /// Prints the stack trace leading to this call (useful for calling from the
    /// debugger).
    void __sanitizer_print_stack_trace(void);

    // Symbolizes the supplied 'pc' using the format string 'fmt'.
    // Outputs at most 'out_buf_size' bytes into 'out_buf'.
    // If 'out_buf' is not empty then output is zero or more non empty C strings
    // followed by single empty C string. Multiple strings can be returned if PC
    // corresponds to inlined function. Inlined frames are printed in the order
    // from "most-inlined" to the "least-inlined", so the last frame should be
    // the not inlined function. Inlined frames can be removed with
    // 'symbolize_inline_frames=0'. The format syntax is described in
    // lib/sanitizer_common/sanitizer_stacktrace_printer.h.
    void __sanitizer_symbolize_pc(void *pc,
                                  const char *fmt,
                                  char *out_buf,
                                  size_t out_buf_size);
    // Same as __sanitizer_symbolize_pc, but for data section (i.e. globals).
    void __sanitizer_symbolize_global(void *data_ptr,
                                      const char *fmt,
                                      char *out_buf,
                                      size_t out_buf_size);

    /// Sets the callback to be called immediately before death on error.
    ///
    /// Passing 0 will unset the callback.
    ///
    /// \param callback User-provided callback.
    void __sanitizer_set_death_callback(void (*callback)(void));

    // Interceptor hooks.
    // Whenever a libc function interceptor is called, it checks if the
    // corresponding weak hook is defined, and calls it if it is indeed defined.
    // The primary use-case is data-flow-guided fuzzing, where the fuzzer needs
    // to know what is being passed to libc functions (for example memcmp).
    // FIXME: implement more hooks.

    /// Interceptor hook for <c>memcmp()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param n Number of bytes to compare.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_memcmp(
        void *called_pc, const void *s1, const void *s2, size_t n, int result);

    /// Interceptor hook for <c>strncmp()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param n Number of bytes to compare.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_strncmp(
        void *called_pc, const char *s1, const char *s2, size_t n, int result);

    /// Interceptor hook for <c>strncasecmp()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param n Number of bytes to compare.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_strncasecmp(
        void *called_pc, const char *s1, const char *s2, size_t n, int result);

    /// Interceptor hook for <c>strcmp()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_strcmp(void *called_pc,
                                      const char *s1,
                                      const char *s2,
                                      int result);

    /// Interceptor hook for <c>strcasecmp()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_strcasecmp(void *called_pc,
                                          const char *s1,
                                          const char *s2,
                                          int result);

    /// Interceptor hook for <c>strstr()</c>.
    ///
    /// \param called_pc PC (program counter) address of the original call.
    /// \param s1 Pointer to block of memory.
    /// \param s2 Pointer to block of memory.
    /// \param result Value returned by the intercepted function.
    void __sanitizer_weak_hook_strstr(void *called_pc,
                                      const char *s1,
                                      const char *s2,
                                      char *result);

    void __sanitizer_weak_hook_strcasestr(void *called_pc,
                                          const char *s1,
                                          const char *s2,
                                          char *result);

    void __sanitizer_weak_hook_memmem(void *called_pc,
                                      const void *s1,
                                      size_t len1,
                                      const void *s2,
                                      size_t len2,
                                      void *result);

    // Prints stack traces for all live heap allocations ordered by total
    // allocation size until top_percent of total live heap is shown.
    // top_percent should be between 1 and 100. At most max_number_of_contexts
    // contexts (stack traces) are printed. Experimental feature currently
    // available only with ASan on Linux/x86_64.
    void __sanitizer_print_memory_profile(size_t top_percent,
                                          size_t max_number_of_contexts);

    /// Notify ASan that a fiber switch has started (required only if
    /// implementing your own fiber library).
    ///
    /// Before switching to a different stack, you must call
    /// <c>__sanitizer_start_switch_fiber()</c> with a pointer to the bottom of
    /// the destination stack and with its size. When code starts running on the
    /// new stack, it must call <c>__sanitizer_finish_switch_fiber()</c> to
    /// finalize the switch. The <c>__sanitizer_start_switch_fiber()</c>
    /// function takes a <c>void**</c> pointer argument to store the current
    /// fake stack if there is one (it is necessary when the runtime option
    /// <c>detect_stack_use_after_return</c> is enabled).
    ///
    /// When restoring a stack, this <c>void**</c> pointer must be given to the
    /// <c>__sanitizer_finish_switch_fiber()</c> function. In most cases, this
    /// pointer can be stored on the stack immediately before switching. When
    /// leaving a fiber definitely, NULL must be passed as the first argument to
    /// the <c>__sanitizer_start_switch_fiber()</c> function so that the fake
    /// stack is destroyed. If your program does not need stack use-after-return
    /// detection, you can always pass NULL to these two functions.
    ///
    /// \note The fake stack mechanism is disabled during fiber switch, so if a
    /// signal callback runs during the switch, it will not benefit from stack
    /// use-after-return detection.
    ///
    /// \param fake_stack_save [out] Fake stack save location.
    /// \param bottom Bottom address of stack.
    /// \param size Size of stack in bytes.
    void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                        const void *bottom,
                                        size_t size);

    /// Notify ASan that a fiber switch has completed (required only if
    /// implementing your own fiber library).
    ///
    /// When code starts running on the new stack, it must call
    /// <c>__sanitizer_finish_switch_fiber()</c> to finalize
    /// the switch. For usage details, see the description of
    /// <c>__sanitizer_start_switch_fiber()</c>.
    ///
    /// \param fake_stack_save Fake stack save location.
    /// \param bottom_old [out] Bottom address of old stack.
    /// \param size_old [out] Size of old stack in bytes.
    void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                         const void **bottom_old,
                                         size_t *size_old);

    // Get full module name and calculate pc offset within it.
    // Returns 1 if pc belongs to some module, 0 if module was not found.
    int __sanitizer_get_module_and_offset_for_pc(void *pc,
                                                 char *module_path,
                                                 size_t module_path_len,
                                                 void **pc_offset);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SANITIZER_COMMON_INTERFACE_DEFS_H

#ifdef __cplusplus
extern "C"
{
#endif
    /// Marks a memory region (<c>[addr, addr+size)</c>) as unaddressable.
    ///
    /// This memory must be previously allocated by your program. Instrumented
    /// code is forbidden from accessing addresses in this region until it is
    /// unpoisoned. This function is not guaranteed to poison the entire region
    /// - it could poison only a subregion of <c>[addr, addr+size)</c> due to
    /// ASan alignment restrictions.
    ///
    /// \note This function is not thread-safe because no two threads can poison
    /// or unpoison memory in the same memory region simultaneously.
    ///
    /// \param addr Start of memory region.
    /// \param size Size of memory region.
    void __asan_poison_memory_region(void const volatile *addr, size_t size);

    /// Marks a memory region (<c>[addr, addr+size)</c>) as addressable.
    ///
    /// This memory must be previously allocated by your program. Accessing
    /// addresses in this region is allowed until this region is poisoned again.
    /// This function could unpoison a super-region of <c>[addr, addr+size)</c>
    /// due to ASan alignment restrictions.
    ///
    /// \note This function is not thread-safe because no two threads can
    /// poison or unpoison memory in the same memory region simultaneously.
    ///
    /// \param addr Start of memory region.
    /// \param size Size of memory region.
    void __asan_unpoison_memory_region(void const volatile *addr, size_t size);

// Macros provided for convenience.
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
/// Marks a memory region as unaddressable.
///
/// \note Macro provided for convenience; defined as a no-op if ASan is not
/// enabled.
///
/// \param addr Start of memory region.
/// \param size Size of memory region.
#define ASAN_POISON_MEMORY_REGION(addr, size) \
    __asan_poison_memory_region((addr), (size))

/// Marks a memory region as addressable.
///
/// \note Macro provided for convenience; defined as a no-op if ASan is not
/// enabled.
///
/// \param addr Start of memory region.
/// \param size Size of memory region.
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) \
    __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) ((void) (addr), (void) (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void) (addr), (void) (size))
#endif

    /// Checks if an address is poisoned.
    ///
    /// Returns 1 if <c><i>addr</i></c> is poisoned (that is, 1-byte read/write
    /// access to this address would result in an error report from ASan).
    /// Otherwise returns 0.
    ///
    /// \param addr Address to check.
    ///
    /// \retval 1 Address is poisoned.
    /// \retval 0 Address is not poisoned.
    int __asan_address_is_poisoned(void const volatile *addr);

    /// Checks if a region is poisoned.
    ///
    /// If at least one byte in <c>[beg, beg+size)</c> is poisoned, returns the
    /// address of the first such byte. Otherwise returns 0.
    ///
    /// \param beg Start of memory region.
    /// \param size Start of memory region.
    /// \returns Address of first poisoned byte.
    void *__asan_region_is_poisoned(void *beg, size_t size);

    /// Describes an address (useful for calling from the debugger).
    ///
    /// Prints the description of <c><i>addr</i></c>.
    ///
    /// \param addr Address to describe.
    void __asan_describe_address(void *addr);

    /// Checks if an error has been or is being reported (useful for calling
    /// from the debugger to get information about an ASan error).
    ///
    /// Returns 1 if an error has been (or is being) reported. Otherwise returns
    /// 0.
    ///
    /// \returns 1 if an error has been (or is being) reported. Otherwise
    /// returns 0.
    int __asan_report_present(void);

    /// Gets the PC (program counter) register value of an ASan error (useful
    /// for calling from the debugger).
    ///
    /// Returns PC if an error has been (or is being) reported.
    /// Otherwise returns 0.
    ///
    /// \returns PC value.
    void *__asan_get_report_pc(void);

    /// Gets the BP (base pointer) register value of an ASan error (useful for
    /// calling from the debugger).
    ///
    /// Returns BP if an error has been (or is being) reported.
    /// Otherwise returns 0.
    ///
    /// \returns BP value.
    void *__asan_get_report_bp(void);

    /// Gets the SP (stack pointer) register value of an ASan error (useful for
    /// calling from the debugger).
    ///
    /// If an error has been (or is being) reported, returns SP.
    /// Otherwise returns 0.
    ///
    /// \returns SP value.
    void *__asan_get_report_sp(void);

    /// Gets the address of the report buffer of an ASan error (useful for
    /// calling from the debugger).
    ///
    /// Returns the address of the report buffer if an error has been (or is
    /// being) reported. Otherwise returns 0.
    ///
    /// \returns Address of report buffer.
    void *__asan_get_report_address(void);

    /// Gets access type of an ASan error (useful for calling from the
    /// debugger).
    ///
    /// Returns access type (read or write) if an error has been (or is being)
    /// reported. Otherwise returns 0.
    ///
    /// \returns Access type (0 = read, 1 = write).
    int __asan_get_report_access_type(void);

    /// Gets access size of an ASan error (useful for calling from the
    /// debugger).
    ///
    /// Returns access size if an error has been (or is being) reported.
    /// Otherwise returns 0.
    ///
    /// \returns Access size in bytes.
    size_t __asan_get_report_access_size(void);

    /// Gets the bug description of an ASan error (useful for calling from a
    /// debugger).
    ///
    /// \returns Returns a bug description if an error has been (or is being)
    /// reported - for example, "heap-use-after-free". Otherwise returns an
    /// empty string.
    const char *__asan_get_report_description(void);

    /// Gets information about a pointer (useful for calling from the debugger).
    ///
    /// Returns the category of the given pointer as a constant string.
    /// Possible return values are <c>global</c>, <c>stack</c>,
    /// <c>stack-fake</c>, <c>heap</c>, <c>heap-invalid</c>, <c>shadow-low</c>,
    /// <c>shadow-gap</c>, <c>shadow-high</c>, and <c>unknown</c>.
    ///
    /// If the return value is <c>global</c> or <c>stack</c>, tries to also
    /// return the variable name, address, and size. If the return value is
    /// <c>heap</c>, tries to return the chunk address and size.
    /// <c><i>name</i></c> should point to an allocated buffer of size
    /// <c><i>name_size</i></c>.
    ///
    /// \param addr Address to locate.
    /// \param name Buffer to store the variable's name.
    /// \param name_size Size in bytes of the variable's name buffer.
    /// \param region_address [out] Address of the region.
    /// \param region_size [out] Size of the region in bytes.
    ///
    /// \returns Returns the category of the given pointer as a constant string.
    const char *__asan_locate_address(void *addr,
                                      char *name,
                                      size_t name_size,
                                      void **region_address,
                                      size_t *region_size);

    /// Gets the allocation stack trace and thread ID for a heap address (useful
    /// for calling from the debugger).
    ///
    /// Stores up to <c><i>size</i></c> frames in <c><i>trace</i></c>. Returns
    /// the number of stored frames or 0 on error.
    ///
    /// \param addr A heap address.
    /// \param trace A buffer to store the stack trace.
    /// \param size Size in bytes of the trace buffer.
    /// \param thread_id [out] The thread ID of the address.
    ///
    /// \returns Returns the number of stored frames or 0 on error.
    size_t __asan_get_alloc_stack(void *addr,
                                  void **trace,
                                  size_t size,
                                  int *thread_id);

    /// Gets the free stack trace and thread ID for a heap address (useful for
    /// calling from the debugger).
    ///
    /// Stores up to <c><i>size</i></c> frames in <c><i>trace</i></c>. Returns
    /// the number of stored frames or 0 on error.
    ///
    /// \param addr A heap address.
    /// \param trace A buffer to store the stack trace.
    /// \param size Size in bytes of the trace buffer.
    /// \param thread_id [out] The thread ID of the address.
    ///
    /// \returns Returns the number of stored frames or 0 on error.
    size_t __asan_get_free_stack(void *addr,
                                 void **trace,
                                 size_t size,
                                 int *thread_id);

    /// Gets the current shadow memory mapping (useful for calling from the
    /// debugger).
    ///
    /// \param shadow_scale [out] Shadow scale value.
    /// \param shadow_offset [out] Offset value.
    void __asan_get_shadow_mapping(size_t *shadow_scale, size_t *shadow_offset);

    /// This is an internal function that is called to report an error. However,
    /// it is still a part of the interface because you might want to set a
    /// breakpoint on this function in the debugger.
    ///
    /// \param pc <c><i>pc</i></c> value of the ASan error.
    /// \param bp <c><i>bp</i></c> value of the ASan error.
    /// \param sp <c><i>sp</i></c> value of the ASan error.
    /// \param addr Address of the ASan error.
    /// \param is_write True if the error is a write error; false otherwise.
    /// \param access_size Size of the memory access of the ASan error.
    void __asan_report_error(void *pc,
                             void *bp,
                             void *sp,
                             void *addr,
                             int is_write,
                             size_t access_size);

    // Deprecated. Call __sanitizer_set_death_callback instead.
    void __asan_set_death_callback(void (*callback)(void));

    /// Sets the callback function to be called during ASan error reporting.
    ///
    /// The callback provides a string pointer to the report.
    ///
    /// \param callback User-provided function.
    void __asan_set_error_report_callback(void (*callback)(const char *));

    /// User-provided callback on ASan errors.
    ///
    /// You can provide a function that would be called immediately when ASan
    /// detects an error. This is useful in cases when ASan detects an error but
    /// your program crashes before the ASan report is printed.
    void __asan_on_error(void);

    /// Prints accumulated statistics to <c>stderr</c> (useful for calling from
    /// the debugger).
    void __asan_print_accumulated_stats(void);

    /// User-provided default option settings.
    ///
    /// You can provide your own implementation of this function to return a
    /// string containing ASan runtime options (for example,
    /// <c>verbosity=1:halt_on_error=0</c>).
    ///
    /// \returns Default options string.
    const char *__asan_default_options(void);

    // The following two functions facilitate garbage collection in presence of
    // ASan's fake stack.

    /// Gets an opaque handler to the current thread's fake stack.
    ///
    /// Returns an opaque handler to be used by
    /// <c>__asan_addr_is_in_fake_stack()</c>. Returns NULL if the current
    /// thread does not have a fake stack.
    ///
    /// \returns An opaque handler to the fake stack or NULL.
    void *__asan_get_current_fake_stack(void);

    /// Checks if an address belongs to a given fake stack.
    ///
    /// If <c><i>fake_stack</i></c> is non-NULL and <c><i>addr</i></c> belongs
    /// to a fake frame in <c><i>fake_stack</i></c>, returns the address of the
    /// real stack that corresponds to the fake frame and sets <c><i>beg</i></c>
    /// and <c><i>end</i></c> to the boundaries of this fake frame. Otherwise
    /// returns NULL and does not touch <c><i>beg</i></c> and <c><i>end</i></c>.
    ///
    /// If <c><i>beg</i></c> or <c><i>end</i></c> are NULL, they are not
    /// touched.
    ///
    /// \note This function can be called from a thread other than the owner of
    /// <c><i>fake_stack</i></c>, but the owner thread needs to be alive.
    ///
    /// \param fake_stack An opaque handler to a fake stack.
    /// \param addr Address to test.
    /// \param beg [out] Beginning of fake frame.
    /// \param end [out] End of fake frame.
    /// \returns Stack address or NULL.
    void *__asan_addr_is_in_fake_stack(void *fake_stack,
                                       void *addr,
                                       void **beg,
                                       void **end);

    /// Performs shadow memory cleanup of the current thread's stack before a
    /// function marked with the <c>[[noreturn]]</c> attribute is called.
    ///
    /// To avoid false positives on the stack, must be called before no-return
    /// functions like <c>_exit()</c> and <c>execl()</c>.
    void __asan_handle_no_return(void);

    /// Update allocation stack trace for the given allocation to the current
    /// stack trace. Returns 1 if successfull, 0 if not.
    int __asan_update_allocation_context(void *addr);

#ifdef __cplusplus
}  // extern "C"
#endif

#if defined(__clang__) || defined(__GNUC__)
#define ATTRIBUTE_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
#define ATTRIBUTE_NO_SANITIZE_ADDRESS
#endif

#endif  // SANITIZER_ASAN_INTERFACE_H
