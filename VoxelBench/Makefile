CXX=g++
CXXFLAGS=-O2 -march=native -fopenmp -std=c++17 -Wall -Wno-unused-variable
SRC=pipeline/core.cpp pipeline/scenes_town.cpp pipeline/scenes_city.cpp pipeline/accel2.cpp pipeline/harness.cpp
HDR=$(wildcard pipeline/*.h pipeline/techniques/*.h)

ACCEL_SRC=pipeline/core.cpp pipeline/scenes_town.cpp pipeline/scenes_city.cpp pipeline/accel.cpp pipeline/accel2.cpp pipeline/bench_accel.cpp

bench: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(SRC) -o bench

bench_accel: $(ACCEL_SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(ACCEL_SRC) -o bench_accel

clean:
	rm -f bench bench_accel
