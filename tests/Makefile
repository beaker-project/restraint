SHELL = /bin/bash
CC = gcc

LIBS =
PACKAGES =
TEST_PROGRAMS =
RESTRAINT_OBJS =

PACKAGES += gio-2.0
PACKAGES += gio-unix-2.0
PACKAGES += glib-2.0
PACKAGES += gobject-2.0
PACKAGES += json-c
PACKAGES += libarchive
PACKAGES += libcurl
PACKAGES += libsoup-2.4
PACKAGES += libxml-2.0

SRC_DIR = $(abspath ../src)

CFLAGS ?= -Og -g
CFLAGS += -iquote $(SRC_DIR)
CFLAGS += -Wall -Werror -std=c99 $(shell pkg-config --cflags $(PACKAGES))

ifeq ($(COVERAGE), 1)
    CFLAGS += --coverage
    LDFLAGS ?= --coverage
endif

ifeq ($(STATIC), 1)
    # The -( -) grouping means we don't have to worry about getting all the
    # dependent libs in the right order (normally pkg-config would do that for
    # us).
    LIBS = -Wl,-\( -Wl,-Bdynamic -lm -pthread -lrt -lresolv -ldl -lutil -Wl,-\) -Wl,-Bstatic -Wl,-\( $(shell pkg-config --static --libs $(PACKAGES)) -Wl,-\) -Wl,-Bdynamic $(LFLAGS)
else
    LIBS = $(shell pkg-config --libs $(PACKAGES) $(XTRAPKGS)) -lutil -pthread
endif

TEST_PROGRAMS += test_beaker_harness
TEST_PROGRAMS += test_cmd_abort
TEST_PROGRAMS += test_cmd_log
TEST_PROGRAMS += test_cmd_result
TEST_PROGRAMS += test_cmd_utils
TEST_PROGRAMS += test_cmd_watchdog
TEST_PROGRAMS += test_config
TEST_PROGRAMS += test_dependency
TEST_PROGRAMS += test_env
TEST_PROGRAMS += test_fetch_git
TEST_PROGRAMS += test_fetch_uri
TEST_PROGRAMS += test_logging
TEST_PROGRAMS += test_metadata
TEST_PROGRAMS += test_process
#TEST_PROGRAMS += test_recipe
TEST_PROGRAMS += test_task
TEST_PROGRAMS += test_upload
TEST_PROGRAMS += test_utils

.PHONY: all
all: $(TEST_PROGRAMS)

$(TEST_PROGRAMS): %: %.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

### test_beaker_harness
#
BEAKER_HARNESS_OBJS =
BEAKER_HARNESS_OBJS += beaker_harness.o

RESTRAINT_OBJS += $(BEAKER_HARNESS_OBJS)

MOCKS_BEAKER_HARNESS = -Wl,--wrap=soup_session_send_message

test_beaker_harness: $(BEAKER_HARNESS_OBJS)
	$(CC) $(MOCKS_BEAKER_HARNESS) $(LDFLAGS) -o $@ $^ $(LIBS)

test_beaker_harness.o: test_beaker_harness.c
	$(CC) $(MOCKS_BEAKER_HARNESS) $(CFLAGS) -c -o $@ $^ $(LIBS)

### test_cmd_abort
#
CMD_ABORT_OBJS =
CMD_ABORT_OBJS += cmd_abort.o
CMD_ABORT_OBJS += cmd_utils.o
CMD_ABORT_OBJS += errors.o
CMD_ABORT_OBJS += utils.o

RESTRAINT_OBJS += $(CMD_ABORT_OBJS)

test_cmd_abort: $(CMD_ABORT_OBJS)

### test_cmd_log
#
CMD_LOG_OBJS =
CMD_LOG_OBJS += cmd_log.o
CMD_LOG_OBJS += cmd_utils.o
CMD_LOG_OBJS += errors.o
CMD_LOG_OBJS += upload.o
CMD_LOG_OBJS += utils.o

RESTRAINT_OBJS += $(CMD_LOG_OBJS)

test_cmd_log: $(CMD_LOG_OBJS)

### test_cmd_result
#
CMD_RESULT_OBJS =
CMD_RESULT_OBJS += cmd_result.o
CMD_RESULT_OBJS += cmd_utils.o
CMD_RESULT_OBJS += errors.o
CMD_RESULT_OBJS += upload.o
CMD_RESULT_OBJS += utils.o

RESTRAINT_OBJS += $(CMD_RESULT_OBJS)

test_cmd_result: $(CMD_RESULT_OBJS)

### test_cmd_utils
#
CMD_UTILS_OBJS =
CMD_UTILS_OBJS += cmd_utils.o
CMD_UTILS_OBJS += env.o
CMD_UTILS_OBJS += errors.o
CMD_UTILS_OBJS += utils.o

RESTRAINT_OBJS += $(CMD_UTILS_OBJS)

test_cmd_utils: $(CMD_UTILS_OBJS)

### test_cmd_watchdog
#
CMD_WATCHDOG_OBJS =
CMD_WATCHDOG_OBJS += cmd_utils.o
CMD_WATCHDOG_OBJS += cmd_watchdog.o
CMD_WATCHDOG_OBJS += errors.o
CMD_WATCHDOG_OBJS += utils.o

RESTRAINT_OBJS += $(CMD_WATCHDOG_OBJS)

test_cmd_watchdog: $(CMD_WATCHDOG_OBJS)

### test_config
#
CONFIG_OBJS =
CONFIG_OBJS += config.o

RESTRAINT_OBJS += $(CONFIG_OBJS)

test_config: $(CONFIG_OBJS)

### test_dependency
#
DEPENDENCY_OBJS =
DEPENDENCY_OBJS += dependency.o
DEPENDENCY_OBJS += errors.o
DEPENDENCY_OBJS += fetch.o
DEPENDENCY_OBJS += fetch_git.o
DEPENDENCY_OBJS += fetch_uri.o
DEPENDENCY_OBJS += metadata.o
DEPENDENCY_OBJS += param.o
DEPENDENCY_OBJS += process.o
DEPENDENCY_OBJS += restraint_forkpty.o
DEPENDENCY_OBJS += utils.o

RESTRAINT_OBJS += $(DEPENDENCY_OBJS)

test_dependency: $(DEPENDENCY_OBJS)

### test_env
#
ENV_OBJS =
ENV_OBJS += cmd_utils.o
ENV_OBJS += env.o
ENV_OBJS += errors.o
ENV_OBJS += utils.o

RESTRAINT_OBJS += $(ENV_OBJS)

test_env: $(ENV_OBJS)

### test_fetch_git
#
FETCH_GIT_OBJS =
FETCH_GIT_OBJS += errors.o
FETCH_GIT_OBJS += fetch.o
FETCH_GIT_OBJS += fetch_git.o

RESTRAINT_OBJS += $(FETCH_GIT_OBJS)

test_fetch_git: $(FETCH_GIT_OBJS)

### test_fetch_uri
#
FETCH_URI_OBJS =
FETCH_URI_OBJS += errors.o
FETCH_URI_OBJS += fetch.o
FETCH_URI_OBJS += fetch_uri.o

RESTRAINT_OBJS += $(FETCH_URI_OBJS)

test_fetch_uri: $(FETCH_URI_OBJS)

### test_logging
#
# logging.c is included in test_logging.c, therefore there is no need
# for logging.o
#
LOGGING_OBJS =
LOGGING_OBJS += beaker_harness.o
LOGGING_OBJS += config.o
LOGGING_OBJS += dependency.o
LOGGING_OBJS += env.o
LOGGING_OBJS += errors.o
LOGGING_OBJS += fetch.o
LOGGING_OBJS += fetch_git.o
LOGGING_OBJS += fetch_uri.o
LOGGING_OBJS += message.o
LOGGING_OBJS += metadata.o
LOGGING_OBJS += param.o
LOGGING_OBJS += process.o
LOGGING_OBJS += recipe.o
LOGGING_OBJS += restraint_forkpty.o
LOGGING_OBJS += role.o
LOGGING_OBJS += task.o
LOGGING_OBJS += utils.o
LOGGING_OBJS += xml.o

RESTRAINT_OBJS += $(LOGGING_OBJS)

test_logging: $(LOGGING_OBJS)
test_logging.o: $(SRC_DIR)/logging.c

### test_metadata
#
METADATA_OBJS =
METADATA_OBJS += errors.o
METADATA_OBJS += metadata.o
METADATA_OBJS += param.o
METADATA_OBJS += process.o
METADATA_OBJS += restraint_forkpty.o
METADATA_OBJS += utils.o

RESTRAINT_OBJS += $(METADATA_OBJS)

test_metadata: $(METADATA_OBJS)

### test_process
#
PROCESS_OBJS =
PROCESS_OBJS += errors.o
PROCESS_OBJS += process.o
PROCESS_OBJS += restraint_forkpty.o

RESTRAINT_OBJS += $(PROCESS_OBJS)

test_process: $(PROCESS_OBJS)

### test_recipe
#
RECIPE_OBJS =
RECIPE_OBJS += fetch_git.o
RECIPE_OBJS += metadata.o
RECIPE_OBJS += param.o
RECIPE_OBJS += recipe.o
RECIPE_OBJS += role.o
RECIPE_OBJS += task.o

RESTRAINT_OBJS += $(RECIPE_OBJS)

test_recipe: $(RECIPE_OBJS)

### test_task
#
# task.c is included in test_task.c, therefore there is no need to link
# task.o.
#
TASK_OBJS =
TASK_OBJS += beaker_harness.o
TASK_OBJS += config.o
TASK_OBJS += dependency.o
TASK_OBJS += env.o
TASK_OBJS += errors.o
TASK_OBJS += fetch.o
TASK_OBJS += fetch_git.o
TASK_OBJS += fetch_uri.o
TASK_OBJS += logging.o
TASK_OBJS += metadata.o
TASK_OBJS += param.o
TASK_OBJS += process.o
TASK_OBJS += recipe.o
TASK_OBJS += restraint_forkpty.o
TASK_OBJS += role.o
TASK_OBJS += utils.o
TASK_OBJS += xml.o

RESTRAINT_OBJS += $(TASK_OBJS)

test_task: $(TASK_OBJS)
test_task.o: $(SRC_DIR)/task.c

### test_upload
#
UPLOAD_OBJS =
UPLOAD_OBJS += upload.o

RESTRAINT_OBJS += $(UPLOAD_OBJS)

test_upload: $(UPLOAD_OBJS)

### test_utils
#
UTILS_OBJS =
UTILS_OBJS += errors.o
UTILS_OBJS += utils.o

RESTRAINT_OBJS += $(UTILS_OBJS)

test_utils: $(UTILS_OBJS)

### Restraint objects
#
RESTRAINT_OBJS := $(sort $(RESTRAINT_OBJS))

$(RESTRAINT_OBJS): %.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test-data/git-remote: test-data/git-remote.tgz
	tar --no-same-owner -C test-data -xzf $<

.PHONY: check
check: $(TEST_PROGRAMS) test-data/git-remote
	./run-tests.sh $(TEST_PROGRAMS)

.PHONY: valgrind
valgrind: $(TEST_PROGRAMS) test-data/git-remote
	./run-tests.sh --valgrind $(TEST_PROGRAMS)

.PHONY: coverage
coverage:
	gcov -s $(SRC_DIR) -o . $(SRC_DIR)/*.c

.PHONY: check-coverage
check-coverage:
	$(MAKE) COVERAGE=1 clean check coverage

.PHONY: clean
clean:
	rm -rf $(TEST_PROGRAMS) *.o *.gcov *.gcda *.gcno rstrnt-commands-env-*.sh test_logging_logs/
