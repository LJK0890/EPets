/* ============================================================
 * EPets · GameShell
 *   宿主骨架：生命周期 + 菜单调度 + 系统菜单（存档 / 读档 / 配置 / 退出）
 *
 * 具体菜单项（状态/背包/喂食/清洁/玩耍/出门/桥屋/记事册/社交/教程/调试…）
 * 全部由模组用 KIND_MENU 注册；本文件只调 modloader_menu_run()。
 * 界面内容零硬编码，主程序只保留“游戏怎么跑”这件事。
 * ============================================================ */

#define _CRT_SECURE_NO_WARNINGS 1

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ModLoader.h"

/* ============ 小工具 ============ */

static const char *pet_name(const Pet *pet)
{
    return (pet && pet->name) ? pet->name : "它";
}

static void pause_key(void)
{
    ml_ui_print("\nZ:继续\n");
    ml_key();
}

/* ============ 系统菜单（宿主自带，不依赖任何模组） ============ */

static int sys_save(Pet *pet)
{
    if (ml_api()->save(pet)) ml_api()->ui_ok("已存档");
    else ml_api()->ui_err("存档失败");
    pause_key();
    return 1;
}

static int sys_load(Pet *pet)
{
    if (ml_api()->load(pet)) ml_api()->ui_ok("已读档");
    else ml_api()->ui_warn("没有存档");
    pause_key();
    return 1;
}

/* ============ 配置页 ============ */

static void config_value_text(const Config_t *c, char *out, size_t cap)
{
    int32_t v = ml_api()->config_get(c->name);
    if (c->type == CFG_BOOL) _snprintf(out, cap, "%s", v ? "开" : "关");
    else if (c->type == CFG_SLIDER) _snprintf(out, cap, "< %d >", (int)v);
    else _snprintf(out, cap, "%d", (int)v);
    out[cap - 1] = '\0';
}

/* 滑动条：< value >，←/→ 调整，Z 确认，X 取消 */
static int edit_slider(const Config_t *c)
{
    int32_t cur = ml_api()->config_get(c->name);
    int32_t lo = c->min, hi = c->max;
    int k;

    if (hi < lo) return 1;
    if (cur < lo) cur = lo;
    if (cur > hi) cur = hi;

    for (;;)
    {
        ml_ui_title(c->label ? c->label : c->name);
        ml_ui_print("\n  < %d >\n\n  ←/→:调整   Z:确认   X:取消\n", (int)cur);
        k = ml_key();
        if (k == KEY_LEFT)
        {
            if (cur > lo) cur--;
        }
        else if (k == KEY_RIGHT)
        {
            if (cur < hi) cur++;
        }
        else if (k == KEY_OK)
        {
            if (ml_api()->config_set(c->name, cur))
                ml_api()->ui_ok("  %s: %d", c->label ? c->label : c->name, (int)cur);
            else
                ml_api()->ui_err("  设置失败");
            return 1;
        }
        else if (k == KEY_CANCEL) return 1;
    }
}

static int sys_config(Pet *pet)
{
    (void)pet;

    /* 无头自检只读，避免 --auto 改掉用户配置 */
    if (ml_api()->is_headless())
    {
        uint64_t i, n = modloader_count(KIND_CONFIG);
        ml_ui_print("\n（自检：配置页只读，%llu 项）\n", (unsigned long long)n);
        for (i = 1; i <= n; i++)
        {
            const Config_t *c = (const Config_t *)modloader_get(KIND_CONFIG, (Id)i);
            char val[32];
            if (!c) continue;
            config_value_text(c, val, sizeof(val));
            ml_ui_print("  %s = %s\n", c->label ? c->label : c->name, val);
        }
        return 1;
    }

    for (;;)
    {
        uint64_t i, total = modloader_count(KIND_CONFIG), n = 0;
        SelectOpt_t *opts;
        int sel;

        if (total == 0 || total == UINT64_MAX)
        {
            ml_ui_print("\n暂无可配置项\n");
            pause_key();
            return 1;
        }

        opts = (SelectOpt_t *)calloc((size_t)(total + 1), sizeof(*opts));
        if (!opts) return 1;

        for (i = 1; i <= total; i++)
        {
            const Config_t *c = (const Config_t *)modloader_get(KIND_CONFIG, (Id)i);
            char val[32];
            if (!c) continue;
            config_value_text(c, val, sizeof(val));
            ml_api()->opt_text(&opts[n], "%s: %s", c->label ? c->label : c->name, val);
            opts[n].value = (int)i;
            n++;
        }
        if (n == 0)
        {
            free(opts);
            ml_ui_print("\n暂无可配置项\n");
            pause_key();
            return 1;
        }
        ml_api()->opt_text(&opts[n], "返回");
        opts[n].value = 0;

        sel = ml_api()->ui_menu("配置", opts, n + 1);
        free(opts); /* 文本已被 ui_menu 释放 */
        if (sel <= 0) return 1;

        {
            const Config_t *c = (const Config_t *)modloader_get(KIND_CONFIG, (Id)sel);
            if (!c) continue;

            if (c->type == CFG_BOOL)
            {
                int32_t nv = ml_api()->config_get(c->name) ? 0 : 1;
                if (ml_api()->config_set(c->name, nv))
                    ml_api()->ui_ok("  %s: %s", c->label ? c->label : c->name, nv ? "开" : "关");
                else
                    ml_api()->ui_err("  设置失败");
            }
            else if (c->type == CFG_SLIDER)
            {
                (void)edit_slider(c);
            }
            else
            {
                int32_t lo = c->min, hi = c->max;
                uint64_t vn = 0, v;
                SelectOpt_t *vals;
                int r;

                if (hi < lo) continue;
                if ((uint64_t)(hi - lo) > 255u)
                {
                    (void)edit_slider(c);
                    continue;
                }

                vals = (SelectOpt_t *)calloc((size_t)(hi - lo + 2), sizeof(*vals));
                if (!vals) continue;

                for (v = (uint64_t)lo; v <= (uint64_t)hi; v++)
                {
                    ml_api()->opt_text(&vals[vn], "%d%s", (int)v,
                                       ((int32_t)v == ml_api()->config_get(c->name)) ? "（当前）" : "");
                    vals[vn].value = (int)v;
                    vn++;
                }
                ml_api()->opt_text(&vals[vn], "返回");
                vals[vn].value = INT_MIN;

                r = ml_api()->ui_menu(c->label ? c->label : c->name, vals, vn + 1);
                free(vals);
                if (r == INT_MIN || r < lo || r > hi) continue;
                if (ml_api()->config_set(c->name, r))
                    ml_api()->ui_ok("  %s: %d", c->label ? c->label : c->name, r);
                else
                    ml_api()->ui_err("  设置失败");
            }
        }
    }
}

/* 配置是宿主自带的：其中 ui.page_size 是滑动条，决定所有菜单每页几项 */
static const Config_t SYSTEM_CONFIGS[] = {
    {"ui.page_size", "每页菜单项数", "主菜单/子菜单每页显示的行数（1~16）", CFG_SLIDER, 4, 1, 16}
};

static const Menu_t SYSTEM_MENUS[] = {
    {"存档", "存档", 0, 900, MENU_FLAG_NONE, sys_save},
    {"读档", "读档", 0, 910, MENU_FLAG_NONE, sys_load},
    {"配置", "配置", 0, 920, MENU_FLAG_NONE, sys_config},
    {"退出", "退出", 0, 999, MENU_FLAG_EXIT, 0}
};

static int g_system_menus_ready;

/* 把系统菜单登记进 KIND_MENU；重复调用安全 */
static int shell_register_system_menus(void)
{
    if (g_system_menus_ready) return 1;

    if (ml_api()->add(KIND_CONFIG, SYSTEM_CONFIGS,
                      (uint64_t)(sizeof(SYSTEM_CONFIGS) / sizeof(SYSTEM_CONFIGS[0])),
                      (uint32_t)sizeof(SYSTEM_CONFIGS[0])) == ID_NONE)
        ml_ui_print("\n警告：界面配置注册失败（配置表已满）\n");

    if (ml_api()->add(KIND_MENU, SYSTEM_MENUS,
                      (uint32_t)(sizeof(SYSTEM_MENUS) / sizeof(SYSTEM_MENUS[0])),
                      (uint32_t)sizeof(SYSTEM_MENUS[0])) == ID_NONE)
        return 0;

    g_system_menus_ready = 1;
    return 1;
}

/* ============ 自检：把每个菜单都点一遍（无头模式用） ============ */

int shell_self_test(Pet *pet)
{
    uint64_t i, n;

    if (!shell_register_system_menus())
    {
        ml_ui_print("\n菜单注册失败：KIND_MENU 内容表已满\n");
        return 0;
    }

    n = modloader_count(KIND_MENU);

    /* 动作型菜单：逐个执行，保证每个模组的菜单回调都被跑到 */
    for (i = 1; i <= n; i++)
    {
        const Menu_t *m = (const Menu_t *)modloader_get(KIND_MENU, (Id)i);
        if (m && m->run && !(m->flags & MENU_FLAG_EXIT)) (void)m->run(pet);
    }

    /* 容器型菜单：无头下 menu_run 只执行第一项，把每个容器点开一次 */
    for (i = 1; i <= n; i++)
    {
        const Menu_t *m = (const Menu_t *)modloader_get(KIND_MENU, (Id)i);
        if (m && !m->run && !(m->flags & MENU_FLAG_EXIT)) (void)modloader_menu_run(pet, m->name);
    }
    return 1;
}

/* ============ 主循环 ============ */

int shell_run(Pet *pet)
{
    uint32_t last_day = ml_api()->rtc_day();
    int steps = 0;

    if (!shell_register_system_menus())
    {
        ml_ui_print("\n菜单注册失败：KIND_MENU 内容表已满\n");
        return 0;
    }

    while (1)
    {
        uint32_t day = ml_api()->rtc_day();
        int r;

        /* 跨天：日常结算 + 随机事件 */
        if (day != last_day)
        {
            last_day = day;
            if (modloader_pet_day(pet)) return 0; /* 离世 */
            if (ml_api()->rnd(0, 99) < 25) (void)modloader_event_roll(pet);
            (void)modloader_ach_poll(pet);
            modloader_save(pet);
        }

        if (pet->dead) return 0;

        ml_ui_title("EPet");
        ml_ui_print("\n  %s  Lv.%u   鲁尼:%u\n", pet_name(pet), (unsigned)pet->level, pet->coins);
        ml_ui_print("  饱食:%u 清洁:%u 心情:%u 健康:%u\n", pet->hunger, pet->clean, pet->happy, pet->health);

        r = modloader_menu_run(pet, NULL);
        if (r < 0) break; /* “退出”菜单项 */

        /* 无头模式防死循环：跑够步数就退出 */
        if (++steps > 2000) break;
    }

    modloader_save(pet);
    return 1;
}
