#ifndef OV_CXXRT_H
#define OV_CXXRT_H
// =============================================================================
// cxxrt — минимальный рантайм C++ для программ overOS.
//
// g++ собирает код и без стандартной библиотеки, но несколько вещей язык
// требует от рантайма: operator new/delete (куча), вызов конструкторов
// глобальных объектов до main, заглушка на вызов чисто виртуальной функции.
// Здесь ровно это и ничего больше: без исключений (-fno-exceptions), без RTTI
// (-fno-rtti), без STL. Контейнеры и строки движок держит свои.
//
// В main первым делом (после libui_bind): ovcxx_init();
// =============================================================================

#include <stddef.h>
#include <stdint.h>

extern "C" {
void*  ov_malloc(size_t n);          // память под 16 байт выровнена, 0 — нет памяти
void   ov_free(void* p);
void*  ov_calloc(size_t n, size_t sz);
size_t ov_heap_used(void);           // сколько занято (для отладки)
size_t ov_heap_largest_free(void);   // самый большой свободный кусок
void*  memcpy(void* d, const void* s, size_t n);
void*  memmove(void* d, const void* s, size_t n);
void*  memset(void* d, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
}

// Конструкторы глобальных объектов (.init_array). Вызвать один раз в начале main.
void ovcxx_init();

// Размещающий new: <new> из libstdc++ мы не подключаем
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }

#endif
