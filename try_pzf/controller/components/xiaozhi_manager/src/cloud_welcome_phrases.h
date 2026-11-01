#pragma once
#include <stddef.h>

/* Entire commands, including prefix/punctuation, are under 15 characters.
 * This is the user's tested input envelope, not a protocol specification. */
static const char *const cloud_welcome_phrases[] = {
    "请只朗读：欢迎回来",
    "请只朗读：很高兴见到你",
    "请只朗读：你来啦我在呢",
    "请只朗读：摸摸头真开心",
    "请只朗读：今天也陪着你",
    "请只朗读：一起慢慢来吧",
    "请只朗读：给你一点好心情",
    "请只朗读：准备好就出发吧",
    "请只朗读：有你在真好",
};
#define CLOUD_WELCOME_COUNT (sizeof(cloud_welcome_phrases) / sizeof(cloud_welcome_phrases[0]))
