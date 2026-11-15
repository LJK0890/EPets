/* ============================================================
 * EPets · 照护模组
 *   居家菜单：状态 / 背包 / 喂食 / 清洁 / 每日任务 / 睡觉·叫醒
 *   只注册 KIND_MENU，物品来自 Core、菜单调度来自加载器
 *
 * 编译: cl /LD /utf-8 /W4 /I..\mods\incl Care.c /Fe:Care.dll
 *
 * 菜单回调返回 1=已处理（继续），0=返回上一级，-1=请求退出游戏。
 * ============================================================ */

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const LoaderApi *g_api;
static char g_err[192];

/* mod_init 失败时留下可读原因，加载器会通过 mod_error() 取走 */
static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

static uint8_t add_stat(uint8_t v, int d)
{
    int r = (int)v + d;
    if (r < MIN_STAT) return (uint8_t)MIN_STAT;
    if (r > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)r;
}

static const char *pet_name(const Pet *pet)
{
    return (pet && pet->name) ? pet->name : "它";
}

static void pause_key(void)
{
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
}

/* ============ 状态 / 背包 ============ */

static int menu_status(Pet *pet)
{
    const PetType *type = (const PetType *)g_api->get(KIND_PET_TYPE, pet->type_id);

    g_api->ui_title("状态");
    g_api->ui_print("\n  %s（%s）Lv.%u\n", pet_name(pet),
                    type && type->name ? type->name : "?", (unsigned)pet->level);
    g_api->ui_print("  饱食:%3u  清洁:%3u  心情:%3u\n", pet->hunger, pet->clean, pet->happy);
    g_api->ui_print("  健康:%3u  活力:%3u  年龄:%u天\n", pet->health, pet->energy, (unsigned)pet->age);
    g_api->ui_print("  鲁尼:%u  经验:%u\n", pet->coins, pet->exp);
    g_api->ui_print("  性格: 魅力%u 聪明%u 勇敢%u\n", pet->personality[0], pet->personality[1], pet->personality[2]);

    if (pet->trait_cnt > 0)
    {
        uint64_t i;
        g_api->ui_print("  特质:");
        for (i = 0; i < pet->trait_cnt; i++)
        {
            const Trait_t *t = (const Trait_t *)g_api->get(KIND_TRAIT, pet->traits[i]);
            if (t) g_api->ui_print(" %s", t->name);
        }
        g_api->ui_print("\n");
    }
    if (pet->sick != D_NONE) g_api->ui_print("  状态: 生病了\n");
    if (pet->rebellious) g_api->ui_print("  状态: 叛逆期\n");
    if (pet->sleeping) g_api->ui_print("  状态: 睡着了\n");

    pause_key();
    return 1;
}

static int menu_bag(Pet *pet)
{
    uint64_t i;

    g_api->ui_title("背包");
    g_api->ui_print("\n");
    for (i = 0; i < pet->inv_cnt; i++)
    {
        const Item *it;
        if (pet->inv[i].id == ID_NONE || pet->inv[i].count == 0) continue;
        it = (const Item *)g_api->get(KIND_ITEM, pet->inv[i].id);
        g_api->ui_print("  %-10s x%llu\n", it ? it->name : "?",
                        (unsigned long long)pet->inv[i].count);
    }
    g_api->ui_print("\n  鲁尼: %u\n", pet->coins);
    pause_key();
    return 1;
}

/* ============ 喂食 / 清洁 ============ */

/* 按名字检查特质（特质 Id 是运行时分配的，名字才是跨模组/存档稳定的键） */
static int pet_has_trait(const Pet *pet, const char *name)
{
    uint64_t i;
    for (i = 0; i < pet->trait_cnt; i++)
    {
        const Trait_t *t;
        if (pet->traits[i] == ID_NONE) continue;
        t = (const Trait_t *)g_api->get(KIND_TRAIT, pet->traits[i]);
        if (t && t->name && strcmp(t->name, name) == 0) return 1;
    }
    return 0;
}

static int menu_feed(Pet *pet)
{
    Id id = g_api->inv_pick(pet, I_FOOD, "选择食物");
    const Item *it;
    int special, greedy, picky, greedy_fixed = 0;
    uint8_t happy_before;

    if (id == ID_NONE) return 1;
    it = (const Item *)g_api->get(KIND_ITEM, id);
    if (!it) return 1;
    if (!g_api->inv_rm(pet, id, 1)) { g_api->ui_warn("数量不足"); return 1; }

    /* 挑食/贪吃特判（可在配置页关掉） */
    special = g_api->config_get("care.feed_trait_special") != 0;
    greedy = special && pet_has_trait(pet, "贪吃");
    picky = special && pet_has_trait(pet, "挑食");
    happy_before = pet->happy;

    g_api->ui_title("喂食");
    g_api->ui_print("\n  %s 吃下了 %s\n", pet_name(pet), it->name);
    if (it->on_used) it->on_used(pet);

    /* 贪吃：食物带来的心情下降归零 */
    if (greedy && pet->happy < happy_before)
    {
        pet->happy = happy_before;
        greedy_fixed = 1;
    }
    /* 挑食：吃正餐额外掉 5 点心情 */
    if (picky && it->type == I_FOOD)
        pet->happy = add_stat(pet->happy, -5);

    if (greedy_fixed) g_api->ui_print("  （贪吃：没因为食物掉心情）\n");
    if (picky && it->type == I_FOOD) g_api->ui_print("  （挑食：心情 -5）\n");

    pet->task_feed++;
    if (pet->task_feed >= 3) pet->task_complete |= TASK_BIT_FEED;
    if (pet->hunger < 15) pet->hungry_streak++; else pet->hungry_streak = 0;
    pet->total_feed++;

    g_api->exp_add(pet, 5);
    g_api->ui_ok("  饱食:%u 心情:%u 健康:%u", pet->hunger, pet->happy, pet->health);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_clean(Pet *pet)
{
    Id id = g_api->inv_pick(pet, I_CLEANING, "选择清洁用品");
    const Item *it;

    if (id == ID_NONE) return 1;
    it = (const Item *)g_api->get(KIND_ITEM, id);
    if (!it) return 1;
    if (!g_api->inv_rm(pet, id, 1)) { g_api->ui_warn("数量不足"); return 1; }

    g_api->ui_title("清洁");
    g_api->ui_print("\n  用 %s 把 %s 洗得干干净净~\n", it->name, pet_name(pet));
    if (it->on_used) it->on_used(pet);

    g_api->ui_ok("  清洁:%u 心情:%u", pet->clean, pet->happy);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* ============ 每日任务 ============ */

static int menu_daily(Pet *pet)
{
    g_api->ui_title("每日任务");
    g_api->ui_print("\n  登录      %s  奖励 100\n", (pet->task_complete & TASK_BIT_LOGIN) ? "完成" : "未完成");
    g_api->ui_print("  喂食 3 次 %s  奖励 150\n", (pet->task_complete & TASK_BIT_FEED) ? "完成" : "未完成");
    g_api->ui_print("  玩耍 3 次 %s  奖励 150\n", (pet->task_complete & TASK_BIT_PLAY) ? "完成" : "未完成");

    if (!(pet->task_complete & TASK_BIT_LOGIN))
    {
        pet->task_complete |= TASK_BIT_LOGIN;
        pet->coins += 100;
        g_api->ui_ok("  完成每日登录 +100");
    }
    if (pet->task_feed >= 3 && !(pet->task_complete & TASK_BIT_FEED))
    {
        pet->task_complete |= TASK_BIT_FEED;
        pet->coins += 150;
        g_api->ui_ok("  完成喂食任务 +150");
    }
    if (pet->task_play >= 3 && !(pet->task_complete & TASK_BIT_PLAY))
    {
        pet->task_complete |= TASK_BIT_PLAY;
        pet->coins += 150;
        g_api->ui_ok("  完成玩耍任务 +150");
    }

    g_api->save(pet);
    pause_key();
    return 1;
}

/* ============ 睡觉 / 叫醒 ============ */

static int menu_sleep(Pet *pet)
{
    if (pet->sleeping)
    {
        pet->sleeping = 0;
        g_api->ui_ok("%s 醒了过来~", pet_name(pet));
    }
    else
    {
        pet->sleeping = 1;
        g_api->ui_ok("%s 进入了梦乡...", pet_name(pet));
    }
    pause_key();
    return 1;
}

/* ============ 注册 ============ */

static const Menu_t MENUS[] = {
    {"状态",      "状态",      0,  10, MENU_FLAG_NONE, menu_status},
    {"背包",      "背包",      0,  20, MENU_FLAG_NONE, menu_bag},
    {"喂食",      "喂食",      0,  30, MENU_FLAG_NONE, menu_feed},
    {"清洁",      "清洁",      0,  40, MENU_FLAG_NONE, menu_clean},
    {"每日任务",  "每日任务",  0,  80, MENU_FLAG_NONE, menu_daily},
    {"睡觉/叫醒", "睡觉/叫醒", 0, 110, MENU_FLAG_NONE, menu_sleep}
};

static const Config_t CONFIGS[] = {
    {"care.feed_trait_special", "喂食特质特判", "贪吃不扣心情、挑食额外 -5 心情", CFG_BOOL, 1, 0, 1}
};

static const char *const REQUIRES[] = {
    "Core:特质/贪吃",
    "Core:特质/挑食"
};

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Care", "1.0.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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

    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_CONFIG, CONFIGS) == ID_NONE)
        return fail("KIND_CONFIG 注册失败：配置表已满，或 Config_t 尺寸与加载器不一致");
    if (g_api->find(KIND_MENU, "喂食") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「喂食」");
    if (g_api->find(KIND_CONFIG, "care.feed_trait_special") == ID_NONE)
        return fail("自检失败：注册后按名字查不到配置「care.feed_trait_special」");
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
