#include "../../head/main/display/task_guide_model.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>

static const char *steps[] = {"丢掉桌面垃圾", "把物品分类放好", "擦净桌面"};

static void validation()
{
    assert(tg_validate_steps(steps));
    const char *english[] = {"Sort items", "Put books away", "Wipe the desk"};
    assert(tg_validate_steps(english));
    const char *invalid[] = {"", "擦桌子", "分类"};
    assert(!tg_validate_steps(invalid));
    invalid[0] = "一二三四五六七八九一二三四五六七八九";
    assert(tg_validate_steps(invalid));
    invalid[0] = "一二三四五六七八九一二三四五六七八九十";
    assert(!tg_validate_steps(invalid));
    const char *bad[] = {"1.擦桌子", "一、擦桌子", "①擦桌子", "*擦桌子", "#擦桌子",
        "擦\n桌子", "擦\t桌子", " 擦桌子", "擦桌子　", "擦`桌子", "擦\x7f桌子",
        "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8", "\xf0\x9f"};
    for (const char *text : bad) {
        invalid[0] = text;
        assert(!tg_validate_steps(invalid));
    }
    invalid[0] = invalid[1];
    assert(!tg_validate_steps(invalid));
    assert(!tg_validate_steps(nullptr));
    assert(!tg_utf8_next(nullptr, nullptr));
    const char *emoji = "\xf0\x9f\x98\x80";
    uint32_t cp;
    assert(tg_utf8_next(&emoji, &cp) && cp == 0x1f600 && !*emoji);
}

static void flow()
{
    task_guide::Model model;
    using State = task_guide::State;
    assert(!model.observe(77, 0, "idle", nullptr, true, 0));
    assert(!model.observe(77, 1, "waiting", nullptr, true, 100));
    assert(model.observe(77, 1, "ready", steps, true, 200));
    assert(model.view().state == State::Preview && model.view().remaining_ms == 180000);
    model.update(60000); // Reading the preview takes no timer time.
    assert(model.view().state == State::Preview && model.view().remaining_ms == 180000);
    assert(model.click(60000));
    assert(model.view().state == State::Running && !std::strcmp(model.report_state(), "running"));
    const uint32_t started_revision = model.view().revision;
    model.update(239999);
    assert(model.view().step == 0 && model.view().remaining_ms == 1);
    assert(model.view().revision == started_revision);
    model.update(240000);
    assert(model.view().step == 1 && model.view().remaining_ms == 180000);
    // No observe or HTTP call for nine minutes: all progression is local.
    model.update(425000);
    assert(model.view().step == 2 && model.view().remaining_ms == 175000);
    model.update(600000);
    assert(model.view().state == State::Done && model.view().remaining_ms == 0);
    assert(!std::strcmp(model.report_state(), "done"));
    assert(model.click(600001)); // Completion screen consumes releases harmlessly.
    const uint32_t terminal_revision = model.view().revision;
    model.update(602000);
    assert(model.view().state == State::Idle);
    assert(!std::strcmp(model.report_state(), "done") && model.view().revision == terminal_revision);
    assert(!model.observe(77, 1, "ready", steps, true, 603000));
    assert(model.observe(77, 2, "ready", steps, true, 604000));
    assert(model.click(604001));
    assert(model.click(604002)); // A separate rapid second tap really exits.
    assert(model.view().state == State::Idle && !std::strcmp(model.report_state(), "dismissed"));
    assert(!model.observe(77, 2, "ready", steps, true, 604003));
    assert(!model.observe(77, 1, "ready", steps, true, 604004));
}

static void requests_and_modes()
{
    using State = task_guide::State;
    task_guide::Model cold;
    assert(!cold.observe(5, 7, "ready", steps, true, 100));
    assert(cold.view().state == State::Idle && !std::strcmp(cold.report_state(), "dismissed"));
    assert(!cold.observe(5, 7, "ready", steps, true, 200));
    assert(cold.observe(5, 8, "ready", steps, true, 300));
    cold.click(400);
    cold.observe(5, 9, "waiting", nullptr, true, 500);
    assert(cold.view().request_id == 8 && cold.view().state == State::Running);
    // Old request may not replace running work after a newer explicit request.
    assert(!cold.observe(5, 8, "ready", steps, true, 600));
    assert(cold.observe(5, 9, "ready", steps, true, 700));
    assert(cold.view().state == State::Preview && cold.view().request_id == 9);
    cold.observe(5, 9, "ready", steps, false, 800);
    assert(cold.view().state == State::Idle);
    assert(!cold.observe(5, 9, "ready", steps, true, 900));
    assert(cold.observe(5, 10, "ready", steps, true, 1000));
    cold.click(1100);
    // A controller restart in companion mode cannot reset the head clock.
    cold.observe(6, 0, "idle", nullptr, true, 2000);
    assert(cold.view().state == State::Running && cold.view().controller_boot_id == 5);
    cold.observe(6, 1, "waiting", nullptr, true, 2500);
    assert(cold.observe(6, 1, "ready", steps, true, 3000));
    assert(cold.view().state == State::Preview && cold.view().controller_boot_id == 6);
    cold.cancel();
    assert(cold.view().state == State::Idle);
    assert(!cold.observe(6, 1, "ready", steps, true, 3100));
    const char *invalid[] = {"短句", "短句", "第三句"};
    assert(!cold.observe(6, 2, "ready", invalid, true, 3200));
    assert(cold.observe(6, 2, "ready", steps, true, 3300));
    cold.click(3400);
    cold.observe(6, 2, "done", nullptr, true, 3500);
    assert(cold.view().state == State::Running); // Only head decides completion.
    cold.observe(6, 2, "dismissed", nullptr, true, 3600);
    assert(cold.view().state == State::Idle);
    assert(!cold.observe(0, 3, "ready", steps, true, 3700));
    assert(!cold.observe(6, 3, "invalid-state", steps, true, 3800));
}

int main()
{
    validation(); flow(); requests_and_modes();
    std::puts("task-guide validation, lifecycle, offline clock and request isolation passed");
}
