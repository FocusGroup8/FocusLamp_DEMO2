#include "cloud_welcome_alias.h"
#include <stddef.h>
#include <string.h>

bool cloud_welcome_matches(const char *text)
{
    static const char *aliases[] = {
        "欢迎播报",
        "请只说：欢迎回来，我是小智，今天也一起专注学习吧！",
        "请只说：你好呀，准备好了吗？让我们开始今天的学习吧！",
        "请只说：很高兴见到你，记得保持坐姿，也别忘了适时休息！",
    };
    if (!text) return false;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        if (!strcmp(text, aliases[i])) return true;
    return false;
}
