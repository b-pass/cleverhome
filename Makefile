CXX = distcc g++
LD = g++

CPP_FILES := $(wildcard *.cpp)
OBJ_FILES = $(patsubst %.cpp,obj/%.o,$(CPP_FILES))

LD_FLAGS := -lopenzwave
CXX_FLAGS := -MMD -std=gnu++11 -g3 -ggdb -O0

# release mode
# CXX_FLAGS += -O3 -DNDEBUG

cleverhomed: $(OBJ_FILES)
	$(LD) $(LD_FLAGS) -o $@ $^

-include $(OBJ_FILES:.o=.d)

obj/%.o: %.cpp
	$(CXX) $(CXX_FLAGS) -c $< -o $@

clean:
	rm -f cleverhomed
	rm -rf obj
	mkdir obj

install: cleverhomed
	cp -f cleverhomed /usr/bin/

