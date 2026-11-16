/* ============================================================
 * EPets · 世界模组
 *   出门菜单：商场 / 医院 / 公园 / 游乐园（4 个项目）/ 出远门
 *   只注册 KIND_MENU；商品与效果来自 Core，场景剧情来自 Story
 *
 * 编译: cl /LD /utf-8 /W4 /I..\mods\incl World.c /Fe:World.dll
 * ============================================================ */

#define _CRT_SECURE_NO_WARNINGS 1

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const LoaderApi *g_api;
static char g_err[192];

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

/* 反复进入一个子菜单，直到玩家选“返回” */
static int run_sub(Pet *pet, const char *name)
{
    int r;
    while ((r = g_api->menu_run(pet, name)) == 1) { }
    return (r < 0) ? -1 : 1;
}

/* ============ 商场 ============ */

static int menu_shop(Pet *pet)
{
    SelectOpt_t *opts;
    Id *ids;
    uint64_t total, n = 0, i;
    Id picked;
    int sel;

    (void)g_api->story_play(pet, "mall", NULL);

    total = g_api->count(KIND_ITEM);
    if (total == 0)
    {
        g_api->ui_print("\n货架是空的\n");
        pause_key();
        return 0;
    }

    opts = (SelectOpt_t *)calloc((size_t)(total + 1), sizeof(*opts));
    ids = (Id *)malloc((size_t)total * sizeof(*ids));
    if (!opts || !ids)
    {
        free(opts);
        free(ids);
        g_api->ui_print("\n商品列表分配失败\n");
        return 0;
    }

    for (i = 1; i <= total; i++)
    {
        const Item *it = (const Item *)g_api->get(KIND_ITEM, (Id)i);
        if (!it || it->price == 0) continue;                      /* 纪念品不卖 */
        if (it->type == I_DECORATION && it->price > 300) continue; /* 贵装饰留给成就 */
        g_api->opt_text(&opts[n], "%s %u", it->name, (unsigned)it->price);
        opts[n].value = (int)n;
        ids[n] = (Id)i;
        n++;
    }
    if (n == 0)
    {
        free(opts);
        free(ids);
        g_api->ui_print("\n货架是空的\n");
        pause_key();
        return 0;
    }

    /* 购买页（MENU_FLAG_STAY）：买完留在这一页，按 X 或选“返回”才回“出门” */
    g_api->opt_text(&opts[n], "返回（鲁尼 %u）", pet->coins);
    opts[n].value = -1;

    sel = g_api->ui_menu("商场", opts, n + 1);
    free(opts);
    if (sel < 0 || (uint64_t)sel >= n)
    {
        free(ids);
        return 0; /* 离开商场页面 */
    }
    picked = ids[sel];
    free(ids);

    {
        const Item *it = (const Item *)g_api->get(KIND_ITEM, picked);
        if (!it) return 0;
        if (pet->coins < it->price)
        {
            g_api->ui_err("  鲁尼不够：还差 %u", it->price - pet->coins);
            pause_key();
            return 1; /* 留在本页，可以换一样买 */
        }
        pet->coins -= it->price;
        g_api->inv_add(pet, picked, 1);
        if (it->price > pet->big_spend) pet->big_spend = it->price;
        g_api->ui_ok("  买下了 %s（-%u），剩余 %u 鲁尼", it->name, it->price, pet->coins);
        g_api->ach_poll(pet);
        g_api->save(pet);
        pause_key();
    }
    return 1; /* 留在商场页面 */
}

/* ============ 医院 ============ */

static int menu_hospital(Pet *pet)
{
    SelectOpt_t opts[2];
    int sel;

    memset(opts, 0, sizeof(opts));
    g_api->opt_text(&opts[0], "治疗（200 鲁尼）"); opts[0].value = 1;
    g_api->opt_text(&opts[1], "离开（健康 %u · 鲁尼 %u）", pet->health, pet->coins);
    opts[1].value = 0;

    (void)g_api->story_play(pet, "hospital", NULL);

    if (pet->coins < 200)
    {
        g_api->ui_title("医院");
        g_api->ui_print("\n  治疗一次 200 鲁尼\n");
        g_api->ui_print("  当前：健康 %u%s\n", pet->health,
                        pet->sick != D_NONE ? "（生病）" : "");
        g_api->ui_err("  鲁尼不够");
        pause_key();
        return 0;
    }

    /* 购买页（MENU_FLAG_STAY）：治完留在这一页，选“离开”才回“出门” */
    sel = g_api->ui_menu("要治吗？", opts, 2);
    if (sel != 1) return 0;

    pet->coins -= 200;
    pet->sick = D_NONE;
    pet->health = add_stat(pet->health, 40);
    g_api->ui_ok("  治好了，健康 %u，剩余 %u 鲁尼", pet->health, pet->coins);
    g_api->save(pet);
    pause_key();
    return 1; /* 留在医院页面 */
}

/* ============ 公园 ============ */

static int menu_park(Pet *pet)
{
    (void)g_api->story_play(pet, "park", NULL);

    g_api->ui_title("公园");
    g_api->ui_print("\n  %s 在公园玩得很开心！\n", pet_name(pet));
    pet->happy = add_stat(pet->happy, 20);
    pet->hunger = add_stat(pet->hunger, -8);
    pet->clean = add_stat(pet->clean, -5);

    if (g_api->rnd(0, 99) < 20)
    {
        uint32_t found = g_api->rnd(5, 20);
        pet->coins += found;
        g_api->ui_print("\n  %s 在草丛里发现了 %u 鲁尼！\n", pet_name(pet), found);
    }

    g_api->exp_add(pet, 6);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* ============ 游乐园 ============ */

static int ride(Pet *pet, int price)
{
    if (pet->coins < (uint32_t)price)
    {
        g_api->ui_err("  鲁尼不足！");
        pause_key();
        return 1;
    }
    if ((uint32_t)price > pet->big_spend) pet->big_spend = (uint32_t)price;
    pet->coins -= (uint32_t)price;
    pet->happy = add_stat(pet->happy, (int)g_api->rnd(30, 50));
    pet->hunger = add_stat(pet->hunger, -(int)g_api->rnd(5, 10));
    g_api->ui_ok("  %s 玩得超开心！", pet_name(pet));
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int ride_carousel(Pet *pet) { return ride(pet, 40); }
static int ride_coaster(Pet *pet)  { return ride(pet, 70); }
static int ride_ferris(Pet *pet)   { return ride(pet, 60); }

static int ride_closed(Pet *pet)
{
    (void)pet;
    g_api->ui_print("\n  这个项目还在建设中~\n  之后再来看看吧！\n");
    pause_key();
    return 1;
}

static int menu_amuse(Pet *pet)
{
    (void)g_api->story_play(pet, "amuse", NULL);

    if (pet->level < 12)
    {
        g_api->ui_title("？？？");
        g_api->ui_print("\n  这里好像是一片工地\n  之后再来看看吧\n");
        pause_key();
        return 1;
    }
    return run_sub(pet, "游乐园");
}

/* ============ 出远门 ============ */

static int menu_travel(Pet *pet)
{
    uint64_t total, i;
    int n;

    if (pet->level < 10)
    {
        g_api->ui_warn("  %s 还太小，不能出远门", pet_name(pet));
        pause_key();
        return 1;
    }
    if (pet->coins < 100)
    {
        g_api->ui_err("  出门至少要 100 鲁尼");
        pause_key();
        return 1;
    }

    (void)g_api->story_play(pet, "travel", NULL);

    pet->coins -= 100;
    pet->happy = add_stat(pet->happy, 50);
    pet->hunger = add_stat(pet->hunger, -20);
    pet->clean = add_stat(pet->clean, -15);
    pet->energy = add_stat(pet->energy, -25);

    g_api->ui_title("旅游归来");
    g_api->ui_print("\n  %s 玩得很开心！\n", pet_name(pet));
    g_api->ui_print("  心情+50 饱食-20 清洁-15 活力-25\n");

    n = (int)g_api->rnd(1, 3);
    total = g_api->count(KIND_ITEM);
    g_api->ui_print("\n  带回：\n");
    for (i = 0; i < (uint64_t)n; i++)
    {
        Id pick = (Id)g_api->rnd64(1, total);
        const Item *it = (const Item *)g_api->get(KIND_ITEM, pick);
        if (!it) continue;
        g_api->inv_add(pet, pick, 1);
        g_api->ui_print("    %s\n", it->name);
    }

    g_api->exp_add(pet, 20);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* ============ 注册 ============ */

static const Menu_t MENUS[] = {
    {"出门",     "出门",         0,       60, MENU_FLAG_NONE, 0},
    {"商场",     "商场",         "出门",  10, MENU_FLAG_STAY, menu_shop},
    {"医院",     "医院",         "出门",  20, MENU_FLAG_STAY, menu_hospital},
    {"公园",     "公园",         "出门",  30, MENU_FLAG_NONE, menu_park},
    {"游乐园",   "游乐园",       "出门",  40, MENU_FLAG_NONE, menu_amuse},
    {"出远门",   "出远门",       "出门",  50, MENU_FLAG_NONE, menu_travel},
    {"旋转木马", "旋转木马 40",  "游乐园", 10, MENU_FLAG_NONE, ride_carousel},
    {"过山车",   "过山车 70",    "游乐园", 20, MENU_FLAG_NONE, ride_coaster},
    {"摩天轮",   "摩天轮 60",    "游乐园", 30, MENU_FLAG_NONE, ride_ferris},
    {"鬼屋",     "鬼屋（未开）", "游乐园", 40, MENU_FLAG_NONE, ride_closed},
    {"碰碰车",   "碰碰车（未开）", "游乐园", 50, MENU_FLAG_NONE, ride_closed}
};

static const char *const REQUIRES[] = { "Core:物品/粗粮", "Story:剧情/首次商场" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "World", "1.0.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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
    if (g_api->find(KIND_MENU, "商场") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「商场」");
    if (g_api->find(KIND_MENU, "游乐园") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「游乐园」");
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
