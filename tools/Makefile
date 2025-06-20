generate: generator.cpp
	mkdir -p build
	clang++-10 -std=c++17 -I/usr/lib/llvm-10/include -Wall -lclang-cpp -lLLVM $< -o build/$@