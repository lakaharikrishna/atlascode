CXX := g++
BUILD ?= release

ifeq ($(BUILD), debug)
CXXFLAGS :=  -g -O2 -Wall -Wextra -pthread -rdynamic
else
CXXFLAGS := -g -O2 -Wall -Wextra -pthread -rdynamic
endif

LDFLAGS := ./lib/libnlohmann.a -lmysqlclient -lmosquitto -lssl -lcrypto -lpthread -lm -ldl -lstdc++fs

SRC_DIRS := src
OBJ_DIR := obj
TARGET := FINAL_HES_NEW_DYNAMIC_MERGE

# Recursively find all .cpp source files
SRC_FILES := $(wildcard $(addsuffix /*.cpp, $(SRC_DIRS)))

# Replace "src/foo.cpp" with "obj/src/foo.o"
OBJ_FILES := $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SRC_FILES))

# Target
all: $(TARGET)

$(TARGET): $(OBJ_FILES)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Pattern rule to build each object file
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean