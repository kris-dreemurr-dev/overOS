#ifndef OV_ASSERT_H
#define OV_ASSERT_H
// C11: static_assert — макрос из assert.h
#ifndef static_assert
#define static_assert _Static_assert
#endif
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void ov_assert_fail(const char* expr, const char* file, int line);
#define assert(x) ((x) ? (void)0 : ov_assert_fail(#x, __FILE__, __LINE__))
#endif
#endif
