#ifndef OVCRAFT_CONFIG_H
#define OVCRAFT_CONFIG_H
// =============================================================================
// config.h — все настройки overCraft в одном месте.
// Поменял число — пересобрал игру (make): ничего другого трогать не нужно.
// =============================================================================

namespace cfg {

// --- Мир
constexpr int   CHUNK        = 16;      // ширина чанка по X и Z, блоков
constexpr int   WORLD_H      = 96;      // высота мира, блоков
constexpr int   SECTION_H    = 16;      // чанк режется по высоте на секции (своя сетка у каждой)
constexpr int   SECTIONS     = WORLD_H / SECTION_H;
constexpr int   VIEW_RADIUS  = 5;       // сколько чанков видно в каждую сторону от игрока
constexpr int   SEA_LEVEL    = 38;      // ниже — вода
constexpr unsigned SEED      = 20261016u;

// Генерация рельефа
constexpr float TERRAIN_BASE   = 42.0f; // средняя высота суши
constexpr float TERRAIN_AMP    = 22.0f; // размах холмов
constexpr float TERRAIN_SCALE  = 1.0f / 96.0f;   // чем меньше, тем шире холмы
constexpr float MOUNTAIN_AMP   = 30.0f; // горы поверх холмов
constexpr float CAVE_SCALE     = 1.0f / 22.0f;
constexpr float CAVE_THRESHOLD = 0.62f; // больше — меньше пещер
constexpr int   TREE_CHANCE    = 90;    // из 10000 клеток травы — дерево

// Сколько чанков за кадр готовить (генерация и сетки идут на всех ядрах)
constexpr int   GEN_PER_FRAME  = 12;
constexpr int   MESH_PER_FRAME = 24;

// --- Игрок
constexpr float PLAYER_W     = 0.6f;    // ширина хитбокса
constexpr float PLAYER_H     = 1.8f;    // рост
constexpr float EYE_H        = 1.62f;   // глаза над ногами
constexpr float WALK_SPEED   = 4.3f;    // блоков в секунду
constexpr float SPRINT_SPEED = 5.6f;
constexpr float FLY_SPEED    = 11.0f;
constexpr float JUMP_SPEED   = 8.4f;    // начальная скорость прыжка (~1.25 блока)
constexpr float GRAVITY      = 28.0f;
constexpr float MAX_FALL     = 50.0f;
constexpr float REACH        = 6.0f;    // до какого блока дотягивается рука
constexpr float MOUSE_SENS   = 0.0028f; // радиан на пиксель мыши
constexpr float BREAK_REPEAT = 0.22f;   // секунд между ударами при зажатой кнопке

// Приседание (Shift), бег (Ctrl или двойное W)
constexpr float SNEAK_SPEED  = 1.3f;
constexpr float PLAYER_H_SNEAK = 1.5f;
constexpr float EYE_H_SNEAK  = 1.27f;
constexpr float SPRINT_JUMP_BOOST = 1.6f;  // рывок вперёд при прыжке на бегу

// --- Выживание (очки как в Minecraft: 20 = 10 сердец / 10 окороков)
constexpr int   MAX_HEALTH   = 20;
constexpr int   MAX_FOOD     = 20;
constexpr float START_SATURATION = 5.0f;
constexpr float SAFE_FALL    = 3.0f;    // с этой высоты падать не больно
constexpr float AIR_SECONDS  = 15.0f;   // запас воздуха под водой (10 пузырей)
constexpr float REGEN_EVERY  = 4.0f;    // секунд на одно очко здоровья, если сыт (голод >= 18)
constexpr float STARVE_EVERY = 4.0f;    // голод 0 — минус очко раз в столько секунд
constexpr int   SPRINT_MIN_FOOD = 7;    // ниже — бежать нет сил
constexpr float EAT_TIME     = 1.6f;    // секунд держать ПКМ с едой
constexpr float INVULN_TIME  = 0.5f;    // после удара столько урон не проходит
constexpr bool  START_CREATIVE = false; // F4 переключает режим
constexpr float BREAK_CREATIVE = 0.2f;  // творческий: блок ломается сразу, повтор — так часто

// --- Мобы и предметы
constexpr int   MAX_PIGS     = 10;      // свиней вокруг игрока не больше
constexpr float SPAWN_MIN    = 20.0f;   // где появляются новые (блоков от игрока)
constexpr float SPAWN_MAX    = 44.0f;
constexpr float DESPAWN      = 96.0f;   // дальше — исчезают
constexpr int   START_PIGS   = 4;       // сразу у точки появления, чтобы было кого найти
constexpr float PICKUP_RANGE = 1.0f;    // на сколько коробка подбора шире игрока
constexpr float ITEM_LIFETIME = 300.0f; // выпавший предмет лежит пять минут
constexpr int   HAND_DAMAGE  = 1;       // урон кулаком
constexpr float ATTACK_REACH = 3.5f;

// --- Камера и картинка
constexpr float FOV_DEG      = 70.0f;
constexpr float FOV_SPRINT   = 1.15f;   // бег: FOV шире (эффект скорости), плавно
constexpr float FOV_FLY      = 1.10f;   // полёт: чуть шире
constexpr float HAND_FOV_DEG = 70.0f;   // рука от первого лица — своя проекция, от бега не меняется
constexpr float THIRD_PERSON_DIST = 4.0f;
constexpr float NEAR_Z       = 0.08f;
// Динамическое разрешение: мир рисуется в доле разрешения окна, и игра сама
// подбирает её, чтобы кадр укладывался в бюджет (интерфейс — всегда в полном)
constexpr bool  DYNAMIC_RES  = true;
constexpr int   RES_MAX      = 100;     // % от окна
constexpr int   RES_MIN      = 55;
constexpr float FRAME_BUDGET = 14.0f;   // мс на кадр игры (60 FPS — 16.7, остальное композитору)
constexpr int   MAX_THREADS  = -1;      // рабочих ядер для движка: -1 — все свободные
constexpr unsigned SKY_TOP    = 0x6FA8E8;
constexpr unsigned SKY_BOTTOM = 0xC8DCF0;
constexpr float FOG_START    = (VIEW_RADIUS - 1.5f) * CHUNK;
constexpr float FOG_END      = (VIEW_RADIUS - 0.2f) * CHUNK;

// --- Физика: шаг фиксированный, чтобы прыжок не зависел от FPS
constexpr float PHYS_DT      = 1.0f / 120.0f;

} // namespace cfg

#endif
