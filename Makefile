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

# ====================================================================
# PHONY TARGETS
# ====================================================================
.PHONY: all loaders modules clean clean_loaders clean_modules \
        copy_manifest copy_loaders copy_modules generate_hashes

# ====================================================================
# TOP LEVEL TARGETS
# ====================================================================
all: generate_hashes modules loaders

loaders: clean_loaders generate_hashes
	@echo "\n--- Building Loaders ---"
	$(MAKE) -C $(LOADERS_DIR)
	$(MAKE) copy_loaders

modules: clean_modules generate_hashes
	@echo "\n--- Building Modules ---"
	@for dir in $(MODULE_SUBDIRS); do \
		if [ -d "$$dir" ]; then \
			echo "\n--- Building Module: $$dir ---"; \
			$(MAKE) -C "$$dir"; \
		fi; \
	done
	$(MAKE) copy_modules

# ====================================================================
# SINGLE MODULE TARGETS
# ====================================================================
module_%: generate_hashes
	@echo "\n--- Building Module: $(MODULES_DIR)/$* ---"
	@if [ ! -d "$(MODULES_DIR)/$*" ]; then \
		echo "[-] Error: Module '$*' not found in $(MODULES_DIR)/"; exit 1; \
	fi
	$(MAKE) -C $(MODULES_DIR)/$*
	@mkdir -p $(TARGET_DIR)
	@for f in $(MODULES_DIR)/$*/*.$(LIB_EXT); do \
		if [ -f "$$f" ]; then \
			mv -f "$$f" $(TARGET_DIR)/; \
			echo "✓ Moved: $$f -> $(TARGET_DIR)/"; \
		fi; \
	done

clean_module_%:
	@if [ ! -d "$(MODULES_DIR)/$*" ]; then \
		echo "[-] Error: Module '$*' not found in $(MODULES_DIR)/"; exit 1; \
	fi
	$(MAKE) -C $(MODULES_DIR)/$* clean
	@rm -f $(TARGET_DIR)/$*.$(LIB_EXT)

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
		for f in $$dir/*.$(LIB_EXT); do \
			if [ -f "$$f" ]; then \
				mv -f "$$f" $(TARGET_DIR)/; \
				echo "✓ Moved module: $$f -> $(TARGET_DIR)/"; \
			fi; \
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
	@rm -f $(TARGET_DIR)/*.$(LIB_EXT)
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
	@rm -f $(TARGET_DIR)/*.$(LIB_EXT)
	@echo "--- Module Cleanup Complete ---\n"
