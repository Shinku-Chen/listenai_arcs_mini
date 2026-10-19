#ifndef __ARCADE_APP_H__
#define __ARCADE_APP_H__

/**
 * 模拟器应用入口（CONFIG_APPLICATION_ARCADE=y 时由 main.c 调用）。
 * 函数内部创建应用任务，正常情况下不会返回。
 */
int arcade_app_main(void);

#endif /* __ARCADE_APP_H__ */
