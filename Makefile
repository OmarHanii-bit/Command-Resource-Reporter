CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Iinclude
WINDRES ?= windres

# Platform detection
ifeq ($(OS),Windows_NT)
    PLATFORM := Windows
    EXE_EXT := .exe
    LIB_EXT := .a
    CORE_SRC := src/core/crr_executor_win.c src/core/crr_formatter.c src/core/crr_stats.c
    CORE_LIBS := -lpsapi
    GUI_LIBS := -mwindows -lcomctl32 -lcomdlg32 -lshlwapi -lole32 -lpsapi
    MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
    RM = if exist "$(subst /,\,$(1))" del /Q /F "$(subst /,\,$(1))"
    RMDIR = if exist "$(subst /,\,$(1))" rmdir /S /Q "$(subst /,\,$(1))"
else
    PLATFORM := POSIX
    EXE_EXT :=
    LIB_EXT := .a
    CORE_SRC := src/core/crr_executor_posix.c src/core/crr_formatter.c src/core/crr_stats.c
    CORE_LIBS := -lm
    GUI_LIBS :=
    MKDIR = mkdir -p $(1)
    RM = rm -f $(1)
    RMDIR = rm -rf $(1)
endif

CORE_OBJS := $(patsubst src/core/%.c,build/%.o,$(CORE_SRC))
STATIC_LIB := build/libcrr$(LIB_EXT)

CLI_TARGET := bin/crr$(EXE_EXT)
GUI_TARGET := bin/crr_gui$(EXE_EXT)
TEST_TARGET := bin/test_runner$(EXE_EXT)

WORKLOADS := bin/cpu_burn$(EXE_EXT) \
             bin/mem_burn$(EXE_EXT) \
             bin/sleep_delay$(EXE_EXT) \
             bin/child_spawner$(EXE_EXT) \
             bin/fail_crash$(EXE_EXT) \
             bin/output_flood$(EXE_EXT) \
             bin/orphan_spawner$(EXE_EXT)

.PHONY: all clean test dirs cli gui workloads

all: dirs $(STATIC_LIB) $(CLI_TARGET) $(GUI_TARGET) $(WORKLOADS) $(TEST_TARGET)

dirs:
	@$(call MKDIR,bin)
	@$(call MKDIR,build)

# Core static library
build/%.o: src/core/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(CORE_OBJS)
	ar rcs $@ $(CORE_OBJS)

# CLI Binary
$(CLI_TARGET): src/cli/main.c $(STATIC_LIB)
	$(CC) $(CFLAGS) src/cli/main.c $(STATIC_LIB) $(CORE_LIBS) -o $@

# GUI Resource & Binary (Windows only)
ifeq ($(PLATFORM),Windows)
build/resource.o: src/gui/resource.rc src/gui/app.manifest
	$(WINDRES) -i src/gui/resource.rc -o $@

$(GUI_TARGET): src/gui/gui_main.c build/resource.o $(STATIC_LIB)
	$(CC) $(CFLAGS) src/gui/gui_main.c build/resource.o $(STATIC_LIB) $(GUI_LIBS) -o $@
else
$(GUI_TARGET):
	@echo "GUI is Windows-native Win32; skipping on $(PLATFORM)"
endif

# Test Workloads
bin/cpu_burn$(EXE_EXT): tests/test_workloads/cpu_burn.c
	$(CC) $(CFLAGS) $< -o $@

bin/mem_burn$(EXE_EXT): tests/test_workloads/mem_burn.c
	$(CC) $(CFLAGS) $< -o $@

bin/sleep_delay$(EXE_EXT): tests/test_workloads/sleep_delay.c
	$(CC) $(CFLAGS) $< -o $@

bin/child_spawner$(EXE_EXT): tests/test_workloads/child_spawner.c
	$(CC) $(CFLAGS) $< -o $@

bin/fail_crash$(EXE_EXT): tests/test_workloads/fail_crash.c
	$(CC) $(CFLAGS) $< -o $@

bin/output_flood$(EXE_EXT): tests/test_workloads/output_flood.c
	$(CC) $(CFLAGS) $< -o $@

bin/orphan_spawner$(EXE_EXT): tests/test_workloads/orphan_spawner.c
	$(CC) $(CFLAGS) $< -o $@

workloads: dirs $(WORKLOADS)

# Test Suite Runner
$(TEST_TARGET): tests/test_runner.c $(STATIC_LIB) $(WORKLOADS)
	$(CC) $(CFLAGS) tests/test_runner.c $(STATIC_LIB) $(CORE_LIBS) -o $@

test: $(TEST_TARGET)
	@echo "Executing Command Resource Reporter Test Suite..."
	@$(TEST_TARGET)

clean:
	@$(call RMDIR,build)
	@$(call RMDIR,bin)
	@echo "Cleanup completed."
