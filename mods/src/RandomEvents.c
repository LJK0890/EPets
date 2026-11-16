/* ============================================================
 * EPets · 随机事件模组
 *   10 个日常随机事件，只注册 KIND_EVENT
 *
 * 编译: cl /LD /utf-8 /I. Events.c /Fe:Events.dll
 *
 * 引擎按 weight 加权抽取、显示 text，然后调用 on_fire。
 * 事件自己能做的事都通过加载器服务完成（加物品 / 扣血 / 触发被动）。
 * ============================================================ */

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>

static const LoaderApi *g_api = 0;
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

/* ============ 小工具 ============ */

static uint8_t inc(uint8_t v, int n)
{
    int r = (int)v + n;
    return (uint8_t)(r > MAX_STAT ? MAX_STAT : r);
}

static uint8_t dec(uint8_t v, int n)
{
    int r = (int)v - n;
    return (uint8_t)(r < MIN_STAT ? MIN_STAT : r);
}

/* ============ 事件效果 ============ */

/* 找到鲁尼：先摇一次被动，再捡钱 */
static int ev_coins(Pet *pet)
{
    g_api->passive_try(pet);
    pet->coins += g_api->rnd(10, 40);
    return 1;
}

/* 摔倒受伤：扣血交给加载器（会自动处理闪避/减伤），没被抵消才掉心情 */
static int ev_fall(Pet *pet)
{
    if (!g_api->damage(pet, 15)) return 1;
    pet->happy = dec(pet->happy, 10);
    return 1;
}

/* 遇到友好动物 */
static int ev_friend(Pet *pet)
{
    pet->happy = inc(pet->happy, 20);
    return 1;
}

/* 发现洞穴：被动先生效，否则 50% 捡到零钱 */
static int ev_cave(Pet *pet)
{
    if (g_api->passive_try(pet) != PASSIVE_FAIL) return 1;
    if (g_api->rnd(0, 1) == 0) pet->coins += g_api->rnd(5, 25);
    return 1;
}

/* 天气变化 */
static int ev_weather(Pet *pet)
{
    pet->happy = dec(pet->happy, 5);
    return 1;
}

/* 收到礼物：按名字找物品，不依赖 Id */
static int ev_gift(Pet *pet)
{
    Id delic = g_api->find(KIND_ITEM, "鲜食");
    if (delic != ID_NONE) g_api->inv_add(pet, delic, 1);
    return 1;
}

/* 美梦 */
static int ev_dream(Pet *pet)
{
    pet->happy = inc(pet->happy, 10);
    return 1;
}

/* 吃坏肚子 */
static int ev_sick(Pet *pet)
{
    pet->health = dec(pet->health, 10);
    pet->clean = dec(pet->clean, 15);
    return 1;
}

/* 幼年痕迹：只有幼年/少年才会弄脏 */
static int ev_toddler(Pet *pet)
{
    if (pet->stage == S_BABY || pet->stage == S_CHILD)
        pet->clean = dec(pet->clean, 20);
    return 1;
}

/* 找到小虫 */
static int ev_bug(Pet *pet)
{
    pet->happy = inc(pet->happy, 15);
    return 1;
}

/* ============ 事件表 ============ */

/* 注册顺序即本模组的 Id 顺序（1~10），weight 全为 1（等概率） */
static const Event_t EVENTS[] = {
    {"找到鲁尼",     "%NAME% 找到了一些  ！", 1, ev_coins},
    {"摔倒受伤",     "%NAME% 摔了一跤，受伤了！",     1, ev_fall},
    {"友好动物",     "%NAME% 遇到了友好的小动物！",   1, ev_friend},
    {"发现洞穴",     "发现了一个洞穴……",             1, ev_cave},
    {"天气变化",     "天气变阴了，%NAME% 有点担心……", 1, ev_weather},
    {"收到礼物",     "%NAME% 收到了神秘礼物！",       1, ev_gift},
    {"美梦",         "%NAME% 做了一个美梦~",          1, ev_dream},
    {"吃坏肚子",     "%NAME% 吃坏肚子了！",           1, ev_sick},
    {"留点痕迹",     "%NAME% 留下了一些小痕迹……",    1, ev_toddler},
    {"找到小虫",     "%NAME% 找到了一只小虫子！",     1, ev_bug}
};

/* ============ 入口 ============ */

static const char *const REQUIRES[] = { "Core:物品/鲜食" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "RandomEvents", "1.1.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
    return &info;
}

MOD_EXPORT int mod_init(const LoaderApi *api)
{
    if (!api) return fail("mod_init 收到空接口表");
    if (api->version != LOADER_API_VERSION)
        return fail("接口版本不符：模组按 v%u 编译，加载器是 v%u", (unsigned)MOD_API_VERSION, (unsigned)api->version);
    if (api->size < (uint32_t)sizeof(LoaderApi))
        return fail("LoaderApi 大小不足（%u < %u）：Defineds.h/Loader.h 与加载器不一致",
                    (unsigned)api->size, (unsigned)sizeof(LoaderApi));

    g_api = api;
    if (MOD_ADD(api, KIND_EVENT, EVENTS) == ID_NONE)
        return fail("KIND_EVENT 注册失败：内容表已满，或 Event_t 尺寸与加载器不一致");
    if (g_api->find(KIND_EVENT, "找到鲁尼") == ID_NONE)
        return fail("自检失败：注册后按名字查不到事件「找到鲁尼」");
    return 1;
}

MOD_EXPORT const char *mod_error(void)
{
    return g_err[0] ? g_err : "mod_init 失败（未提供原因）";
}

MOD_EXPORT void mod_exit(void)
{
    g_api = 0;
}
