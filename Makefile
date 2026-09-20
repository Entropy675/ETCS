# --- Master Makefile for ETCS ---
# ====================================================================
# CONFIGURATION
# ====================================================================
LOADERS_DIR   := ./loaders
MODULES_DIR   := ./modules
TARGET_DIR    := ./bin
CORE_DIR      := ./core
LIBS_DIR      := ./libs
ONTOLOGY_DIR  := ./ontology

# Modules are subdirectories with their own Makefile
MODULE_SUBDIRS := $(patsubst %/,%,$(dir $(wildcard $(MODULES_DIR)/*/*.cc)))

# Platform-specific shared library extension
ifeq ($(OS),Windows_NT)
    LIB_EXT := dll
else
    LIB_EXT := so
endif
# Web builds produce side modules named *.wasm. Host uname is still Linux
# under emscripten, so LIB_EXT alone cannot name the artifact -- MODULE_EXTS
# is every suffix a module target may leave in modules/<name>/.
MODULE_EXTS := $(LIB_EXT) wasm

# Registration/Hash files
ONTOLOGY_HASH_FILE := ./ontology_hashes.h
LIBS_HASH_FILE     := ./libs_hashes.h
CORE_HASH_FILE     := ./core_hashes.h

# ====================================================================
# GLOBAL FLAGS & INHERITANCE
# ====================================================================

# Exporting these ensures all sub-make calls (loaders/modules) inherit them.
# EXTRADEFINES comes from the Python script: EXTRADEFINES="-DFLAG1 -DFLAG2"
export CFLAGS   += $(EXTRADEFINES)
export CXXFLAGS += $(EXTRADEFINES)

# Global variable mappings (e.g., ASAN=1)
ifeq ($(ASAN),1)
    export CFLAGS   += -fsanitize=address -fno-omit-frame-pointer
    export CXXFLAGS += -fsanitize=address -fno-omit-frame-pointer
    export LDFLAGS  += -fsanitize=address
endif
# emscripten: module/loader Makefiles key on ifdef EMSCRIPTEN.
ifdef EMSCRIPTEN
    export EMSCRIPTEN
endif

# ====================================================================
# PHONY TARGETS
# ====================================================================
.PHONY: all loaders modules clean clean_loaders clean_modules \
        copy_manifest copy_loaders copy_modules generate_hashes

# ====================================================================
# TOP LEVEL TARGETS
# ====================================================================
# THE PHASES ARE ORDERED BY A RECIPE, NOT BY PREREQUISITES.
#
# As three prerequisites these ran CONCURRENTLY under -j, and two of them ask
# for generate_hashes themselves -- so a parallel `all` had one process
# rewriting ontology_hashes.h while another compiled against it. Ordering them
# here costs nothing: the parallelism worth having is INSIDE modules and
# loaders, and each sub-make inherits -j through MAKEFLAGS.
#
# generate_hashes is not listed separately because both phases below already
# depend on it, so this now runs the pass twice instead of three times.
all:
	$(MAKE) modules
	$(MAKE) loaders

# generate_hashes IS A PREREQUISITE ONLY WHEN NOBODY ELSE HAS RUN IT.
#
# ace does the hash pass once before a batch and then builds the modules
# concurrently; without this guard every one of those makes would re-run it,
# which is both N redundant openssl sweeps and N writers on the same three
# headers. Set in the environment by ace, so it reaches every nested make
# without being passed along by hand.
HASH_PREREQ := generate_hashes
ifdef ACE_HASHES_READY
    HASH_PREREQ :=
endif

loaders: clean_loaders $(HASH_PREREQ)
	@echo "\n--- Building Loaders ---"
	$(MAKE) -C $(LOADERS_DIR)
	$(MAKE) copy_loaders

# ONE RECIPE PER MODULE, NOT ONE LOOP OVER ALL OF THEM.
#
# This was a shell `for` loop, and a loop inside a recipe is ONE command to make:
# -j could not touch it, so every module compiled in series no matter what the
# build was told. The per-directory rule it needed already existed and was simply
# never used ($(MODULE_SUBDIRS), under BUILD RULES below).
#
# STILL A SUB-MAKE, and that part is deliberate rather than leftover. Listing the
# modules as prerequisites of this target would let -j run them CONCURRENTLY WITH
# clean_modules and generate_hashes, which is a race that deletes artifacts out
# from under a compile and reads hash headers while they are being rewritten. The
# phases have to stay ordered; only the middle one parallelises. The sub-make
# inherits -j through MAKEFLAGS, so nothing has to be passed on by hand.
modules: clean_modules $(HASH_PREREQ)
	@echo "\n--- Building Modules ---"
	$(MAKE) $(MODULE_SUBDIRS)
	$(MAKE) copy_modules

# ====================================================================
# SINGLE MODULE TARGETS
# ====================================================================
module_%: $(HASH_PREREQ)
	@echo "\n--- Building Module: $(MODULES_DIR)/$* ---"
	@if [ ! -d "$(MODULES_DIR)/$*" ]; then \
		echo "[-] Error: Module '$*' not found in $(MODULES_DIR)/"; exit 1; \
	fi
	$(MAKE) -C $(MODULES_DIR)/$*
	@mkdir -p $(TARGET_DIR)
	@for ext in $(MODULE_EXTS); do \
		for f in $(MODULES_DIR)/$*/*.$$ext; do \
			if [ -f "$$f" ]; then \
				mv -f "$$f" $(TARGET_DIR)/; \
				echo "✓ Moved: $$f -> $(TARGET_DIR)/"; \
			fi; \
		done; \
	done

clean_module_%:
	@if [ ! -d "$(MODULES_DIR)/$*" ]; then \
		echo "[-] Error: Module '$*' not found in $(MODULES_DIR)/"; exit 1; \
	fi
	$(MAKE) -C $(MODULES_DIR)/$* clean
	@rm -f $(TARGET_DIR)/$*.$(LIB_EXT) $(TARGET_DIR)/$*.wasm

# ====================================================================
# HASH GENERATION
# ====================================================================
generate_hashes:
	@echo "--- Generating Global Manifests ---"
	@echo "// Auto-generated - do not edit" > $(ONTOLOGY_HASH_FILE)
	@for f in $(wildcard $(ONTOLOGY_DIR)/*.h); do \
		HASH=$$(openssl dgst -sha256 $$f | awk '{print $$NF}'); \
		BN=$$(basename $$f); \
		VAR=$$(echo $$BN | tr '.' '_'); \
		echo "inline const bool _reg_ont_$$VAR = []() { \
			ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); \
			ETCS::Entity::getManifest()[\"ONTOLOGY:$$BN\"] = \"$$HASH\"; \
			return true; \
		}();" >> $(ONTOLOGY_HASH_FILE); \
	done
	@echo "// Auto-generated - do not edit" > $(LIBS_HASH_FILE)
	@for f in $(wildcard $(LIBS_DIR)/*.h); do \
		HASH=$$(openssl dgst -sha256 $$f | awk '{print $$NF}'); \
		BN=$$(basename $$f); \
		VAR=$$(echo $$BN | tr '.' '_'); \
		echo "inline const bool _reg_lib_$$VAR = []() { \
			ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); \
			ETCS::Entity::getManifest()[\"LIB:$$BN\"] = \"$$HASH\"; \
			return true; \
		}();" >> $(LIBS_HASH_FILE); \
	done
	@echo "// Auto-generated - do not edit" > $(CORE_HASH_FILE)
	@for f in $(wildcard $(CORE_DIR)/*.h); do \
		HASH=$$(openssl dgst -sha256 $$f | awk '{print $$NF}'); \
		BN=$$(basename $$f); \
		VAR=$$(echo $$BN | tr '.' '_'); \
		echo "inline const bool _reg_core_$$VAR = []() { \
			ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); \
			ETCS::Entity::getManifest()[\"CORE:$$BN\"] = \"$$HASH\"; \
			return true; \
		}();" >> $(CORE_HASH_FILE); \
	done

# ====================================================================
# BUILD RULES
# ====================================================================
$(MODULE_SUBDIRS):
	@echo "\n--- Building Module: $@ ---"
	$(MAKE) -C $@

# ====================================================================
# COPY TARGETS
# ====================================================================
copy_loaders:
	@mkdir -p $(TARGET_DIR)
	@echo "\n--- Moving Loaders to $(TARGET_DIR)/ ---"
	@for f in $(LOADERS_DIR)/Run_*; do \
		if [ -f "$$f" ]; then \
			mv -f "$$f" $(TARGET_DIR)/; \
			echo "✓ Moved loader: $$f -> $(TARGET_DIR)/"; \
		fi; \
	done

copy_modules:
	@mkdir -p $(TARGET_DIR)
	@echo "\n--- Moving Modules to $(TARGET_DIR)/ ---"
	@for dir in $(MODULE_SUBDIRS); do \
		for ext in $(MODULE_EXTS); do \
			for f in $$dir/*.$$ext; do \
				if [ -f "$$f" ]; then \
					mv -f "$$f" $(TARGET_DIR)/; \
					echo "✓ Moved module: $$f -> $(TARGET_DIR)/"; \
				fi; \
			done; \
		done; \
	done

copy_manifest: copy_loaders copy_modules

# ====================================================================
# CLEANUP
# ====================================================================
clean:
	@echo "--- Performing Full Pre-Build Cleanup ---"
	$(MAKE) -C $(LOADERS_DIR) clean
	@for dir in $(MODULE_SUBDIRS); do \
		if [ -d "$$dir" ]; then $(MAKE) -C "$$dir" clean; fi; \
	done
	@rm -f $(ONTOLOGY_HASH_FILE) $(LIBS_HASH_FILE) $(CORE_HASH_FILE)
	@rm -f $(TARGET_DIR)/Run_*
	@rm -f $(TARGET_DIR)/*.$(LIB_EXT) $(TARGET_DIR)/*.wasm
	@echo "--- Cleanup Complete ---\n"

clean_loaders:
	@echo "--- Cleaning Loaders ---"
	$(MAKE) -C $(LOADERS_DIR) clean
	@rm -f $(TARGET_DIR)/Run_*
	@echo "--- Loader Cleanup Complete ---\n"

clean_modules:
	@echo "--- Cleaning Modules ---"
	@for dir in $(MODULE_SUBDIRS); do \
		if [ -d "$$dir" ]; then $(MAKE) -C "$$dir" clean; fi; \
	done
	@rm -f $(TARGET_DIR)/*.$(LIB_EXT) $(TARGET_DIR)/*.wasm
	@echo "--- Module Cleanup Complete ---\n"
