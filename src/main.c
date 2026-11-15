/* ============================================================
 * EPets · 主程序
 *   静态链接 ModLoader.lib（运行时需要 ModLoader.dll 与 mods\*.dll）
 *
 *   epet.exe            正常游戏（宿主只做生命周期 + 菜单调度，菜单项在模组里）
 *   epet.exe --demo     无头自检：加载/依赖/剧情/事件/小游戏/存档/死亡全跑一遍
 *   epet.exe --auto     无头把每个菜单点一遍（冒烟）
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ModLoader.h"

extern int shell_run(Pet *pet);       /* shell.c */
extern int shell_self_test(Pet *pet); /* shell.c */

/* 价格表：重生时按这几项收费（祭坛那份在 Activities 模组里） */
#define REBIRTH_SPECIES_COST 1000u /* 自选类型与性别 */
#define REBIRTH_NAME_COST    1000u /* 选取名字 */
#define TRAIT_RANDOM_COST     500u /* 第 3 个起的随机特质 */
#define TRAIT_PICK_COST      1000u /* 手选特质 */
#define TRAIT_DELETE_COST    2000u /* 删除特质（重生不给删，祭坛用） */

/* ============ 新宠物 ============ */

/* 随机类型 / 性别：首次进入游戏与“随机重生”共用 */
static Id random_pet_type(void)
{
    uint64_t n = modloader_count(KIND_PET_TYPE);
    return n ? (Id)ml_api()->rnd64(1, n) : ID_NONE;
}

static Gender_t random_gender(void)
{
    return (Gender_t)ml_api()->rnd((uint32_t)G_MALE, (uint32_t)G_NEUTER);
}

/* 建号：type 无效时退回第 1 种；name 为 NULL 时摇一个随机名字 */
static void new_game(Pet *pet, Id type_id, Gender_t gender, const char *name,
                     uint32_t coins, int play_adopt)
{
    memset(pet, 0, sizeof(*pet));
    if (type_id == ID_NONE && modloader_count(KIND_PET_TYPE) > 0) type_id = (Id)1;
    pet->type_id = type_id;
    pet->gender = gender;
    pet->name = name;
    if (!pet->name)
    {
        pet->name = ml_api()->rand_name(gender, NULL);
        if (!pet->name) pet->name = modloader_name_at(gender, 0);
        if (!pet->name) pet->name = "新伴兽";
    }

    pet->hunger = 70;
    pet->clean = 80;
    pet->happy = 80;
    pet->health = 100;
    pet->energy = 100;
    pet->level = 1;
    pet->coins = coins;
    pet->login_days = 1;

    /* 2 个免费随机特质：1 正 1 负且不互斥 */
    (void)ml_api()->gen_traits(pet);

    /* 新手礼包 */
    ml_inv_add(pet, modloader_find(KIND_ITEM, "粗粮"), 10);
    ml_inv_add(pet, modloader_find(KIND_ITEM, "伤药"), 2);
    ml_inv_add(pet, modloader_find(KIND_ITEM, "浴布"), 3);

    /* 有逝去记录就继承“失去过伴兽” */
    modloader_pet_inherit(pet);
    ml_api()->ach_merge_global(pet);

    /* 首次领养剧情；死亡后的重生不再重复整个领养流程 */
    if (play_adopt) (void)modloader_story_play(pet, "adopt", "向导");
}

/* 首次进入游戏：类型 / 性别 / 名字 / 2 个特质全部随机，不收钱 */
static void new_first_game(Pet *pet)
{
    new_game(pet, random_pet_type(), random_gender(), NULL, 1000, 1);
}

/* ============ 死亡后的新宠物选择 ============ */

static int choose_pet_type(Id *out)
{
    uint64_t n = modloader_count(KIND_PET_TYPE), i;
    SelectOpt_t *opts;
    int sel;

    if (n == 0 || n > (uint64_t)(SIZE_MAX / sizeof(*opts))) return 0;
    opts = (SelectOpt_t *)calloc((size_t)n, sizeof(*opts));
    if (!opts) return 0;

    for (i = 1; i <= n; i++)
    {
        const PetType *t = (const PetType *)modloader_get(KIND_PET_TYPE, (Id)i);
        ml_api()->opt_text(&opts[i - 1], "%s", (t && t->name) ? t->name : "?");
        opts[i - 1].value = (int)i;
    }

    sel = ml_api()->ui_menu("选择宠物种类", opts, n);
    free(opts); /* 文本由 ui_menu 释放 */
    if (sel <= 0) return 0;
    *out = (Id)sel;
    return 1;
}

static int choose_gender(Gender_t *out)
{
    SelectOpt_t opts[3];
    int sel;

    memset(opts, 0, sizeof(opts));
    ml_api()->opt_text(&opts[0], "男"); opts[0].value = G_MALE;
    ml_api()->opt_text(&opts[1], "女"); opts[1].value = G_FEMALE;
    ml_api()->opt_text(&opts[2], "中性"); opts[2].value = G_NEUTER;

    sel = ml_api()->ui_menu("选择性别", opts, 3);
    if (sel < 0) return 0;
    *out = (Gender_t)sel;
    return 1;
}

/* 类型与性别：随机免费 / 自选 1000。返回 1=选好，0=玩家放弃整段重生 */
static int rebirth_pick_species(Id *type_id, Gender_t *gender, uint32_t *coins)
{
    static const char *const PAGES[] = {
        "§O桥屋向导§0：§S又见面了。上一只伴兽已经不在了。§0",
        "§O桥屋向导§0：§S先定下种类和性别吧。§0"
    };
    static const StoryOpt_t OPTS[] = {
        {"随机（免费）", 1},
        {"自选（1000 鲁尼）", 2},
        {"暂时不了", 0}
    };

    for (;;)
    {
        int choice = ml_dialog("新的伴兽", PAGES, 2, OPTS, 3);

        if (choice == 0) return 0;
        if (choice == 1)
        {
            *type_id = random_pet_type();
            *gender = random_gender();
            return 1;
        }
        if (*coins < REBIRTH_SPECIES_COST)
        {
            ml_api()->ui_err("  鲁尼不够：自选需要 %u 鲁尼", REBIRTH_SPECIES_COST);
            ml_key();
            continue;
        }
        if (!choose_pet_type(type_id)) continue;
        if (!choose_gender(gender)) continue;
        *coins -= REBIRTH_SPECIES_COST;
        return 1;
    }
}

/* 名字：随机免费 / 选取 1000。返回 1=选好，0=退出，-1=返回上一步 */
static int rebirth_pick_name(Gender_t gender, const char **name, uint32_t *coins)
{
    static const char *const PAGES[] = {
        "§O桥屋向导§0：§S再给它取个名字吧。§0"
    };
    static const StoryOpt_t OPTS[] = {
        {"随机名字（免费）", 1},
        {"选取名字（1000 鲁尼）", 2},
        {"返回上一步", 0}
    };

    for (;;)
    {
        int choice = ml_dialog("新的伴兽", PAGES, 1, OPTS, 3);

        if (choice == 0) return -1;
        if (choice == 1)
        {
            *name = ml_api()->rand_name(gender, NULL);
            return 1;
        }
        if (*coins < REBIRTH_NAME_COST)
        {
            ml_api()->ui_err("  鲁尼不够：选取名字需要 %u 鲁尼", REBIRTH_NAME_COST);
            ml_key();
            continue;
        }

        {
            const char *picked = modloader_name_pick(gender, "选择名字");
            if (!picked) continue; /* 取消选择：留在这一步 */
            *name = picked;
            *coins -= REBIRTH_NAME_COST;
            return 1;
        }
    }
}

/* 返回 1=已经创建新宠物，0=玩家放弃（主程序退出） */
static int rebirth(Pet *pet)
{
    uint32_t coins;
    Id type_id = ID_NONE;
    Gender_t gender = G_MALE;
    const char *name = NULL;

    if (!pet) return 0;
    coins = pet->coins;

    if (modloader_is_headless())
    {
        ml_ui_print("\n[重生] 随机宠物，继承 %u 鲁尼\n", coins);
        modloader_pet_release(pet);
        new_game(pet, random_pet_type(), random_gender(), NULL, coins, 0);
        return 1;
    }

    /* 整段向导：名字那步选“返回上一步”就重新来（钱按 pet->coins 重算） */
    for (;;)
    {
        int r;

        coins = pet->coins;
        type_id = ID_NONE;
        gender = G_MALE;
        name = NULL;

        if (!rebirth_pick_species(&type_id, &gender, &coins)) return 0;

        r = rebirth_pick_name(gender, &name, &coins);
        if (r == 0) return 0;
        if (r < 0) continue;
        break;
    }

    modloader_pet_release(pet);
    new_game(pet, type_id, gender, name, coins, 0);

    /* 2 个免费随机特质已由 new_game 摇好；想多要就继续买（随机 500 / 手选 1000） */
    (void)modloader_trait_shop(pet, "特质定制", 0,
                               TRAIT_RANDOM_COST, TRAIT_PICK_COST, TRAIT_DELETE_COST);
    return 1;
}

/* ============ 无头自检 ============ */

static void demo(void)
{
    Pet pet;
    Id *games;
    uint64_t gm, i, cap;
    uint64_t n;

    modloader_set_headless(1);
    n = modloader_init("mods");
    if (n == UINT64_MAX || n == 0) { printf("没有加载到模组（详见上面的模组错误列表）\n"); return; }
    if (modloader_count(KIND_PET_TYPE) == 0)
    {
        printf("核心内容缺失：Core 模组没有加载成功，无法开始游戏（详见上面的模组错误列表）\n");
        return;
    }

    ml_ui_print("加载模组 %llu 个:\n", (unsigned long long)n);
    for (i = 0; i < n; i++) ml_ui_print("  - %s\n", modloader_mod_name(i));
    ml_ui_print("内容: 特质=%llu 物品=%llu 宠物=%llu 成就=%llu 剧情=%llu 事件=%llu 小游戏=%llu 术语=%llu NPC=%llu 菜单=%llu 配置=%llu\n",
                (unsigned long long)modloader_count(KIND_TRAIT),
                (unsigned long long)modloader_count(KIND_ITEM),
                (unsigned long long)modloader_count(KIND_PET_TYPE),
                (unsigned long long)modloader_count(KIND_ACHIEVEMENT),
                (unsigned long long)modloader_count(KIND_STORY),
                (unsigned long long)modloader_count(KIND_EVENT),
                (unsigned long long)modloader_count(KIND_MINIGAME),
                (unsigned long long)modloader_count(KIND_TERM),
                (unsigned long long)modloader_count(KIND_NPC),
                (unsigned long long)modloader_count(KIND_MENU),
                (unsigned long long)modloader_count(KIND_CONFIG));
    if (modloader_error_count() > 0)
        ml_ui_print("模组问题 %llu 条（依赖/版本/重名，详见上面的列表）\n",
                    (unsigned long long)modloader_error_count());

    new_game(&pet, modloader_find(KIND_PET_TYPE, "暗影猫"), G_MALE, NULL, 1000, 1);
    ml_ui_print("\n宠物: %s（%s）Lv.%u 鲁尼%u 特质:%llu\n", pet.name,
                modloader_name_of(KIND_PET_TYPE, pet.type_id), (unsigned)pet.level,
                pet.coins, (unsigned long long)pet.trait_cnt);

    pet.level = 5;
    {
        int fresh = modloader_ach_poll(&pet);
        uint64_t done = 0, k;
        for (k = 1; k <= modloader_count(KIND_ACHIEVEMENT); k++)
            if (ml_api()->ach_unlocked(&pet, (Id)k)) done++;
        ml_ui_print("成就轮询: 新解锁 %d 个，累计 %llu/%llu\n", fresh,
                    (unsigned long long)done,
                    (unsigned long long)modloader_count(KIND_ACHIEVEMENT));
    }

    {
        int played = modloader_story_play(&pet, "mall", NULL);
        uint64_t seen = 0, k;
        for (k = 1; k <= modloader_count(KIND_STORY); k++)
            if (ml_api()->story_seen(&pet, (Id)k)) seen++;
        ml_ui_print("剧情播放(mall): %d，累计已播 %llu/%llu\n", played,
                    (unsigned long long)seen,
                    (unsigned long long)modloader_count(KIND_STORY));
    }

    ml_ui_print("随机事件: %d\n", modloader_event_roll(&pet));

    cap = modloader_count(KIND_MINIGAME);
    games = (Id *)malloc((size_t)(cap ? cap : 1) * sizeof(*games));
    gm = games ? modloader_minigame_list(pet.type_id, games, cap) : 0;
    ml_ui_print("这只宠物能玩的小游戏 %llu 个:\n", (unsigned long long)gm);
    for (i = 0; i < gm; i++) ml_ui_print("  - %s\n", modloader_name_of(KIND_MINIGAME, games[i]));
    free(games);

    /* 存档往返 */
    ml_inv_add(&pet, modloader_find(KIND_ITEM, "粗粮"), 3);
    ml_ui_print("\n存档: %d\n", modloader_save(&pet));
    ml_ui_print("背包粗粮: %llu\n",
                (unsigned long long)ml_inv_count(&pet, modloader_find(KIND_ITEM, "粗粮")));

    /* 死亡流程 */
    {
        int days = 0, died = 0;
        pet.age = 34; pet.hunger = 8; pet.clean = 8; pet.health = 6;
        while (!died && days < 60) { died = modloader_pet_day(&pet); days++; }
        ml_ui_print("\n经过 %d 天，死亡=%d，死因=%s\n", days, died,
                    pet.death_reason ? pet.death_reason : "-");
        ml_ui_print("逝去记录 %u 条：\n", modloader_dead_count());
        modloader_dead_menu();

        {
            Pet next;
            memset(&next, 0, sizeof(next));
            next.name = modloader_name_at(G_FEMALE, 1);
            modloader_pet_inherit(&next);
            ml_ui_print("新宠物 lost_friend=%d（有逝去记录应为 1）\n", next.lost_friend);
            modloader_pet_release(&next);
        }
    }

    modloader_pet_release(&pet);
    modloader_shutdown();
}

/* ============ 入口 ============ */

int main(int argc, char **argv)
{
    Pet pet;
    int auto_mode;

    if (argc > 1 && strcmp(argv[1], "--demo") == 0) { demo(); return 0; }

    /* 自动化冒烟：无头模式把每个菜单点一遍 */
    auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);
    if (auto_mode) modloader_set_headless(1);

    {
        uint64_t loaded = modloader_init("mods");
        if (loaded == UINT64_MAX || loaded == 0)
        {
            printf("没有加载到模组（mods\\*.dll）；若 mods 目录存在，请看上面的模组错误列表。\n");
            return 1;
        }
    }

    if (modloader_count(KIND_PET_TYPE) == 0)
    {
        printf("\n[错误] 基础模组 Core 没有加载成功：缺少宠物类型/物品/名字，无法开始游戏。\n");
        printf("       检查 mods\\Core.dll 是否存在、是否用同一份 Loader.h 编译（接口 v%u）。\n",
               (unsigned)LOADER_API_VERSION);
        return 1;
    }

    /* 非致命问题（个别模组没加载）也先停一下，否则第一屏清屏就把诊断冲掉了 */
    if (modloader_error_count() > 0 && !modloader_is_headless())
    {
        printf("\n上面是模组加载诊断。Z:继续\n");
        fflush(stdout);
        ml_key();
    }

    memset(&pet, 0, sizeof(pet));
    if (!modloader_load(&pet))
    {
        new_first_game(&pet); /* 首次进入：类型 / 性别 / 名字 / 2 个特质全随机 */
        modloader_save(&pet);
    }

    /* 上次退出时宠物已经死亡：先处理逝去记录，再选择新宠物 */
    if (pet.dead)
    {
        ml_ui_print("\n上一只伴兽已经不在了。\n");
        modloader_dead_menu();
        if (!rebirth(&pet))
        {
            modloader_pet_release(&pet);
            modloader_shutdown();
            return 0;
        }
        modloader_save(&pet);
    }

    if (auto_mode)
    {
        shell_self_test(&pet);
    }
    else
    {
        for (;;)
        {
            (void)shell_run(&pet);
            if (!pet.dead) break;          /* 正常退出菜单 */
            if (!rebirth(&pet)) break;     /* 玩家放弃重生 */
            modloader_save(&pet);
        }
    }

    modloader_pet_release(&pet);
    modloader_shutdown();
    return 0;
}
