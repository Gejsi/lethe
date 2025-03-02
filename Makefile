CXX      := g++
CXXFLAGS := -std=c++11

SRC_DIR  := src
OBJ_DIR  := obj
BIN_DIR  := bin

# Target executable name
TARGET   := $(BIN_DIR)/main

# Gather all cpp source files and corresponding object files
SRCS     := $(wildcard $(SRC_DIR)/*.cpp)
OBJS     := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

# Build mode flags
DEBUG_FLAGS   := -O0 -g -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wsign-conversion -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -DDEBUG
RELEASE_FLAGS := -O3 -march=native -funroll-loops -flto -DNDEBUG

# Choose build mode (default is debug)
MODE ?= debug
ifeq ($(MODE),release)
    CXXFLAGS += $(RELEASE_FLAGS)
else
    CXXFLAGS += $(DEBUG_FLAGS)
endif

# Create required directories if they don't exist
$(shell mkdir -p $(OBJ_DIR) $(BIN_DIR))

# Default target
all: $(TARGET)

# Linking object files into the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compiling each .cpp file into an object file
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)/*.o $(TARGET)

.PHONY: all clean

