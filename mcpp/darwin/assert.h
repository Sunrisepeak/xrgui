// Apple's <assert.h>, with the branch hint taken out.
//
// The SDK's assert() expands to `__builtin_expect(!(e), 0) ? __assert_rtn(...)
// : (void)0`. Named in a C++20 module purview, clang 22 attaches the builtin's
// implicit declaration to that module; a unit that imports two such modules
// and then asserts itself is told the call is ambiguous (elem_ptr + magic_enum,
// elem_ptr + plf_hive, ...). Neither glibc's nor MSVC's assert() names a
// builtin, so only this platform sees it.
//
// build.mcpp puts this directory on the package's own -I on macOS, ahead of
// the SDK, so every <cassert> / <assert.h> in the tree reaches the SDK's file
// through #include_next and then loses the hint. Same __assert_rtn, same
// message, same NDEBUG behaviour. No include guard, like the original: a
// re-include after a change of NDEBUG must redefine the macro.
#include_next <assert.h>

#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) \
    ((e) ? (void)0 : __assert_rtn(__func__, __ASSERT_FILE_NAME, __LINE__, #e))
#endif
