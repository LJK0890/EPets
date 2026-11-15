#ifndef MODLOADER_H
#define MODLOADER_H

#include <stdint.h>
#include "Loader.h"

/**
 * @file ModLoader.h
 * @brief 加载框架对外的 API（实现编译成 ModLoader.dll，主程序链接 ModLoader.lib）
 *
 * 主程序只需要这一组函数：
 *   modloader_init("mods") → 扫描目录、按依赖顺序加载模组、注册内容
 *   然后按需调用内容驱动：菜单调度、轮询成就、播剧情、抽事件、跑小游戏。
 *
 * 内容与界面全部由模组提供，主程序不含任何具体宠物/物品/剧情/菜单项。
 */

#if defined(MODLOADER_BUILD)
#  define ML_API __declspec(dllexport)
#else
#  define ML_API __declspec(dllimport)
#endif

/* ==== 生命周期 ==== */

/* 扫描目录下所有 *.dll，先读 mod_info 做依赖检查，再按依赖顺序 mod_init。
 * 返回成功加载的模组数；扫描目录失败返回 UINT64_MAX。
 * 加载过程中的失败原因（缺依赖/接口版本不符/初始化失败/内容重名）
 * 会记进错误表，并由 modloader_init 打印一份摘要。 */
ML_API uint64_t modloader_init(const char *mods_dir);
ML_API void modloader_shutdown(void);
ML_API void modloader_pet_release(Pet *pet); /* 释放 Pet 里的动态数组与变长位图 */
ML_API uint64_t modloader_mod_count(void);
ML_API const char *modloader_mod_name(uint64_t index); /* 越界返回 NULL */

/* ==== 模组诊断（依赖检查 / 加载失败 / 内容重名） ==== */

typedef struct {
    const char *mod;    /* 模组名（拿不到 mod_info 时是文件名） */
    const char *file;   /* DLL 文件名，可空 */
    const char *reason; /* 一行原因，例如“缺少依赖 Core” */
    const char *detail; /* 详细信息，可空 */
    int warning;        /* 1=只是警告（例如内容重名），0=该模组没有加载 */
} ModError_t;

ML_API uint64_t modloader_error_count(void);
ML_API const ModError_t *modloader_error_at(uint64_t index); /* 越界返回 NULL */
ML_API void modloader_print_errors(const char *title);       /* 打印完整错误表 */

/* ==== 内容访问（与 LoaderApi 同名同义） ==== */

ML_API Id modloader_find(ContentKind_t kind, const char *name);
ML_API const void *modloader_get(ContentKind_t kind, Id id);
ML_API uint64_t modloader_count(ContentKind_t kind);
ML_API const char *modloader_name_of(ContentKind_t kind, Id id);
ML_API uint64_t modloader_name_count(Gender_t gender);
ML_API const char *modloader_name_at(Gender_t gender, uint64_t index);

/* ==== 菜单调度 ==== */

/* 渲染 parent 的子菜单并执行选中项（parent=NULL 为主菜单）。
 * 返回 1=已处理（继续），0=返回上一级，-1=请求退出游戏。
 * 主程序只调用这一项，具体菜单项由模组用 KIND_MENU 注册。 */
ML_API int modloader_menu_run(Pet *pet, const char *parent);

/* ==== 配置（KIND_CONFIG + config.dat 持久化） ==== */

ML_API int32_t modloader_config_get(const char *name);            /* 未注册/未存过返回 0 */
ML_API int modloader_config_set(const char *name, int32_t value); /* 夹到描述符范围并落盘 */
ML_API void modloader_set_config_file(const char *path);          /* 默认 config.dat，请在 init 前调用 */

/* ==== 存档（Pet 按名字序列化，XOR + 校验字节） ==== */

ML_API int modloader_save(const Pet *pet);            /* 存到当前存档文件，1=成功 */
ML_API int modloader_load(Pet *pet);                  /* 读存档，1=成功（失败不动 pet） */
ML_API void modloader_set_save_file(const char *path); /* 默认 pet.sav，请在 init 前调用 */

/* ==== 生死流程 ==== */

/* 一天结算：衰减 / 成长 / 生病 / 寿终；返回1=这天离世 */
ML_API int modloader_pet_day(Pet *pet);

/* 强制离世（自定义死因）；返回1=本次死亡。
 * 内部顺序：置 dead 与死因 → 播 scene 为 death 的剧情（模组可加遗言）→
 * 离世画面 → 写入逝去记录 → 跑一遍成就 */
ML_API int modloader_pet_die(Pet *pet, const char *reason);

/* 逝去记录（内存内，最多 MAX_DEAD 条，满后丢最旧的） */
ML_API uint32_t modloader_dead_count(void);
ML_API const DeadPet_t *modloader_dead_at(uint32_t index);
ML_API void modloader_dead_clear(void);
ML_API void modloader_set_dead_file(const char *path); /* 记录文件路径，默认 dead.dat；请在 init 前调用 */
ML_API void modloader_dead_menu(void); /* 分页浏览逝去宠物 */

/* 新宠物继承：有逝去记录时置 lost_friend（旧友的信） */
ML_API void modloader_pet_inherit(Pet *pet);

/* ==== 宠物定制（重生 / 祭坛共用） ==== */

/* 从名字库里手选一个名字；返回只读串，取消返回 NULL */
ML_API const char *modloader_name_pick(Gender_t gender, const char *title);

/* 特质商店：随机加一个（cost_random）/ 手选加一个（cost_pick）/
 * 删除一个（cost_delete，allow_delete=1 时才出现）。
 * 价钱从 pet->coins 扣；返回 1=已处理 */
ML_API int modloader_trait_shop(Pet *pet, const char *title, int allow_delete,
                                uint32_t cost_random, uint32_t cost_pick, uint32_t cost_delete);

/* ==== 内容驱动 ==== */

/* 轮询所有成就，返回本次新解锁的数量 */
ML_API int modloader_ach_poll(Pet *pet);

/* 播放匹配 scene 的第一段未播剧情（npc 为当前 NPC 名，可 NULL）；返回1=播了 */
ML_API int modloader_story_play(Pet *pet, const char *scene, const char *npc);

/* 按权重抽一个随机事件并触发；返回1=触发了 */
ML_API int modloader_event_roll(Pet *pet);

/* 列出某宠物类型可玩的小游戏 Id（pet_type=ID_NONE 表示只要通用游戏）；返回数量 */
ML_API uint64_t modloader_minigame_list(Id pet_type, Id *out, uint64_t max);

/* 跑一个小游戏；返回1=成功 */
ML_API int modloader_minigame_run(Pet *pet, Id game);

/* ==== 运行期服务（主程序/调试复用同一套输出与数值规则） ==== */

ML_API void ml_ui_title(const char *title);
ML_API void ml_ui_print(const char *fmt, ...);
ML_API int ml_key(void);
ML_API int ml_decode_key(int c1, int c2); /* 纯解码：c2<0 单键，返回 KEY_*，0=未识别 */
ML_API void ml_delay(uint32_t ms);
ML_API int ml_dialog(const char *speaker, const char *const *pages, uint32_t n_pages,
                     const StoryOpt_t *opts, uint32_t n_opts);
ML_API uint32_t ml_rnd(uint32_t lo, uint32_t hi);
ML_API int ml_inv_add(Pet *pet, Id item, uint64_t count);
ML_API uint64_t ml_inv_count(const Pet *pet, Id item);
ML_API int ml_passive_try(Pet *pet);
ML_API int ml_damage(Pet *pet, uint32_t amount);
ML_API int ml_ach_unlock(Pet *pet, Id ach);
ML_API void ml_exp_add(Pet *pet, int amount);
ML_API const LoaderApi *ml_api(void);

/* 无头模式（自动化测试用）：跳过清屏/停顿，按键自动确认 */
ML_API void modloader_set_headless(int on);
ML_API int  modloader_is_headless(void);

#endif
