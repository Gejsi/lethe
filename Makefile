CXX      := g++
CXXFLAGS := -std=c++20

SRC_DIR  := src
OBJ_DIR  := obj
BIN_DIR  := bin
DEP_DIR  := $(OBJ_DIR)/.deps

# Target executable name
TARGET   := $(BIN_DIR)/main

# Gather all cpp source files and corresponding object files
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(patsubst $(SRC_DIR)/%.cpp,$(DEP_DIR)/%.d,$(SRCS))

# Paths to VoliMem
VOLIMEM_DIR := $(HOME)/Desktop/voli
VOLIMEM_LIB := $(VOLIMEM_DIR)/build/lib
VOLIMEM_INC := $(VOLIMEM_DIR)/include

CXXFLAGS += -isystem $(VOLIMEM_INC)
LDFLAGS += -L$(VOLIMEM_LIB) -lvolimem

# Build mode flags
DEBUG_FLAGS   := -O0 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -fsanitize=undefined -fno-omit-frame-pointer
RELEASE_FLAGS := -O3 -march=native -funroll-loops -flto
MODE ?= debug
ifeq ($(MODE),release)
	CXXFLAGS += $(RELEASE_FLAGS)
else
	CXXFLAGS += $(DEBUG_FLAGS)
endif

# Create required directories if they don't exist
$(shell mkdir -p $(OBJ_DIR) $(BIN_DIR) $(DEP_DIR))

# Include generated dependency files
-include $(DEPS)

# Default target
all: $(TARGET)

# Linking object files into the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Compiling each .cpp file into an object file
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@ -MT $@ -MMD -MP -MF $(DEP_DIR)/$*.d

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

run: $(TARGET)
	LD_LIBRARY_PATH=$(VOLIMEM_LIB) ./$(TARGET)

debug: $(TARGET)
	LD_LIBRARY_PATH=$(VOLIMEM_LIB) gdb ./$(TARGET)

.PHONY: all clean run debug
