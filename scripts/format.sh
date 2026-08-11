echo "Formatting all .hpp, .cpp., .h, and .c files in src/"
find src/ -type f  -iname '*.hpp' -o -iname '*.cpp' -o -iname '*.h' -o -iname '*.c' | xargs clang-format -i
echo "Formatting all .hpp, .cpp., .h, and .c files in tests/"
find tests/ -type f  -iname '*.hpp' -o -iname '*.cpp' -o -iname '*.h' -o -iname '*.c' | xargs clang-format -i
echo "Formatting all .hpp, .cpp., .h, and .c files in benches/"
find benches/ -type f  -iname '*.hpp' -o -iname '*.cpp' -o -iname '*.h' -o -iname '*.c' | xargs clang-format -i
echo "Formatting all .hpp, .cpp., .h, and .c files in comparison/ and tools/"
find comparison/ tools/ -type d -name '.*' -prune -o \
     -type f \( -iname '*.hpp' -o -iname '*.cpp' -o -iname '*.h' -o -iname '*.c' \) -print | xargs -r clang-format -i
