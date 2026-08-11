#include "inotask_config.h"
#include "inotask_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n",                       \
                          __FILE__, __LINE__, #condition);                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static const it_watch *find_watch(const it_config *config, const char *path)
{
    size_t watch_index;
    for (watch_index = 0; watch_index < config->watches.n; watch_index++) {
        if (strcmp(config->watches.v[watch_index].path.s, path) == 0)
            return &config->watches.v[watch_index];
    }
    return NULL;
}

static void test_paths(void)
{
    CHECK(it_path_is_absolute("/tmp"));
    CHECK(it_path_is_absolute("/"));
    CHECK(!it_path_is_absolute("tmp"));
    CHECK(!it_path_is_absolute(""));
    CHECK(!it_path_is_absolute(NULL));
}

static void test_watch_merging_and_plan(void)
{
    const char *run_task[] = { "worker" };
    it_config config;
    it_runtime_plan plan;
    const it_watch *tmp_watch;
    const it_watch *var_watch;

    it_config_init(&config);
    it_runtime_plan_init(&plan);

    CHECK(it_config_add_task(&config, "worker", "/usr/bin/true",
                             NULL, 0) == IT_CFG_OK);
    CHECK(it_config_add_rule(&config, "tmp_create", "/tmp", IN_CREATE,
                             0, NULL, 0, NULL, 0,
                             run_task, 1) == IT_CFG_OK);
    CHECK(it_config_add_rule(&config, "tmp_moved_to", "/tmp", IN_MOVED_TO,
                             250, NULL, 0, NULL, 0,
                             run_task, 1) == IT_CFG_OK);
    CHECK(it_config_add_rule(&config, "var_attrib", "/var/tmp", IN_ATTRIB,
                             0, NULL, 0, NULL, 0,
                             run_task, 1) == IT_CFG_OK);

    CHECK(config.tasks.n == 1);
    CHECK(config.rules.n == 3);
    CHECK(config.watches.n == 2);

    tmp_watch = find_watch(&config, "/tmp");
    var_watch = find_watch(&config, "/var/tmp");
    CHECK(tmp_watch != NULL);
    CHECK(var_watch != NULL);
    if (tmp_watch)
        CHECK(tmp_watch->events == (IN_CREATE | IN_MOVED_TO));
    if (tmp_watch) CHECK((tmp_watch->events & IN_MOVED_FROM) == 0);
    if (var_watch) CHECK(var_watch->events == IN_ATTRIB);

    CHECK(it_runtime_plan_build(&config, &plan));
    CHECK(plan.targets.n == 2);
    if (plan.targets.n == 2) {
        CHECK(strcmp(plan.targets.v[0].path.s, "/tmp") == 0);
        CHECK(plan.targets.v[0].spec_index == 0);
        CHECK(strcmp(plan.targets.v[1].path.s, "/var/tmp") == 0);
        CHECK(plan.targets.v[1].spec_index == 1);
    }

    it_runtime_plan_free(&plan);
    it_config_free(&config);
}

static void test_invalid_config_inputs(void)
{
    const char *run_task[] = { "worker" };
    it_config config;

    it_config_init(&config);
    CHECK(it_config_add_watch(&config, "tmp", IN_CREATE) ==
          IT_CFG_EWATCH_PATH_NOT_ABS);
    CHECK(it_config_add_watch(&config, "/tmp", 0) ==
          IT_CFG_EWATCH_EVENTS_EMPTY);
    CHECK(it_config_add_task(&config, "worker", "usr/bin/true", NULL, 0) ==
          IT_CFG_ETASK_EXEC_NOT_ABS);
    CHECK(it_config_add_rule(&config, "empty_events", "/tmp", 0, 0,
                             NULL, 0, NULL, 0, run_task, 1) ==
          IT_CFG_ERULE_EVENTS_EMPTY);
    CHECK(it_config_add_rule(&config, "empty_run", "/tmp", IN_CREATE, 0,
                             NULL, 0, NULL, 0, NULL, 0) ==
          IT_CFG_ERULE_RUN_EMPTY);
    it_config_free(&config);
}

int main(void)
{
    test_paths();
    test_watch_merging_and_plan();
    test_invalid_config_inputs();

    if (failures != 0) {
        (void)fprintf(stderr, "%d runtime unit test(s) failed\n", failures);
        return 1;
    }
    (void)puts("runtime unit tests passed");
    return 0;
}
