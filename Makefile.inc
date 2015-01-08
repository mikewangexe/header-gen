clang = $(CLANG)
plugin = $(TOP)/DeclFilter.so
composer = $(TOP)/DeclComposer.py

CC_PATH = $(addprefix -I,$(header_paths))
clang_plugin_args = -cc1 -print-stats -load $(plugin) -plugin decl-filter

marker = ">>>"

all: $(files:.c=.o) $(addsuffix .o,$(directories))

define template_file =

  $(1).sqlite: $(1).oo $(plugin)
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS decls'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS all_decls'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS macros'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS deps'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS prototypes'
	@$(clang) $(clang_plugin_args) -plugin-arg-decl-filter $(1).sqlite $(CC_PATH) $(CC_FLAGS) $(1).c > /dev/null 2>&1

  $(1).o: $(1).sqlite $(composer)
	@python $(composer) -o $(1).d --db $(1).sqlite $(composer_flags) $(1).c
	@printf "=== %-50sOK\n" $(1)

  $(1).oo: $(1).c
	@$(clang) $(CC_PATH) $(CC_FLAGS) -c -o $(1).oo $(1).c

  debug-$(1): $(1).oo FORCE
	@$(clang) $(clang_plugin_args) $(CC_PATH) $(CC_FLAGS) $(1).c 2>&1 | grep $(marker)

  dump-$(1): $(1).d FORCE
	@sqlite3 $(1).sqlite 'SELECT * FROM decls'

  .SECONDARY: $(1).oo $(1).d $(1).o

endef

$(foreach f,$(files:.c=),$(eval $(call template_file,$(f))))

define template_directory =
  $(1)_src := $(wildcard $(1)/*.c)
  $(1)_obj := $$($(1)_src:.c=.o)
  $(1)_original_obj := $$($(1)_src:.c=.oo)
  $(1)_debug := $$(addprefix debug-,$$($(1)_src:.c=))

  $(1).sqlite: $(1).oo $(plugin)
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS decls'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS all_decls'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS macros'
	@sqlite3 $(1).sqlite 'DROP TABLE IF EXISTS deps'
	@$(clang) $(clang_plugin_args) -plugin-arg-decl-filter $(1).sqlite $(CC_PATH) $(CC_FLAGS) $$($(1)_src) > /dev/null 2>&1 || true

  $(1).o: $(1).sqlite $(composer)
	@python $(composer) -o $(1).d --db $(1).sqlite $(composer_flags) $(1)

  $$($(1)_obj): %.o: %.c $(1).d
	@$(clang) -I$(1) -I$(1).d $(CC_FLAGS) $(CC_OBJ_FLAGS) -c -o $$@ $$<

  $$($(1)_debug): debug-%: %.c FORCE
	@$(clang) $(clang_plugin_args) -I$(1).d $(CC_PATH) $(CC_FLAGS) $(CC_OBJ_FLAGS) $$< 2>&1 | grep $(marker)

  $(1).oo: $$($(1)_original_obj)
	@$(TOOLCHAIN_PREFIX)ld -r -o $$@ $$+

  $$($(1)_original_obj): %.oo: %.c
	@$(clang) -I$(1) $(CC_PATH) $(CC_FLAGS) $(CC_OBJ_FLAGS) -c -o $$@ $$<

endef

$(foreach d,$(directories),$(eval $(call template_directory,$(d))))

FORCE:

PHONY += FORCE

clean:
	@find . -name '*.o' -delete
	@find . -name '*.oo' -delete
	@find . -name '*.builtin' -delete
	@rm -rf *.sqlite *.d *.log *.dummy.c
